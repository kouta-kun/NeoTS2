// Builds a lot's scene the way the game does, with no window, and says what it holds, how long each step took
// and how much memory the process had after it. With --stub-pixels every texture is one white pixel, which shows what
// a scene costs apart from its pictures.
#include "../../app/lot_light.hpp"
#include "content/install.hpp"
#include "content/lot.hpp"
#include "diag/memory.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"
#include "scene/scene_memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

void report(const char* step, Clock::time_point since) {
    const ts2::diag::MemoryUse use = ts2::diag::memoryUse();
    constexpr double kMiB = 1024.0 * 1024.0;
    std::printf("%-26s %6.2f s   heap %6.0f MiB   mapped %5.0f MiB   peak %6.0f MiB\n", step,
                std::chrono::duration<double>(Clock::now() - since).count(), static_cast<double>(use.anonymous) / kMiB,
                static_cast<double>(use.mapped) / kMiB, static_cast<double>(use.peak) / kMiB);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: lotscene <install-folder> <lot.package> [--stub-pixels] [--no-lighting]\n");
        return 1;
    }
    bool stub = false, lighting = true;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        stub = stub || arg == "--stub-pixels";
        lighting = lighting && arg != "--no-lighting";
    }
    try {
        const std::filesystem::path install = argv[1], file = argv[2];
        const auto start = Clock::now();
        report("start", start);
        ts2::scene::Assets assets(install);
        assets.stubPixels = stub;
        report("assets loaded", start);
        ts2::scene::SceneBuilder builder(assets);
        const auto lot = ts2::content::loadLot(file);
        ts2::scene::addLot(builder, lot, file);
        report("lot added to the scene", start);
        if (lighting) {
            const std::string state = app::savedLightingState(assets.lighting(), lot);
            const auto light = app::lightLot(assets, builder.scene(), lot, file, state);
            report("lot lit", start);
        }
        const ts2::scene::Scene scene = std::move(builder.scene());
        report("scene moved out", start);
        std::printf("\nscene holds %s\n", ts2::scene::describeMemory(scene).c_str());
        std::printf("geometry: %s\n", ts2::scene::describeGeometry(scene).c_str());
        // The biggest pictures left in the scene, and a material that uses each.
        std::vector<std::size_t> order(scene.images.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return scene.images[a].rgba.size() > scene.images[b].rgba.size(); });
        std::printf("biggest images:\n");
        for (std::size_t k = 0; k < order.size() && k < 8; ++k) {
            const auto& image = scene.images[order[k]];
            std::string user = "(no material)";
            for (std::size_t m = 0; m < scene.materials.size(); ++m) {
                if (scene.materials[m].image == static_cast<int>(order[k])) {
                    user = "material " + std::to_string(m);
                    break;
                }
            }
            std::printf("  %4ux%-4u %7.1f KiB   %s\n", image.width, image.height, static_cast<double>(image.rgba.size()) / 1024, user.c_str());
        }
        std::printf("asset warnings: %zu\n", assets.warnings.size());
        for (std::size_t i = 0; i < assets.warnings.size() && i < 5; ++i) {
            std::printf("  %s\n", assets.warnings[i].c_str());
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "failed: %s\n", error.what());
        return 1;
    }
    return 0;
}

// Bakes a lot's scene for the console: builds it as the game does (lit, with its light maps), converts its textures
// and writes it as one file (render/console_scene.hpp), then reads it back and checks it.
#include "../../app/lot_light.hpp"
#include "content/lot.hpp"
#include "render/console_scene.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    ts2::render::ConvertOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-side" && i + 1 < argc) {
            options.maxSide = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (a == "--palette-error" && i + 1 < argc) {
            options.paletteError = std::stod(argv[++i]);
        } else {
            args.push_back(a);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "Usage: ts2bake <install-folder> <lot.package> <out.cscene> [--max-side N] [--palette-error E]\n");
        return 1;
    }
    try {
        const auto start = std::chrono::steady_clock::now();
        ts2::scene::Assets assets(args[0]);
        ts2::scene::SceneBuilder builder(assets);
        const auto lot = ts2::content::loadLot(args[1]);
        ts2::scene::addLot(builder, lot, args[1]);
        const std::string state = app::savedLightingState(assets.lighting(), lot);
        const auto light = app::lightLot(assets, builder.scene(), lot, args[1], state);
        (void)light;
        const ts2::scene::Scene& scene = builder.scene();
        std::printf("scene built: %zu images, %zu materials, %zu meshes, %zu instances\n", scene.images.size(), scene.materials.size(),
                    scene.meshes.size(), scene.instances.size());
        ts2::render::BakeStats stats;
        const auto bytes = ts2::render::bakeScene(scene, options, &stats);
        std::ofstream(args[2], std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::printf("baked %.2f MiB: images %.2f MiB (4-bit %zu, 8-bit %zu, 32-bit %zu), meshes %.2f MiB (%zu primitives, %zu triangles), rest %.2f MiB\n",
                    double(bytes.size()) / 1048576.0, double(stats.imageBytes) / 1048576.0, stats.byBits[0], stats.byBits[1], stats.byBits[2],
                    double(stats.meshBytes) / 1048576.0, stats.primitives, stats.triangles, double(stats.otherBytes) / 1048576.0);
        const ts2::scene::Scene back = ts2::render::readBakedScene(bytes);
        const bool same = back.images.size() == scene.images.size() && back.materials.size() == scene.materials.size() &&
                          back.meshes.size() == scene.meshes.size() && back.instances.size() == scene.instances.size();
        std::printf("read back: %zu images, %zu materials, %zu meshes, %zu instances: %s (%.1f s)\n", back.images.size(), back.materials.size(),
                    back.meshes.size(), back.instances.size(), same ? "same counts" : "COUNTS DIFFER",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        return same ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}

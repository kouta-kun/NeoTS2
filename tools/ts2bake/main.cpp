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
        } else if (a == "--mesh-keep" && i + 1 < argc) {
            options.meshKeep = std::stod(argv[++i]);
        } else if (a == "--mesh-error" && i + 1 < argc) {
            options.meshError = std::stod(argv[++i]);
        } else if (a == "--palette-error" && i + 1 < argc) {
            options.paletteError = std::stod(argv[++i]);
        } else {
            args.push_back(a);
        }
    }
    if (args.size() != 3) {
        std::fprintf(stderr, "Usage: ts2bake <install-folder> <lot.package> <out.cscene> [--max-side N] [--palette-error E] [--mesh-keep K [--mesh-error F]]\n");
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
        std::printf("baked %.2f MiB: images %.2f MiB (4-bit %zu, 8-bit %zu, 32-bit %zu), meshes %.2f MiB (%zu primitives, %zu triangles from %zu), rest %.2f MiB\n",
                    double(bytes.size()) / 1048576.0, double(stats.imageBytes) / 1048576.0, stats.byBits[0], stats.byBits[1], stats.byBits[2],
                    double(stats.meshBytes) / 1048576.0, stats.primitives, stats.triangles, stats.trianglesBefore, double(stats.otherBytes) / 1048576.0);
        const ts2::scene::Scene back = ts2::render::readBakedScene(bytes);
        const bool same = back.images.size() == scene.images.size() && back.materials.size() == scene.materials.size() &&
                          back.meshes.size() == scene.meshes.size() && back.instances.size() == scene.instances.size();
        std::printf("read back: %zu images, %zu materials, %zu meshes, %zu instances: %s (%.1f s)\n", back.images.size(), back.materials.size(),
                    back.meshes.size(), back.instances.size(), same ? "same counts" : "COUNTS DIFFER",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        // The compact form (what the console reads) must expand to what the full read gives, primitive by primitive.
        ts2::render::BakedMeshes compact;
        std::vector<std::uint8_t> head(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(ts2::render::bakedHeadSize(bytes)));
        const ts2::scene::Scene shell = ts2::render::readBakedHeadCompact(std::move(head), compact);
        std::size_t checked = 0, wrong = 0;
        ts2::render::ExpandedPrimitive expanded;
        std::vector<float> reference;
        for (std::size_t m = 0; m < back.meshes.size() && m < compact.meshes.size(); ++m) {
            for (std::size_t k = 0; k < back.meshes[m].primitives.size() && k < compact.meshes[m].size(); ++k) {
                const auto& full = back.meshes[m].primitives[k];
                ts2::render::expandPrimitive(compact, compact.meshes[m][k], expanded);
                reference.resize(full.positions.size() / 3 * ts2::render::kVertexFloats);
                ts2::render::packPrimitiveVertices(full, reference.data());
                ++checked;
                if (expanded.vertices != reference || expanded.indices != full.indices || compact.meshes[m][k].vertices != full.positions.size() / 3 ||
                    compact.meshes[m][k].indices != full.indices.size()) {
                    ++wrong;
                }
            }
        }
        // Each image must sit where the head says, on a 16-byte boundary, and parse as a console texture.
        std::size_t badImages = 0;
        for (const auto& image : shell.images) {
            if (image.consoleOffset % 16 != 0 || image.consoleOffset + image.consoleSize > bytes.size()) {
                ++badImages;
                continue;
            }
            try {
                const auto view = ts2::content::ConsoleTextureView::of(std::span<const std::uint8_t>(bytes).subspan(image.consoleOffset, image.consoleSize));
                badImages += view.width != image.width || view.height != image.height;
            } catch (const std::exception&) {
                ++badImages;
            }
        }
        std::printf("images in the file: %zu checked, %zu wrong\n", shell.images.size(), badImages);
        std::printf("compact read: %zu meshes, %zu primitives checked, %zu differ; scene part holds %zu meshes (should be 0)\n", compact.meshes.size(), checked,
                    wrong, shell.meshes.size());
        return same && wrong == 0 && badImages == 0 && compact.meshes.size() == back.meshes.size() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}

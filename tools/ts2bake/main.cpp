// Bakes a lot's scene for the console: builds it as the game does (lit, with its light maps), converts its textures
// and writes it as one file (render/console_scene.hpp), then reads it back and checks it.
#include "../../app/lot_light.hpp"
#include "content/lot.hpp"
#include "render/console_scene.hpp"
#include "formats/resource_types.hpp"
#include "scene/view.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    ts2::render::ConvertOptions options;
    bool withMarkers = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-side" && i + 1 < argc) {
            options.maxSide = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (a == "--markers") {
            withMarkers = true;
        } else if (a == "--no-weld") {
            options.weld = false;
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
        {
            ts2::scene::Scene& editable = builder.scene();
            // The floor's height under every tile of every storey, for a console that has no lot terrain to ask (Scene::floorGrid).
            const int ground = ts2::scene::groundLevel(editable), top = ts2::scene::topLevel(editable);
            const int storeys = top - ground + 1;
            const int width = static_cast<int>(std::lround(editable.lotSize[0])), height = static_cast<int>(std::lround(editable.lotSize[1]));
            if (editable.floorHeight && storeys > 0 && width > 0 && height > 0) {
                editable.floorGridStoreys = storeys;
                editable.floorGrid.reserve(static_cast<std::size_t>(storeys) * static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
                for (int storey = 1; storey <= storeys; ++storey) {
                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            editable.floorGrid.push_back(editable.floorHeight(storey, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f));
                        }
                    }
                }
            }
            // The Sims' own models are in the scene already, one instance for each of a Sim's meshes (hair, head, body), named "<Sim's id> <mesh>"; filing them
            // under that object id lets the console move them with the simulation. With --markers a post as tall as a Sim is added for each Sim as well.
            std::size_t simInstances = 0;
            for (auto& instance : editable.instances) {
                if (instance.lightGroup != ts2::scene::LightGroup::Sims || instance.object != 0) {
                    continue;
                }
                std::size_t digits = 0;
                while (digits < instance.name.size() && instance.name[digits] >= '0' && instance.name[digits] <= '9') {
                    ++digits;
                }
                if (digits > 0 && digits < instance.name.size() && instance.name[digits] == ' ') {
                    instance.object = static_cast<std::uint32_t>(std::stoul(instance.name.substr(0, digits)));
                    ++simInstances;
                }
            }
            // The console cannot skin yet: the Sims' meshes are drawn as they are in the bind pose, as plain meshes, with no joints or weights.
            for (auto& mesh : editable.meshes) {
                for (auto& primitive : mesh.primitives) {
                    primitive.joints.clear();
                    primitive.weights.clear();
                }
                mesh.skin.clear();
            }
            std::size_t markers = 0;
            if (withMarkers) {
                const int material = builder.colorMaterial("sim marker", {0.95f, 0.25f, 0.2f});
                ts2::scene::Mesh marker;
                marker.name = "sim marker";
                ts2::scene::Primitive& post = marker.primitives.emplace_back();
                post.material = material;
                constexpr float kHalf = 0.15f, kHeight = 0.9f;
                // Six faces of four vertices: position, normal.
                const float faces[6][4][3] = {{{-kHalf, -kHalf, 0}, {kHalf, -kHalf, 0}, {kHalf, kHalf, 0}, {-kHalf, kHalf, 0}},
                                              {{-kHalf, -kHalf, kHeight}, {-kHalf, kHalf, kHeight}, {kHalf, kHalf, kHeight}, {kHalf, -kHalf, kHeight}},
                                              {{-kHalf, -kHalf, 0}, {-kHalf, -kHalf, kHeight}, {kHalf, -kHalf, kHeight}, {kHalf, -kHalf, 0}},
                                              {{kHalf, kHalf, 0}, {kHalf, kHalf, kHeight}, {-kHalf, kHalf, kHeight}, {-kHalf, kHalf, 0}},
                                              {{-kHalf, kHalf, 0}, {-kHalf, kHalf, kHeight}, {-kHalf, -kHalf, kHeight}, {-kHalf, -kHalf, 0}},
                                              {{kHalf, -kHalf, 0}, {kHalf, -kHalf, kHeight}, {kHalf, kHalf, kHeight}, {kHalf, kHalf, 0}}};
                const float normals[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
                for (int f = 0; f < 6; ++f) {
                    const auto first = static_cast<std::uint32_t>(post.positions.size() / 3);
                    for (int v = 0; v < 4; ++v) {
                        post.positions.insert(post.positions.end(), faces[f][v], faces[f][v] + 3);
                        post.normals.insert(post.normals.end(), normals[f], normals[f] + 3);
                        post.uvs.push_back(v == 1 || v == 2 ? 1.0f : 0.0f);
                        post.uvs.push_back(v >= 2 ? 1.0f : 0.0f);
                    }
                    for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
                        post.indices.push_back(first + i);
                    }
                }
                const int markerMesh = static_cast<int>(editable.meshes.size());
                editable.meshes.push_back(std::move(marker));
                for (const auto& object : lot.objects) {
                    if (object.resourceType != ts2::types::kSavedPerson) {
                        continue;
                    }
                    const auto& header = object.state.header;
                    const float lotX = header.location[1], lotY = header.location[0];  // (a position's x runs along the lot's y)
                    const int storey = std::max(1, static_cast<int>(header.value1));
                    ts2::scene::Instance instance;
                    instance.mesh = markerMesh;
                    instance.name = "sim-marker";
                    instance.object = object.id;
                    instance.part = ts2::scene::Part::Object;
                    instance.level = ground + storey - 1;
                    instance.lightGroup = ts2::scene::LightGroup::Sims;
                    instance.transform = ts2::scene::Transform::at(lotX, lotY, editable.floorHeight ? editable.floorHeight(storey, lotX, lotY) : 0.0f, 0.0f);
                    editable.instances.push_back(std::move(instance));
                    ++markers;
                }
            }
            std::printf("Sims: %zu instances filed under their object ids\n", simInstances);
            std::printf("floor grid: %d storeys, %zu heights; %zu Sim markers\n", editable.floorGridStoreys, editable.floorGrid.size(), markers);
        }
        const ts2::scene::Scene& scene = builder.scene();
        std::printf("scene built: %zu images, %zu materials, %zu meshes, %zu instances\n", scene.images.size(), scene.materials.size(),
                    scene.meshes.size(), scene.instances.size());
        ts2::render::BakeStats stats;
        const auto bytes = ts2::render::bakeScene(scene, options, &stats);
        std::ofstream(args[2], std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::printf("baked %.2f MiB: images %.2f MiB (4-bit %zu, 8-bit %zu, 32-bit %zu), meshes %.2f MiB (%zu primitives, %zu triangles from %zu), rest %.2f MiB\n",
                    double(bytes.size()) / 1048576.0, double(stats.imageBytes) / 1048576.0, stats.byBits[0], stats.byBits[1], stats.byBits[2],
                    double(stats.meshBytes) / 1048576.0, stats.primitives, stats.triangles, stats.trianglesBefore, double(stats.otherBytes) / 1048576.0);
        std::printf("vertices: %zu from %zu (%.2f a triangle, was %.2f)\n", stats.vertices, stats.verticesBefore,
                    double(stats.vertices) / double(std::max<std::size_t>(1, stats.triangles)),
                    double(stats.verticesBefore) / double(std::max<std::size_t>(1, stats.trianglesBefore)));
        const ts2::scene::Scene back = ts2::render::readBakedScene(bytes);
        const bool same = back.images.size() == scene.images.size() && back.materials.size() == scene.materials.size() &&
                          back.meshes.size() == scene.meshes.size() && back.instances.size() == scene.instances.size();
        std::printf("read back: %zu images, %zu materials, %zu meshes, %zu instances: %s (%.1f s)\n", back.images.size(), back.materials.size(),
                    back.meshes.size(), back.instances.size(), same ? "same counts" : "COUNTS DIFFER",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        // The compact form (what the console reads) must expand to what the full read gives, primitive by primitive.
        ts2::render::BakedMeshes compact;
        ts2::render::AlignedBytes head(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(ts2::render::bakedHeadSize(bytes)));
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
                // The view must give the same corners as the full read: the padding repeats the last corner, and 16-byte alignment holds.
                ts2::render::PrimitiveView view;
                ts2::render::viewPrimitive(compact, compact.meshes[m][k], view);
                const std::size_t real = full.positions.size() / 3;
                bool viewOk = view.corners >= real && view.corners % 12 == 0 && reinterpret_cast<std::uintptr_t>(view.positions) % 16 == 0 &&
                              reinterpret_cast<std::uintptr_t>(view.normals) % 16 == 0 && (view.uvs == nullptr || reinterpret_cast<std::uintptr_t>(view.uvs) % 16 == 0);
                for (std::size_t i = 0; viewOk && i < real * 3; ++i) {
                    const float lo = view.lo[i % 3], hi = view.hi[i % 3];
                    viewOk = std::abs(lo + (hi - lo) * (static_cast<float>(view.positions[(i / 3) * 4 + i % 3]) / 65535.0f) - full.positions[i]) < 1e-5f * (1 + std::abs(hi - lo));
                }
                wrong += viewOk ? 0 : 1;
                if (expanded.vertices != reference || expanded.indices != full.indices || compact.meshes[m][k].vertices < full.positions.size() / 3 ||
                    compact.meshes[m][k].vertices % 12 != 0) {
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

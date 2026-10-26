// What a baked scene holds, by what each instance is: cscenestat <scene.cscene>
// Prints the instances' counts by part and storey, with the first names of each, and the scene's storeys.
#include "render/console_scene.hpp"
#include "scene/view.hpp"

#include <cstdio>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: cscenestat <scene.cscene>\n");
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    const ts2::scene::Scene scene = ts2::render::readBakedScene(bytes);
    std::printf("%zu instances; ground level %d, top level %d, lot %g x %g\n", scene.instances.size(), ts2::scene::groundLevel(scene), ts2::scene::topLevel(scene),
                scene.lotSize[0], scene.lotSize[1]);
    std::map<std::pair<int, int>, std::pair<std::size_t, std::string>> groups;  // part, level -> count, an example
    for (const auto& i : scene.instances) {
        auto& g = groups[{static_cast<int>(i.part), i.level}];
        ++g.first;
        if (g.second.empty()) {
            g.second = i.name + " (mesh " + std::to_string(i.mesh) + ")";
        }
    }
    // The Sims' own models, if the scene has them: instances lit as Sims.
    std::size_t sims = 0;
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const auto& inst = scene.instances[i];
        if (inst.lightGroup == ts2::scene::LightGroup::Sims && inst.name != "sim-marker") {
            ++sims;
            if (sims <= 6) {
                std::printf("  Sim-lit instance %zu: object %u, mesh %d, level %d, at (%.1f %.1f %.1f) name '%s'\n", i, inst.object, inst.mesh, inst.level,
                            static_cast<double>(inst.transform.m[3]), static_cast<double>(inst.transform.m[7]), static_cast<double>(inst.transform.m[11]), inst.name.c_str());
                if (inst.mesh >= 0 && static_cast<std::size_t>(inst.mesh) < scene.meshes.size()) {
                    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                    std::size_t triangles = 0, skinned = 0;
                    for (const auto& p : scene.meshes[static_cast<std::size_t>(inst.mesh)].primitives) {
                        triangles += p.indices.size() / 3;
                        skinned += !p.joints.empty();
                        for (std::size_t v = 0; v + 2 < p.positions.size(); v += 3) {
                            for (int k = 0; k < 3; ++k) {
                                lo[k] = std::min(lo[k], p.positions[v + static_cast<std::size_t>(k)]);
                                hi[k] = std::max(hi[k], p.positions[v + static_cast<std::size_t>(k)]);
                            }
                        }
                    }
                    for (const auto& p : scene.meshes[static_cast<std::size_t>(inst.mesh)].primitives) {
                        const ts2::scene::Material* m = p.material >= 0 ? &scene.materials[static_cast<std::size_t>(p.material)] : nullptr;
                        unsigned colorLo = 255, colorHi = 0;
                        for (const auto& c : p.colors) {
                            colorLo = std::min<unsigned>(colorLo, static_cast<unsigned>(c * 255.0f + 0.5f));
                            colorHi = std::max<unsigned>(colorHi, static_cast<unsigned>(c * 255.0f + 0.5f));
                        }
                        std::printf("      vertex colours: %zu values, %u..%u; normals %zu, uvs %zu, alphas %zu, tangents %zu\n", p.colors.size(), colorLo, colorHi, p.normals.size(), p.uvs.size(),
                                    p.alphas.size(), p.tangents.size());
                        std::printf("      primitive: %zu triangles, material %d '%s' alpha %d image %d (%ux%u, console %u bytes) simLighting %d unlit %d doubleSided %d color %.2f %.2f %.2f\n",
                                    p.indices.size() / 3, p.material, m ? m->name.c_str() : "", m ? static_cast<int>(m->alpha) : -1, m ? m->image : -1,
                                    m && m->image >= 0 ? scene.images[static_cast<std::size_t>(m->image)].width : 0u,
                                    m && m->image >= 0 ? scene.images[static_cast<std::size_t>(m->image)].height : 0u,
                                    m && m->image >= 0 ? scene.images[static_cast<std::size_t>(m->image)].consoleSize : 0u, m ? m->simLighting : 0, m ? m->unlit : 0,
                                    m ? m->doubleSided : 0, m ? static_cast<double>(m->color[0]) : 0.0, m ? static_cast<double>(m->color[1]) : 0.0,
                                    m ? static_cast<double>(m->color[2]) : 0.0);
                    }
                    std::printf("    %zu triangles (%zu skinned primitives), bounds x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f; transform row0 %.2f %.2f %.2f row1 %.2f %.2f %.2f row2 %.2f %.2f %.2f\n",
                                triangles, skinned, static_cast<double>(lo[0]), static_cast<double>(hi[0]), static_cast<double>(lo[1]), static_cast<double>(hi[1]),
                                static_cast<double>(lo[2]), static_cast<double>(hi[2]), static_cast<double>(inst.transform.m[0]), static_cast<double>(inst.transform.m[1]),
                                static_cast<double>(inst.transform.m[2]), static_cast<double>(inst.transform.m[4]), static_cast<double>(inst.transform.m[5]),
                                static_cast<double>(inst.transform.m[6]), static_cast<double>(inst.transform.m[8]), static_cast<double>(inst.transform.m[9]),
                                static_cast<double>(inst.transform.m[10]));
                }
            }
        }
    }
    std::printf("%zu instances lit as Sims (besides the markers)\n", sims);
    if (scene.floorHeight) {
        for (const auto& spot : {std::pair<float, float>{21.5f, 24.5f}, {12.5f, 20.5f}}) {
            std::printf("floor heights at (%.1f %.1f) by storey:", static_cast<double>(spot.first), static_cast<double>(spot.second));
            for (int storey = 1; storey <= scene.floorGridStoreys; ++storey) {
                std::printf(" %d: %.2f", storey, static_cast<double>(scene.floorHeight(storey, spot.first, spot.second)));
            }
            std::printf("\n");
        }
    }
    static const char* names[] = {"Other", "Object", "Floor", "Roof", "Wall", "WallHub", "Skirt"};
    for (const auto& [key, g] : groups) {
        std::printf("part %-8s level %2d: %5zu  e.g. %s\n", names[key.first], key.second, g.first, g.second.c_str());
    }
    return 0;
}

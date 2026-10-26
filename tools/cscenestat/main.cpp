// What a baked scene holds, by what each instance is: cscenestat <scene.cscene>
// Prints the instances' counts by part and storey, with the first names of each, and the scene's storeys.
#include "render/console_scene.hpp"
#include "scene/view.hpp"

#include <cstdio>
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
    static const char* names[] = {"Other", "Object", "Floor", "Roof", "Wall", "WallHub", "Skirt"};
    for (const auto& [key, g] : groups) {
        std::printf("part %-8s level %2d: %5zu  e.g. %s\n", names[key.first], key.second, g.first, g.second.c_str());
    }
    return 0;
}

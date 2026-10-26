// What a lot's Sims cost to draw as static models: simstat <install-folder> <lot.package>
// Builds the lot's scene as the game does and, for each Sim, counts the triangles and vertices of its skinned instances and the textures their materials use,
// with what those textures take in VRAM at several size caps (4-bit palettised: a half byte a texel and a 64-byte palette; 8-bit: a byte a texel and a
// kilobyte of palette; 32-bit: four bytes), a texture counted once however many Sims share it.
#include "content/lot.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

namespace {

std::size_t texelBytes(std::uint32_t width, std::uint32_t height, unsigned cap, int bits) {
    while (width > cap || height > cap) {
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    const std::size_t texels = std::size_t{width} * height;
    return bits == 4 ? texels / 2 + 64 : bits == 8 ? texels + 1024 : texels * 4;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: simstat <install-folder> <lot.package>\n");
        return 1;
    }
    ts2::scene::Assets assets(argv[1]);
    ts2::scene::SceneBuilder builder(assets);
    const auto lot = ts2::content::loadLot(argv[2]);
    ts2::scene::addLot(builder, lot, argv[2]);
    const ts2::scene::Scene& scene = builder.scene();
    std::printf("%zu actors in the scene\n", scene.actors.size());
    std::set<int> allImages;
    std::size_t allTriangles = 0, allVertices = 0;
    for (const auto& actor : scene.actors) {
        std::size_t triangles = 0, vertices = 0, primitives = 0;
        std::set<int> images;
        for (const auto& skinned : actor.skinned) {
            const auto& instance = scene.instances[skinned.instance];
            if (instance.mesh < 0) {
                continue;
            }
            for (const auto& p : scene.meshes[static_cast<std::size_t>(instance.mesh)].primitives) {
                triangles += p.indices.size() / 3;
                vertices += p.positions.size() / 3;
                ++primitives;
                if (p.material >= 0) {
                    const auto& m = scene.materials[static_cast<std::size_t>(p.material)];
                    if (m.image >= 0) {
                        images.insert(m.image);
                    }
                }
            }
        }
        std::printf("Sim %u (age %u, gender %u): %zu skinned instances, %zu primitives, %zu triangles, %zu vertices, %zu textures:", actor.object, actor.age, actor.gender,
                    actor.skinned.size(), primitives, triangles, vertices, images.size());
        for (const int i : images) {
            const auto& im = scene.images[static_cast<std::size_t>(i)];
            std::printf(" %ux%u", im.width, im.height);
            allImages.insert(i);
        }
        std::printf("\n");
        allTriangles += triangles;
        allVertices += vertices;
    }
    std::printf("all Sims: %zu triangles, %zu vertices, %zu distinct textures\n", allTriangles, allVertices, allImages.size());
    for (const unsigned cap : {256u, 128u, 64u, 32u, 16u}) {
        std::size_t four = 0, eight = 0, full = 0;
        for (const int i : allImages) {
            const auto& im = scene.images[static_cast<std::size_t>(i)];
            four += texelBytes(im.width, im.height, cap, 4);
            eight += texelBytes(im.width, im.height, cap, 8);
            full += texelBytes(im.width, im.height, cap, 32);
        }
        std::printf("cap %3u px: 4-bit %7.1f KiB, 8-bit %7.1f KiB, 32-bit %7.1f KiB\n", cap, four / 1024.0, eight / 1024.0, full / 1024.0);
    }
    return 0;
}

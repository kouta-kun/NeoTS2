#include "scene/scene_memory.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <numeric>

namespace ts2::scene {

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

std::size_t imageBytes(const Image& image) {
    std::size_t bytes = image.rgba.size();
    for (const auto& level : image.smallerLevels) {
        bytes += level.size();
    }
    return bytes;
}

std::size_t imagesBytes(const std::vector<Image>& images) {
    std::size_t bytes = 0;
    for (const auto& image : images) {
        bytes += imageBytes(image);
    }
    return bytes;
}

std::size_t primitiveBytes(const Primitive& p) {
    return (p.positions.size() + p.normals.size() + p.uvs.size() + p.alphas.size() + p.tangents.size() + p.colors.size() + p.lightUvs.size() +
            p.weights.size()) * sizeof(float) +
           p.indices.size() * sizeof(std::uint32_t) + p.joints.size() + p.group.size();
}

} // namespace

std::string describeMemory(const Scene& scene) {
    std::size_t images = imagesBytes(scene.images) + imagesBytes(scene.skybox);
    for (const auto& cube : scene.envCubes) {
        images += imagesBytes(cube);
    }
    std::size_t meshes = 0, primitives = 0, vertices = 0, indices = 0;
    for (const auto& mesh : scene.meshes) {
        for (const auto& p : mesh.primitives) {
            meshes += primitiveBytes(p);
            ++primitives;
            vertices += p.positions.size() / 3;
            indices += p.indices.size();
        }
    }
    std::size_t terrain = 0, terrainImages = 0, imposterBytes = 0;
    if (scene.terrain) {
        const PaintedTerrain& t = *scene.terrain;
        terrainImages = imagesBytes(t.textures) + imagesBytes(t.roadTextures) + imagesBytes(t.imposterTextures) + imagesBytes(t.sky) +
                        imageBytes(t.lightMap) + imageBytes(t.ripples) + imageBytes(t.underwaterGradient);
        for (const auto& part : t.imposters) {
            imposterBytes += part.vertices.size() * sizeof(float) + part.indices.size() * sizeof(std::uint32_t);
        }
        terrain = (t.heights.size() + t.normals.size()) * sizeof(float) + t.alphas.size() + t.cellTextures.size() + t.waterTiles.size() + t.hiddenTiles.size() +
                  t.roads.size() * sizeof(PaintedTerrain::RoadQuad);
    }
    return std::format(
        "{:.0f} MiB of images ({} of them, with their smaller levels), {:.0f} MiB of meshes ({} primitives, {} vertices, {} indices), terrain {:.0f} MiB of "
        "pictures and {:.0f} MiB of grids and imposters, {} instances ({:.1f} MiB), {} materials, {} actors",
        static_cast<double>(images) / kMiB, scene.images.size(), static_cast<double>(meshes) / kMiB, primitives, vertices, indices,
        static_cast<double>(terrainImages) / kMiB, static_cast<double>(terrain + imposterBytes) / kMiB, scene.instances.size(),
        static_cast<double>(scene.instances.size() * sizeof(Instance)) / kMiB, scene.materials.size(), scene.actors.size());
}

std::string describeGeometry(const Scene& scene) {
    struct Use {
        std::size_t instances = 0, triangles = 0;
    };
    std::vector<std::size_t> meshTriangles(scene.meshes.size(), 0), meshVertices(scene.meshes.size(), 0);
    for (std::size_t m = 0; m < scene.meshes.size(); ++m) {
        for (const auto& p : scene.meshes[m].primitives) {
            meshTriangles[m] += p.indices.size() / 3;
            meshVertices[m] += p.positions.size() / 3;
        }
    }
    static constexpr std::array<const char*, 7> kNames{"other", "object", "floor", "roof", "wall", "wall hub", "skirt"};
    std::array<Use, 7> byPart{};
    std::vector<Use> byMesh(scene.meshes.size());
    std::size_t total = 0;
    for (const auto& instance : scene.instances) {
        if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= scene.meshes.size()) {
            continue;
        }
        const auto m = static_cast<std::size_t>(instance.mesh);
        Use& part = byPart[std::min<std::size_t>(static_cast<std::size_t>(instance.part), byPart.size() - 1)];
        ++part.instances;
        part.triangles += meshTriangles[m];
        ++byMesh[m].instances;
        byMesh[m].triangles += meshTriangles[m];
        total += meshTriangles[m];
    }
    std::string parts;
    for (std::size_t i = 0; i < byPart.size(); ++i) {
        if (byPart[i].instances > 0) {
            parts += std::format("{}{} {}k triangles in {} instances", parts.empty() ? "" : "; ", kNames[i], byPart[i].triangles / 1000, byPart[i].instances);
        }
    }
    std::vector<std::size_t> order(scene.meshes.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return byMesh[a].triangles > byMesh[b].triangles; });
    std::string top;
    for (std::size_t i = 0; i < order.size() && i < 8; ++i) {
        const std::size_t m = order[i];
        top += std::format("{}{} ({} triangles x {} = {}k)", top.empty() ? "" : "; ", scene.meshes[m].name, meshTriangles[m], byMesh[m].instances,
                           byMesh[m].triangles / 1000);
    }
    return std::format("{}k triangles if every instance were in view ({} meshes, {}k triangles once each). By kind: {}. The most: {}", total / 1000,
                       scene.meshes.size(), std::accumulate(meshTriangles.begin(), meshTriangles.end(), std::size_t{0}) / 1000, parts, top);
}

} // namespace ts2::scene

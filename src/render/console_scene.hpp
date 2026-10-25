// A lot's scene baked for the console: what the converter writes after building the scene on a computer, and what the
// console reads, so that it never runs the builder. Images are console textures; meshes, materials and instances are
// stored field by field. Little-endian.
//
// What is kept: images, materials, meshes (with skins), instances, the lights, and the lot's size. What is not (yet): the
// neighborhood's terrain and imposters, sky boxes, environment cubes, actors and anything that runs code (the callbacks
// the lot scene carries to light objects and place Sims).
#pragma once

#include "render/mesh_pack.hpp"
#include "render/texture_convert.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <new>
#include <span>
#include <vector>

namespace ts2::render {

struct BakeStats {
    std::size_t images = 0, meshes = 0, primitives = 0, instances = 0, triangles = 0, trianglesBefore = 0, vertices = 0, verticesBefore = 0;
    std::size_t imageBytes = 0, meshBytes = 0, otherBytes = 0;
    std::size_t byBits[3] = {0, 0, 0};  // images stored as 4-bit, 8-bit, 32-bit
};

// The scene baked. Light maps and other pictures that change are kept at 32 bits with no smaller levels.
[[nodiscard]] std::vector<std::uint8_t> bakeScene(const scene::Scene& scene, const ConvertOptions& options, BakeStats* stats = nullptr);

// Reads a whole baked scene held in memory. Its images carry console textures (Image::console). Throws std::runtime_error on bad data.
[[nodiscard]] scene::Scene readBakedScene(std::span<const std::uint8_t> bytes);

// The file is a head (everything but the pixels) followed by the images, so a console can keep the images on its disc and read
// each when it is needed. `bakedHeadSize` says from the first 12 bytes how many bytes at the start of the file make up the head;
// `readBakedHead` reads those. The scene's images have their size and place in the file (Image::consoleOffset, consoleSize) and
// no `console`.
constexpr std::size_t kBakedPreambleSize = 16;
[[nodiscard]] std::size_t bakedHeadSize(std::span<const std::uint8_t> preamble);
[[nodiscard]] scene::Scene readBakedHead(std::span<const std::uint8_t> head);

// A console that cannot hold a scene's meshes as floats keeps them as they are in the head, one primitive at a time: what a draw
// needs to know of each without reading its vertices, and where its bytes are.
// Bytes held on a 16-byte boundary, which the console's DMA needs for what it reads straight out of the head.
template <class T>
struct Aligned16 {
    using value_type = T;
    Aligned16() = default;
    template <class U>
    Aligned16(const Aligned16<U>&) {}
    T* allocate(std::size_t n) { return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{16})); }
    void deallocate(T* p, std::size_t) { ::operator delete(p, std::align_val_t{16}); }
    template <class U>
    bool operator==(const Aligned16<U>&) const { return true; }
};
using AlignedBytes = std::vector<std::uint8_t, Aligned16<std::uint8_t>>;

struct BakedPrimitive {
    int material = -1;
    std::uint32_t vertices = 0, indices = 0;
    bool lightMapped = false, skinned = false;
    std::array<float, 3> low{}, high{};  // the box around its vertices
    std::size_t offset = 0, size = 0;    // its bytes in BakedMeshes::bytes
};

struct BakedMeshes {
    AlignedBytes bytes;  // the head, kept (what the primitives' offsets point into)
    std::vector<std::vector<BakedPrimitive>> meshes;  // by the scene's mesh index (the scene returned has none of its own)
};

// Reads a head as readBakedHead does, but leaves the meshes in `meshes` as bytes (it takes the head over). The scene it returns has
// no meshes, and its images are as readBakedHead's.
[[nodiscard]] scene::Scene readBakedHeadCompact(AlignedBytes head, BakedMeshes& meshes);

// A primitive's corners as they lie in the head: triangle soup (three corners a triangle, no indices) padded to a multiple of 12 corners with
// degenerate triangles, each array 16-byte aligned so that the VIF can unpack it straight from memory. Positions are x, y, z, 0 in 16 bits over each axis's
// range (value = lo + u / 65535 * (hi - lo)); texture coordinates u, v in 16 bits over one range; normals x, y, z, 0 as signed bytes (/127); colours r, g, b, a bytes
// (/255) or null; light-map coordinates u, v in 16 bits (/65535) or null.
struct PrimitiveView {
    std::uint32_t corners = 0;
    float lo[3]{}, hi[3]{};
    float uvLo = 0, uvHi = 0;
    const std::uint16_t* positions = nullptr;
    const std::uint16_t* uvs = nullptr;
    const std::int8_t* normals = nullptr;
    const std::uint8_t* colors = nullptr;
    const std::uint16_t* lightUvs = nullptr;
};
void viewPrimitive(const BakedMeshes& meshes, const BakedPrimitive& primitive, PrimitiveView& out);

// A primitive's vertices (kVertexFloats floats each, see mesh_pack.hpp) and indices, expanded from its bytes for drawing.
struct ExpandedPrimitive {
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
};
void expandPrimitive(const BakedMeshes& meshes, const BakedPrimitive& primitive, ExpandedPrimitive& out);

} // namespace ts2::render

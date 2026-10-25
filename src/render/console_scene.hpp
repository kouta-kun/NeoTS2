// A lot's scene baked for the console: what the converter writes after building the scene on a computer, and what the
// console reads, so that it never runs the builder. Images are console textures; meshes, materials and instances are
// stored field by field. Little-endian.
//
// What is kept: images, materials, meshes (with skins), instances, the lights, and the lot's size. What is not (yet): the
// neighborhood's terrain and imposters, sky boxes, environment cubes, actors and anything that runs code (the callbacks
// the lot scene carries to light objects and place Sims).
#pragma once

#include "render/texture_convert.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::render {

struct BakeStats {
    std::size_t images = 0, meshes = 0, primitives = 0, instances = 0, triangles = 0, trianglesBefore = 0;
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
constexpr std::size_t kBakedPreambleSize = 12;
[[nodiscard]] std::size_t bakedHeadSize(std::span<const std::uint8_t> preamble);
[[nodiscard]] scene::Scene readBakedHead(std::span<const std::uint8_t> head);

} // namespace ts2::render

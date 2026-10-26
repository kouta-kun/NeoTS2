// Making a console texture (content/console_texture.hpp) of a decoded image: resampled to a power of two within a
// size cap, then 4-bit if a 16-colour palette changes it by at most `paletteError` RMS levels, else 8-bit if 256 colours
// change it by at most half that, else 32-bit. The smaller levels follow, sharing the palette.
#pragma once

#include "content/console_texture.hpp"
#include "scene/scene.hpp"

namespace ts2::render {

struct ConvertOptions {
    unsigned maxSide = 256;
    unsigned minSide = 8;
    double paletteError = 8;
    bool dither = false;
    bool mips = true;
    bool palettes = true;  // false: always 32-bit (for pictures that change, like light maps)
    // Baked scenes only: the share of each mesh's triangles kept (1: all) and how far reducing may move a surface, as a share of
    // the primitive's size (see content/mesh_decimate.hpp).
    bool weld = true;  // identical vertices made one before anything else is done to a mesh
    double meshKeep = 1;
    // Baked scenes only: primitives (of meshes that nothing indexes by primitive, opaque or layered) with more triangles than this are cut into
    // pieces of at most that many along their longest side, so that a piece in view is drawn whole and one out of view is not drawn (0: not cut).
    std::size_t splitTriangles = 0;
    double meshError = 0.1;
};

struct ConvertedTexture {
    content::ConsoleTexture texture;
    double error = 0;  // RMS levels the palette changed it by (0 for 32-bit)
    bool resized = false;
};

[[nodiscard]] ConvertedTexture convertTexture(const scene::Image& image, const ConvertOptions& options);

// The texels of a console texture's largest level as RGBA, for looking at it.
[[nodiscard]] scene::Image decodeConsoleTexture(const content::ConsoleTexture& texture);

} // namespace ts2::render

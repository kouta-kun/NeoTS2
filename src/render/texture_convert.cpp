#include "render/texture_convert.hpp"

#include "render/texture_fit.hpp"

#include <cstring>
#include <unordered_map>

namespace ts2::render {

namespace {

using content::ConsoleTexture;

// Appends indices as texels: 4-bit ones two to a byte, the first in the low nibble.
void appendPacked(std::vector<std::uint8_t>& texels, const std::vector<std::uint8_t>& indices, unsigned bits) {
    if (bits == 8) {
        texels.insert(texels.end(), indices.begin(), indices.end());
        return;
    }
    const std::size_t start = texels.size();
    texels.resize(start + (indices.size() + 1) / 2, 0);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        texels[start + i / 2] = static_cast<std::uint8_t>(texels[start + i / 2] | (indices[i] << ((i & 1) * 4)));
    }
}

// The palette and texels of an image that has no more than 1 << bits colours. False when it has more.
bool toIndexed(const scene::Image& image, unsigned bits, ConsoleTexture& out) {
    std::unordered_map<std::uint32_t, std::uint8_t> index;
    out.palette.clear();
    const std::size_t pixels = std::size_t(image.width) * image.height;
    std::vector<std::uint8_t> indices(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint32_t c;
        std::memcpy(&c, &image.rgba[i * 4], 4);
        auto it = index.find(c);
        if (it == index.end()) {
            if (index.size() >= (1u << bits)) {
                return false;
            }
            it = index.emplace(c, static_cast<std::uint8_t>(index.size())).first;
            out.palette.insert(out.palette.end(), image.rgba.begin() + std::ptrdiff_t(i * 4), image.rgba.begin() + std::ptrdiff_t(i * 4 + 4));
        }
        indices[i] = it->second;
    }
    out.bits = bits;
    out.texels.clear();
    appendPacked(out.texels, indices, bits);
    return true;
}

std::uint8_t nearest(const std::vector<std::uint8_t>& palette, const std::uint8_t* px) {
    int best = 0;
    long bestDistance = -1;
    for (std::size_t i = 0; i < palette.size() / 4; ++i) {
        long d = 0;
        for (int c = 0; c < 4; ++c) {
            const long diff = long(palette[i * 4 + std::size_t(c)]) - px[c];
            d += diff * diff;
        }
        if (bestDistance < 0 || d < bestDistance) {
            bestDistance = d;
            best = static_cast<int>(i);
        }
    }
    return static_cast<std::uint8_t>(best);
}

// The smaller levels of `base`, each made from the picture itself, in the texture's depth and palette.
void addMips(const scene::Image& base, ConsoleTexture& out) {
    out.mips = ConsoleTexture::levelCount(out.width, out.height);
    for (unsigned level = 1; level < out.mips; ++level) {
        const scene::Image small = stretched(base, out.width >> level, out.height >> level);
        if (out.bits == 32) {
            out.texels.insert(out.texels.end(), small.rgba.begin(), small.rgba.end());
            continue;
        }
        std::vector<std::uint8_t> indices(std::size_t(small.width) * small.height);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = nearest(out.palette, &small.rgba[i * 4]);
        }
        appendPacked(out.texels, indices, out.bits);
    }
}

} // namespace

ConvertedTexture convertTexture(const scene::Image& image, const ConvertOptions& options) {
    ConvertedTexture result;
    const TextureLimits limits{options.maxSide, options.minSide};
    const unsigned tw = fitSide(image.width, limits), th = fitSide(image.height, limits);
    result.resized = tw != image.width || th != image.height;
    const scene::Image fitted = result.resized ? stretched(image, tw, th) : image;

    ConsoleTexture& out = result.texture;
    out.width = fitted.width;
    out.height = fitted.height;
    for (std::size_t i = 3; i < fitted.rgba.size(); i += 4) {
        out.hasAlpha = out.hasAlpha || fitted.rgba[i] != 255;
    }
    QuantizeOptions quant;
    quant.iterations = 3;
    quant.dither = options.dither;
    bool done = false;
    for (unsigned bits : {4u, 8u}) {
        if (!options.palettes) {
            break;
        }
        const scene::Image reduced = quantized(fitted, 1u << bits, quant);
        const double error = imageError(fitted, reduced);
        if (error <= (bits == 4 ? options.paletteError : options.paletteError / 2) && toIndexed(reduced, bits, out)) {
            result.error = error;
            done = true;
            break;
        }
    }
    if (!done) {
        out.bits = 32;
        out.palette.clear();
        out.texels = fitted.rgba;
    }
    out.mips = 1;
    if (options.mips) {
        addMips(fitted, out);
    }
    return result;
}

scene::Image decodeConsoleTexture(const ConsoleTexture& texture) {
    scene::Image image;
    image.width = texture.width;
    image.height = texture.height;
    const std::size_t pixels = std::size_t(texture.width) * texture.height;
    image.rgba.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        if (texture.bits == 32) {
            std::memcpy(&image.rgba[i * 4], &texture.texels[i * 4], 4);
            continue;
        }
        const std::size_t index = texture.bits == 8 ? texture.texels[i] : (texture.texels[i / 2] >> ((i & 1) * 4)) & 15;
        std::memcpy(&image.rgba[i * 4], &texture.palette[index * 4], 4);
    }
    return image;
}

} // namespace ts2::render

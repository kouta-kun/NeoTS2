#include "content/console_texture.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ts2::content {

namespace {

struct Header {
    std::uint32_t magic = ConsoleTexture::kMagic;
    std::uint16_t width = 0, height = 0;
    std::uint8_t bits = 0, flags = 0;
    std::uint16_t paletteSize = 0;
};
static_assert(sizeof(Header) == 12);

} // namespace

std::size_t ConsoleTexture::texelBytes(std::uint32_t width, std::uint32_t height, unsigned bits, unsigned mips) {
    std::size_t total = 0;
    for (unsigned level = 0; level < mips; ++level) {
        const std::size_t w = std::max<std::size_t>(1, width >> level), h = std::max<std::size_t>(1, height >> level);
        total += (w * h * bits + 7) / 8;
    }
    return total;
}

unsigned ConsoleTexture::levelCount(std::uint32_t width, std::uint32_t height) {
    unsigned n = 1;
    while (n < 15 && (width >> n) >= 8 && (height >> n) >= 8) {
        ++n;
    }
    return n;
}

bool ConsoleTexture::matches(std::span<const std::uint8_t> bytes) {
    std::uint32_t magic = 0;
    return bytes.size() >= sizeof magic && (std::memcpy(&magic, bytes.data(), sizeof magic), magic == kMagic);
}

std::vector<std::uint8_t> ConsoleTexture::serialize() const {
    Header h;
    h.width = static_cast<std::uint16_t>(width);
    h.height = static_cast<std::uint16_t>(height);
    h.bits = static_cast<std::uint8_t>(bits);
    h.flags = static_cast<std::uint8_t>((hasAlpha ? 1 : 0) | ((mips - 1) << 4));
    h.paletteSize = static_cast<std::uint16_t>(palette.size() / 4);
    std::vector<std::uint8_t> out(sizeof h + palette.size() + texels.size());
    std::memcpy(out.data(), &h, sizeof h);
    if (!palette.empty()) {
        std::memcpy(out.data() + sizeof h, palette.data(), palette.size());
    }
    if (!texels.empty()) {
        std::memcpy(out.data() + sizeof h + palette.size(), texels.data(), texels.size());
    }
    return out;
}

ConsoleTexture ConsoleTexture::parse(std::span<const std::uint8_t> bytes) {
    Header h;
    if (bytes.size() < sizeof h) {
        throw std::runtime_error("not a console texture: too short");
    }
    std::memcpy(&h, bytes.data(), sizeof h);
    if (h.magic != kMagic || (h.bits != 4 && h.bits != 8 && h.bits != 32)) {
        throw std::runtime_error("not a console texture: bad header");
    }
    ConsoleTexture t;
    t.width = h.width;
    t.height = h.height;
    t.bits = h.bits;
    t.hasAlpha = (h.flags & 1) != 0;
    t.mips = (h.flags >> 4) + 1u;
    const std::size_t paletteBytes = static_cast<std::size_t>(h.paletteSize) * 4;
    const std::size_t texelBytes = ConsoleTexture::texelBytes(t.width, t.height, t.bits, t.mips);
    if (sizeof h + paletteBytes + texelBytes != bytes.size()) {
        throw std::runtime_error("not a console texture: wrong size");
    }
    t.palette.assign(bytes.begin() + sizeof h, bytes.begin() + static_cast<std::ptrdiff_t>(sizeof h + paletteBytes));
    t.texels.assign(bytes.begin() + static_cast<std::ptrdiff_t>(sizeof h + paletteBytes), bytes.end());
    return t;
}

} // namespace ts2::content

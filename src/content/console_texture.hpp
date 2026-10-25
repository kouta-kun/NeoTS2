// A texture as the console takes it: a power-of-two size, and 4-bit or 8-bit palette indices (or plain 32-bit
// texels where a palette would change the picture too much). A converter makes these from the game's textures and
// stores them in a bank under the texture's own key. Little-endian.
//
// Layout: Header, then the palette (`paletteSize` RGBA entries), then the texels. 4-bit texels are packed two to a
// byte with the first in the low nibble, as the Graphics Synthesizer reads them; 8-bit are one byte each; 32-bit are
// RGBA. Rows run from the top, and each is `width` texels. Only the largest level is stored.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::content {

struct ConsoleTexture {
    static constexpr std::uint32_t kMagic = 0x31585443;  // "CTX1"

    std::uint32_t width = 0, height = 0;
    unsigned bits = 32;                  // 4, 8 or 32
    bool hasAlpha = false;
    std::vector<std::uint8_t> palette;   // RGBA, bits <= 8 only
    std::vector<std::uint8_t> texels;    // as the layout says

    // The bytes of this texture as stored in a bank.
    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    // Throws std::runtime_error when the bytes are not a console texture.
    static ConsoleTexture parse(std::span<const std::uint8_t> bytes);

    // True when the bytes begin like a console texture.
    static bool matches(std::span<const std::uint8_t> bytes);
    // The size the texels take for these properties.
    static std::size_t texelBytes(std::uint32_t width, std::uint32_t height, unsigned bits);
};

} // namespace ts2::content

#include "formats/qfs.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ts2::qfs {

namespace {

constexpr std::size_t kHeaderSize = 9;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("QFS: " + message);
}

} // namespace

bool isCompressed(std::span<const std::uint8_t> data) {
    return data.size() >= kHeaderSize && data[4] == 0x10 && data[5] == 0xFB;
}

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> in) {
    if (!isCompressed(in)) {
        fail("missing 0x10FB signature");
    }

    const std::size_t outSize = static_cast<std::size_t>(in[6]) << 16
                              | static_cast<std::size_t>(in[7]) << 8
                              | static_cast<std::size_t>(in[8]);
    std::vector<std::uint8_t> out(outSize);

    std::size_t src = kHeaderSize;
    std::size_t dst = 0;

    auto need = [&](std::size_t count) {
        if (count > in.size() - src) {
            fail("compressed data ends too early");
        }
    };

    auto copyLiteral = [&](std::size_t count) {
        need(count);
        if (count > outSize - dst) {
            fail("literal run overflows output");
        }
        std::copy_n(in.begin() + src, count, out.begin() + dst);
        src += count;
        dst += count;
    };

    auto copyBackReference = [&](std::size_t offset, std::size_t count) {
        if (offset > dst) {
            fail("back-reference points before start of output");
        }
        if (count > outSize - dst) {
            fail("back-reference overflows output");
        }
        for (std::size_t i = 0; i < count; ++i, ++dst) {
            out[dst] = out[dst - offset];
        }
    };

    while (dst < outSize) {
        need(1);
        const std::uint8_t b0 = in[src++];

        std::size_t literal = 0;
        std::size_t copy = 0;
        std::size_t offset = 0;

        if (b0 < 0x80) {
            need(1);
            const std::uint8_t b1 = in[src++];
            literal = b0 & 0x03;
            copy = ((b0 & 0x1C) >> 2) + 3;
            offset = ((b0 & 0x60) << 3) + b1 + 1;
        } else if (b0 < 0xC0) {
            need(2);
            const std::uint8_t b1 = in[src++];
            const std::uint8_t b2 = in[src++];
            literal = (b1 & 0xC0) >> 6;
            copy = (b0 & 0x3F) + 4;
            offset = ((b1 & 0x3F) << 8) + b2 + 1;
        } else if (b0 < 0xE0) {
            need(3);
            const std::uint8_t b1 = in[src++];
            const std::uint8_t b2 = in[src++];
            const std::uint8_t b3 = in[src++];
            literal = b0 & 0x03;
            copy = ((b0 & 0x0C) << 6) + b3 + 5;
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
        } else if (b0 < 0xFC) {
            literal = ((b0 & 0x1F) << 2) + 4;
        } else {
            copyLiteral(b0 & 0x03);
            break;
        }

        copyLiteral(literal);
        if (copy > 0) {
            copyBackReference(offset, copy);
        }
    }

    if (dst != outSize) {
        fail("stream ended after " + std::to_string(dst) + " of " +
             std::to_string(outSize) + " bytes");
    }
    return out;
}

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> in) {
    constexpr std::size_t kMaxSize = (1u << 24) - 1;
    constexpr std::size_t kHashBits = 15, kChain = 16;
    if (in.size() > kMaxSize) {
        fail("too large to compress");
    }
    std::vector<std::uint8_t> out(kHeaderSize);
    out[4] = 0x10;
    out[5] = 0xFB;
    out[6] = static_cast<std::uint8_t>(in.size() >> 16);
    out[7] = static_cast<std::uint8_t>(in.size() >> 8);
    out[8] = static_cast<std::uint8_t>(in.size());

    // Positions by the hash of their next three bytes, newest first through `previous`.
    std::vector<std::int32_t> head(std::size_t{1} << kHashBits, -1), previous(in.size(), -1);
    const auto hash = [&](std::size_t at) {
        return ((in[at] << 10) ^ (in[at + 1] << 5) ^ in[at + 2]) & ((1u << kHashBits) - 1);
    };
    const auto insert = [&](std::size_t at) {
        if (at + 3 <= in.size()) {
            const auto h = hash(at);
            previous[at] = head[h];
            head[h] = static_cast<std::int32_t>(at);
        }
    };

    std::size_t literalStart = 0, at = 0;
    // Writes the pending literals but their last (count % 4), which the next command carries, and returns that remainder.
    const auto flushLiterals = [&](std::size_t end) {
        std::size_t count = end - literalStart;
        while (count >= 4) {
            const std::size_t run = std::min<std::size_t>(count & ~std::size_t{3}, 112);
            out.push_back(static_cast<std::uint8_t>(0xE0 | ((run - 4) >> 2)));
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(literalStart), in.begin() + static_cast<std::ptrdiff_t>(literalStart + run));
            literalStart += run;
            count -= run;
        }
        return count;
    };
    while (at < in.size()) {
        std::size_t bestLength = 0, bestOffset = 0;
        if (at + 3 <= in.size()) {
            std::int32_t candidate = head[hash(at)];
            for (std::size_t tries = 0; candidate >= 0 && tries < kChain; ++tries, candidate = previous[static_cast<std::size_t>(candidate)]) {
                const std::size_t offset = at - static_cast<std::size_t>(candidate);
                if (offset > 131072) {
                    break;
                }
                const std::size_t limit = std::min<std::size_t>(in.size() - at, 1028);
                std::size_t length = 0;
                while (length < limit && in[static_cast<std::size_t>(candidate) + length] == in[at + length]) {
                    ++length;
                }
                if (length > bestLength) {
                    bestLength = length;
                    bestOffset = offset;
                }
            }
        }
        // The shortest command that holds the match: 2 bytes (3-10 bytes back up to 1024), 3 bytes (4-67, up to 16384), 4 bytes (5-1028).
        enum class Form { None, Two, Three, Four };
        Form form = Form::None;
        std::size_t length = bestLength;
        if (bestLength >= 3 && bestLength <= 10 && bestOffset <= 1024) {
            form = Form::Two;
        } else if (bestLength >= 4 && bestOffset <= 16384) {
            form = Form::Three;
            length = std::min<std::size_t>(bestLength, 67);
        } else if (bestLength >= 5) {
            form = Form::Four;
            length = std::min<std::size_t>(bestLength, 1028);
        }
        if (form == Form::None) {
            insert(at);
            ++at;
            continue;
        }
        const std::size_t proc = flushLiterals(at);
        const std::size_t o = bestOffset - 1;
        if (form == Form::Two) {
            out.push_back(static_cast<std::uint8_t>(((o >> 3) & 0x60) | ((length - 3) << 2) | proc));
            out.push_back(static_cast<std::uint8_t>(o));
        } else if (form == Form::Three) {
            out.push_back(static_cast<std::uint8_t>(0x80 | (length - 4)));
            out.push_back(static_cast<std::uint8_t>((proc << 6) | (o >> 8)));
            out.push_back(static_cast<std::uint8_t>(o));
        } else {
            const std::size_t c = length - 5;
            out.push_back(static_cast<std::uint8_t>(0xC0 | ((o >> 12) & 0x10) | ((c >> 6) & 0x0C) | proc));
            out.push_back(static_cast<std::uint8_t>(o >> 8));
            out.push_back(static_cast<std::uint8_t>(o));
            out.push_back(static_cast<std::uint8_t>(c));
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(literalStart), in.begin() + static_cast<std::ptrdiff_t>(literalStart + proc));
        for (std::size_t i = 0; i < length; ++i) {
            insert(at + i);
        }
        at += length;
        literalStart = at;
    }
    const std::size_t rest = flushLiterals(in.size());
    out.push_back(static_cast<std::uint8_t>(0xFC | rest));
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(literalStart), in.end());
    const auto total = out.size();
    out[0] = static_cast<std::uint8_t>(total);
    out[1] = static_cast<std::uint8_t>(total >> 8);
    out[2] = static_cast<std::uint8_t>(total >> 16);
    out[3] = static_cast<std::uint8_t>(total >> 24);
    return out;
}

} // namespace ts2::qfs

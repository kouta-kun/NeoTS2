#pragma once

#include <cstdint>
#include <span>
#include <vector>

// QFS, the compression the game uses for resources in packages.
namespace ts2::qfs {

bool isCompressed(std::span<const std::uint8_t> data);

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> data);

// Compresses `data` (up to 16 MiB) into a stream `decompress` and the game read back. A greedy search of recent matches: smaller than
// the original compressor wins nothing, but it is quick to run and to unpack.
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> data);

} // namespace ts2::qfs

// A bank: the resources a console build reads, gathered from the game's packages by a converter and stored in one
// file the console opens and reads from directly. Little-endian, every blob aligned to 16 bytes, entries sorted by key
// so a lookup is a binary search with no allocation. Blobs are stored decompressed.
#pragma once

#include "formats/dbpf.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ts2::content {

namespace bank {

constexpr std::uint32_t kMagic = 0x42535432;  // "2TSB" read as bytes: '2','T','S','B'
constexpr std::uint32_t kVersion = 1;

struct Header {
    std::uint32_t magic = kMagic;
    std::uint32_t version = kVersion;
    std::uint32_t count = 0;        // entries
    std::uint32_t tableOffset = 0;  // of the first entry, from the start of the file
};

struct Entry {
    std::uint32_t type = 0, group = 0, instance = 0, instanceHigh = 0;
    std::uint32_t offset = 0;  // of the blob, from the start of the file
    std::uint32_t size = 0;
    std::uint32_t source = 0;  // which set of packages it came from (see Source)
    std::uint32_t reserved = 0;
};

enum Source : std::uint32_t { kModels, kObjects, kCatalog, kSkins };

static_assert(sizeof(Header) == 16);
static_assert(sizeof(Entry) == 32);

} // namespace bank

// A resource to put in a bank.
struct BankResource {
    dbpf::ResourceKey key;
    std::uint32_t source = 0;
    std::vector<std::uint8_t> bytes;
};

// Writes a bank of `resources` (in any order). Returns the size of the file.
std::size_t writeBank(const std::filesystem::path& path, std::vector<BankResource> resources);

// A bank held in memory (or mapped), read in place.
class Bank {
public:
    // Throws std::runtime_error when the bytes are not a bank.
    explicit Bank(std::span<const std::uint8_t> bytes);

    std::size_t size() const { return count_; }
    // The blob of `key`, or empty when it is not there.
    std::span<const std::uint8_t> find(const dbpf::ResourceKey& key) const;

private:
    std::span<const std::uint8_t> bytes_;
    const bank::Entry* table_ = nullptr;
    std::size_t count_ = 0;
};

} // namespace ts2::content

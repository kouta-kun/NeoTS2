#include "content/bank.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <tuple>

namespace ts2::content {

namespace {

constexpr std::size_t kAlign = 16;

std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t> order(const bank::Entry& e) {
    return {e.type, e.group, e.instance, e.instanceHigh};
}

std::size_t aligned(std::size_t n) { return (n + kAlign - 1) / kAlign * kAlign; }

} // namespace

std::size_t writeBank(const std::filesystem::path& path, std::vector<BankResource> resources) {
    std::sort(resources.begin(), resources.end(), [](const BankResource& a, const BankResource& b) {
        return std::tie(a.key.type, a.key.group, a.key.instance, a.key.instanceHigh) <
               std::tie(b.key.type, b.key.group, b.key.instance, b.key.instanceHigh);
    });
    bank::Header header;
    header.count = static_cast<std::uint32_t>(resources.size());
    header.tableOffset = sizeof(bank::Header);
    std::size_t at = aligned(header.tableOffset + resources.size() * sizeof(bank::Entry));
    std::vector<bank::Entry> table;
    table.reserve(resources.size());
    for (const BankResource& r : resources) {
        bank::Entry e;
        e.type = r.key.type;
        e.group = r.key.group;
        e.instance = r.key.instance;
        e.instanceHigh = r.key.instanceHigh;
        e.offset = static_cast<std::uint32_t>(at);
        e.size = static_cast<std::uint32_t>(r.bytes.size());
        e.source = r.source;
        table.push_back(e);
        at = aligned(at + r.bytes.size());
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
    const std::vector<char> zeros(kAlign, 0);
    std::size_t written = 0;
    const auto put = [&](const void* data, std::size_t n) {
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
        written += n;
    };
    const auto padTo = [&](std::size_t offset) { put(zeros.data(), offset - written); };
    put(&header, sizeof header);
    put(table.data(), table.size() * sizeof(bank::Entry));
    for (std::size_t i = 0; i < resources.size(); ++i) {
        padTo(table[i].offset);
        put(resources[i].bytes.data(), resources[i].bytes.size());
    }
    padTo(at);
    if (!out) {
        throw std::runtime_error("failed writing " + path.string());
    }
    return written;
}

Bank::Bank(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
    bank::Header header;
    if (bytes.size() < sizeof header) {
        throw std::runtime_error("not a bank: too short");
    }
    std::memcpy(&header, bytes.data(), sizeof header);
    if (header.magic != bank::kMagic || header.version != bank::kVersion) {
        throw std::runtime_error("not a bank: bad header");
    }
    const std::size_t tableBytes = static_cast<std::size_t>(header.count) * sizeof(bank::Entry);
    if (header.tableOffset % alignof(bank::Entry) != 0 || header.tableOffset + tableBytes > bytes.size()) {
        throw std::runtime_error("not a bank: bad table");
    }
    table_ = reinterpret_cast<const bank::Entry*>(bytes.data() + header.tableOffset);
    count_ = header.count;
}

std::span<const std::uint8_t> Bank::find(const dbpf::ResourceKey& key) const {
    const auto want = std::tuple(key.type, key.group, key.instance, key.instanceHigh);
    const bank::Entry* end = table_ + count_;
    const bank::Entry* it = std::lower_bound(table_, end, want,
                                             [](const bank::Entry& e, const auto& w) { return order(e) < w; });
    if (it == end || order(*it) != want || std::size_t(it->offset) + it->size > bytes_.size()) {
        return {};
    }
    return bytes_.subspan(it->offset, it->size);
}

} // namespace ts2::content

// The converter's first step: builds the scenes of some lots against an install, notes every resource they read, and
// writes those to a bank file (src/content/bank.hpp) with a report of what the bank holds. Textures are stored as
// the game has them for now; converting them is the next step.
#include "content/bank.hpp"
#include "content/lot.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct Key {
    std::uint32_t type, group, instance, instanceHigh;
    bool operator<(const Key& o) const {
        return std::tie(type, group, instance, instanceHigh) < std::tie(o.type, o.group, o.instance, o.instanceHigh);
    }
};

std::uint32_t sourceOf(const std::string& name) {
    using namespace ts2::content::bank;
    return name == "models" ? kModels : name == "objects" ? kObjects : name == "catalog" ? kCatalog : kSkins;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: ts2bank <install-folder> <out.bank> <lot.package>...\n");
        return 1;
    }
    try {
        const std::filesystem::path install = argv[1], out = argv[2];
        ts2::scene::Assets assets(install);
        std::map<Key, ts2::content::BankResource> gathered;
        std::size_t differing = 0;
        assets.observeReads([&](const char* source, const ts2::dbpf::ResourceKey& key, const std::vector<std::uint8_t>& bytes) {
            auto [it, inserted] = gathered.try_emplace(Key{key.type, key.group, key.instance, key.instanceHigh});
            if (inserted) {
                it->second = {key, sourceOf(source), bytes};
            } else if (it->second.bytes != bytes) {
                ++differing;  // the same key in two sets of packages, with other contents: the bank keeps the first
            }
        });
        for (int i = 3; i < argc; ++i) {
            ts2::scene::SceneBuilder builder(assets);
            const auto lot = ts2::content::loadLot(argv[i]);
            ts2::scene::addLot(builder, lot, argv[i]);
            std::printf("%-28s %zu resources so far\n", std::filesystem::path(argv[i]).stem().string().c_str(), gathered.size());
        }
        assets.observeReads(nullptr);

        std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> byType;
        std::size_t total = 0;
        std::vector<ts2::content::BankResource> resources;
        for (auto& [key, r] : gathered) {
            auto& t = byType[key.type];
            ++t.first;
            t.second += r.bytes.size();
            total += r.bytes.size();
            resources.push_back(std::move(r));
        }
        const std::size_t fileSize = ts2::content::writeBank(out, std::move(resources));
        std::printf("\n%-12s %8s %12s\n", "type", "count", "bytes");
        for (const auto& [type, t] : byType) {
            std::printf("0x%08x   %8zu %12zu\n", type, t.first, t.second);
        }
        std::printf("\n%zu resources, %.2f MiB of data, file %.2f MiB; %zu keys read with different contents; %zu asset warnings\n",
                    gathered.size(), static_cast<double>(total) / 1048576.0, static_cast<double>(fileSize) / 1048576.0, differing,
                    assets.warnings.size());

        // Read it back through the reader and compare.
        std::ifstream in(out, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        const ts2::content::Bank bank(bytes);
        std::printf("bank reads back: %zu entries\n", bank.size());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "failed: %s\n", error.what());
        return 1;
    }
    return 0;
}

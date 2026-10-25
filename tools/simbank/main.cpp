// Gathers what a lot's simulation reads into a small tree a console can run it from:
//   simbank <install-folder> <lot.package> <out-folder> [ticks]
// out/code.package holds every resource the simulation read while the lot loaded and during `ticks` ticks (decompressed, under the keys the index gave them,
// the neighborhood's characters included), out/sim.cfg the installed-packs mask, and the lot's and its neighborhood's packages are copied to where the
// neighborhood folders put them. The console runs LotSimulation(packages, mask, lot) on that tree.
#include "content/install.hpp"
#include "content/neighborhood.hpp"
#include "content/resource_index.hpp"
#include "formats/dbpf_writer.hpp"
#include "sim/lot_simulation.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: simbank <install-folder> <lot.package> <out-folder> [ticks]\n");
        return 1;
    }
    namespace fs = std::filesystem;
    try {
        const fs::path install = argv[1], lot = argv[2], out = argv[3];
        const std::size_t ticks = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 3000;
        using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
        std::map<Key, std::vector<std::uint8_t>> gathered;
        ts2::content::ResourceIndex::sharedObserver = [&](const ts2::content::ResourceIndex::Resource& r, const std::vector<std::uint8_t>& bytes) {
            const auto& k = r.entry.key;
            gathered.try_emplace(Key{k.type, k.group, k.instance, k.instanceHigh}, bytes);
        };
        {
            ts2::sim::LotSimulation simulation(install, lot, {});
            for (std::size_t i = 0; i < ticks; ++i) {
                simulation.tick();
            }
        }
        ts2::content::ResourceIndex::sharedObserver = nullptr;

        const fs::path hood = ts2::content::neighborhoodOfLot(lot);
        const fs::path hoodOut = out / "Neighborhoods" / hood.filename();
        fs::create_directories(hoodOut / "Lots");
        {
            std::ofstream file(out / "code.package", std::ios::binary | std::ios::trunc);
            ts2::dbpf::PackageWriter writer(file);
            std::size_t bytes = 0;
            for (const auto& [key, data] : gathered) {
                writer.add({std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key)}, data, false, static_cast<std::uint32_t>(data.size()));
                bytes += data.size();
            }
            writer.finish(0);
            std::printf("code.package: %zu resources, %.2f MiB\n", gathered.size(), static_cast<double>(bytes) / 1048576.0);
        }
        fs::copy_file(lot, hoodOut / "Lots" / lot.filename(), fs::copy_options::overwrite_existing);
        const fs::path hoodPackage = ts2::content::neighborhoodPackage(hood);
        if (fs::exists(hoodPackage)) {
            fs::copy_file(hoodPackage, hoodOut / hoodPackage.filename(), fs::copy_options::overwrite_existing);
        }
        std::ofstream(out / "sim.cfg") << "packs=" << ts2::content::installedPacksMask(ts2::content::findProducts(install)) << "\nlot=Neighborhoods/" << hood.filename().string() << "/Lots/"
                                       << lot.filename().string() << "\n";
        std::printf("tree written to %s (lot %.2f MiB, neighborhood %.2f MiB)\n", out.c_str(), static_cast<double>(fs::file_size(lot)) / 1048576.0,
                    fs::exists(hoodPackage) ? static_cast<double>(fs::file_size(hoodPackage)) / 1048576.0 : 0.0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}

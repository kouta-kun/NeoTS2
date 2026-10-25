// Which behaviours (BHAV trees) a lot's objects can ever run, found statically, against which ones a run executes:
//   bhavclosure <install-folder> <lot.package> [ticks]
// The closure starts from each present object type's function table (OBJf) and interaction table (TTAB) and follows calls: by number (ids below
// 0x1000 are global, below 0x2000 the code owner's, the rest its semi-global group) and by name (Run Tree by Name: the name is a constant from a string table;
// only the group searched can depend on run-time values). It also counts the Run Tree by Name sites whose group comes from a GUID in temps (mode 3).
#include "content/install.hpp"
#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "content/resource_index.hpp"
#include "formats/resource_types.hpp"
#include "sim/behavior_closure.hpp"
#include "sim/index_code_source.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/vm_execution.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace {

using namespace ts2;
using sim::Tree;

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: bhavclosure <install-folder> <lot.package> [ticks]\n");
        return 1;
    }
    try {
        const std::filesystem::path install = argv[1], lotPath = argv[2];
        const std::size_t ticks = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 0;
        content::ResourceIndex index;
        for (const auto& path : content::objectLoadOrder(content::findProducts(install))) {
            index.addPackage(path);
        }
        content::addCharacters(index, content::neighborhoodOfLot(lotPath));
        sim::IndexCodeSource code(index);
        const auto lot = content::loadLot(lotPath);

        sim::BehaviorClosure c{code};
        std::set<std::uint32_t> guids;
        for (const auto& object : lot.objects) {
            if (object.type) {
                guids.insert(object.type->guid);
            }
        }
        for (const std::uint32_t guid : code.objectTypeGuids()) {
            if (const auto* t = code.objectType(guid)) {
                c.allGroups.insert(t->group);
            }
        }
        // Also every object type whose name starts "Social" (the objects Sims' interactions create at run time), with those the lot has.
        std::set<std::uint32_t> socialGuids;
        for (const std::uint32_t guid : code.objectTypeGuids()) {
            if (const auto* t = code.objectType(guid); t && t->definition.name.rfind("Social", 0) == 0) {
                socialGuids.insert(guid);
            }
        }
        if (std::getenv("WITH_SOCIAL")) {
            guids.insert(socialGuids.begin(), socialGuids.end());
        }
        std::printf("%zu object types named Social*\n", socialGuids.size());
        std::size_t types = guids.size(), seeds = 0;
        c.addTypes(guids);
        const auto run = [&](bool expandMode3) {
            c.expandMode3 = expandMode3;
            c.run();
        };
        const auto bytesOf = [&](const std::set<Tree>& set) {
            std::size_t total = 0;
            for (const auto& [g, i] : set) {
                if (const auto* r = index.find({types::kBehavior, g, i, 0})) {
                    total += r->entry.uncompressedSize ? r->entry.uncompressedSize : r->entry.size;
                }
            }
            return total;
        };
        seeds = c.start.size();
        std::size_t all = 0;
        for (const auto& r : index.resources()) {
            all += r.entry.key.type == types::kBehavior;
        }
        std::printf("%zu object types present (groups: %zu), %zu entry trees (function tables and interaction tables); %zu behaviours in the install\n", types, c.groups.size(),
                    seeds, all);
        {
            // Every Run Tree by Name in the install: how many take the group from a GUID in temps (mode 3), and the modes in all.
            std::map<int, std::size_t> modes;
            std::size_t sites = 0;
            for (const auto& r : index.resources()) {
                if (r.entry.key.type != types::kBehavior) {
                    continue;
                }
                const auto tree = code.behavior(r.entry.key.group, static_cast<std::uint16_t>(r.entry.key.instance));
                for (const auto& in : tree ? tree->instructions : std::vector<bhav::Instruction>{}) {
                    if (in.opcode == sim::vm_detail::kRunTreeByNameOpcode) {
                        ++sites;
                        ++modes[in.operands[5]];
                    }
                }
            }
            std::printf("Run Tree by Name in the whole install: %zu sites; by mode:", sites);
            for (const auto& [m, n] : modes) {
                std::printf(" %d:%zu", m, n);
            }
            std::printf("\n");
        }
        run(false);
        const std::set<Tree> bound = c.trees;
        std::printf("closure, Run Tree by Name searched in the present groups: %zu trees, %.2f MB raw\n", bound.size(), static_cast<double>(bytesOf(bound)) / 1e6);
        std::printf("  Run Tree by Name sites reached: %zu (names not found in a string table: %zu); sites whose group comes from a GUID in temps (mode 3): %zu\n", c.namedSites,
                    c.missingNames, c.mode3Sites);
        run(true);
        std::printf("closure with mode-3 sites searched in every object group of the install: %zu trees, %.2f MB raw\n", c.trees.size(), static_cast<double>(bytesOf(c.trees)) / 1e6);
        const std::set<Tree> widest = c.trees;
        for (const auto& [g, i, pc] : c.mode3Where) {
            const auto t = code.behavior(g, i);
            std::printf("  mode 3 at group 0x%08X tree 0x%04X instruction %u (%s)\n", g, i, pc, t ? t->name.c_str() : "?");
        }

        if (ticks > 0) {
            std::set<Tree> executed;
            sim::LotSimulation simulation(install, lotPath, {});
            simulation.vm().onStep = [&](const sim::StepInfo& s) { executed.emplace(s.group, s.instance); };
            for (std::size_t t = 0; t < ticks; ++t) {
                simulation.tick();
            }
            std::size_t inBound = 0, inWidest = 0;
            for (const auto& t : executed) {
                inBound += bound.contains(t);
                inWidest += widest.contains(t);
            }
            for (const auto& t : executed) {
                if (!bound.contains(t)) {
                    const auto tree = code.behavior(t.first, t.second);
                    std::printf("  executed but not in the closure: group 0x%08X tree 0x%04X (%s)\n", t.first, t.second, tree ? tree->name.c_str() : "?");
                }
            }
            std::printf("executed in %zu ticks: %zu trees, %.2f MB raw; %zu of them in the closure, %zu in the wider closure\n", ticks, executed.size(),
                        static_cast<double>(bytesOf(executed)) / 1e6, inBound, inWidest);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}

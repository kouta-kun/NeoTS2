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
using Tree = std::pair<std::uint32_t, std::uint16_t>;  // group, instance

struct Closure {
    sim::CodeSource& code;
    std::set<std::uint32_t> groups;       // the groups of the types present
    std::set<std::uint32_t> allGroups;    // every object group of the install (for mode 3 sites)
    bool expandMode3 = false;
    std::set<std::tuple<std::uint32_t, std::uint16_t, std::uint32_t>> states;  // group, instance, code owner
    std::set<Tree> trees;
    std::size_t namedSites = 0, mode3Sites = 0, mode3Reached = 0, missingNames = 0;
    std::set<std::tuple<std::uint32_t, std::uint16_t, std::uint16_t>> mode3Where;

    Tree resolve(std::uint16_t id, std::uint32_t owner) {
        return {id < 0x1000 ? sim::kGlobalGroup : id < 0x2000 ? owner : code.semiGlobalGroup(owner), id};
    }

    void visit(std::uint32_t group, std::uint16_t instance, std::uint32_t owner) {
        if (!states.emplace(group, instance, owner).second) {
            return;
        }
        const auto tree = code.behavior(group, instance);
        if (!tree) {
            return;
        }
        trees.emplace(group, instance);
        for (std::size_t pc = 0; pc < tree->instructions.size(); ++pc) {
            const auto& in = tree->instructions[pc];
            if (in.opcode >= 0x0100) {
                const auto [g, i] = resolve(in.opcode, owner);
                visit(g, i, owner);
            } else if (in.opcode == sim::vm_detail::kRunTreeByNameOpcode) {
                named(in, group, instance, pc, owner);
            }
        }
    }

    void named(const bhav::Instruction& in, std::uint32_t group, std::uint16_t instance, std::size_t pc, std::uint32_t owner) {
        const auto& o = in.operands;
        const std::uint8_t mode = o[5], flags = o[2];
        const std::size_t number = o[4] | ((flags & 0x40) ? 0x100 : 0);
        if (number == 0) {
            return;
        }
        ++namedSites;
        const std::uint32_t tableGroup = (flags & 0x01) ? sim::kGlobalGroup : (flags & 0x02) ? code.semiGlobalGroup(owner) : owner;
        const auto name = code.string(tableGroup, sim::vm_detail::kTreeNameTable, number - 1);
        if (!name || name->empty()) {
            ++missingNames;
            return;
        }
        const bool fromGuid = mode == 3;
        if (fromGuid) {
            ++mode3Sites;
            mode3Where.emplace(group, instance, static_cast<std::uint16_t>(pc));
        }
        const auto& candidates = fromGuid && expandMode3 ? allGroups : groups;
        bool reached = false;
        for (const std::uint32_t treeGroup : candidates) {
            for (const std::uint32_t g : {treeGroup, code.semiGlobalGroup(treeGroup), sim::kGlobalGroup}) {
                if (const auto found = code.behaviorByName(g, *name)) {
                    visit(g, *found, treeGroup);
                    reached = true;
                    break;
                }
            }
        }
        if (fromGuid && reached) {
            ++mode3Reached;
        }
    }
};

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

        Closure c{code};
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
        std::size_t types = 0, seeds = 0;
        std::vector<std::pair<std::uint32_t, std::uint16_t>> start;  // owner, tree
        for (const std::uint32_t guid : guids) {
            const auto* type = code.objectType(guid);
            if (!type) {
                continue;
            }
            ++types;
            c.groups.insert(type->group);
            if (const auto* functions = code.objectFunctions(*type)) {
                for (const auto& f : *functions) {
                    for (const std::uint16_t id : {f.check, f.action}) {
                        if (id != 0) {
                            start.emplace_back(type->group, id);
                        }
                    }
                }
            }
            std::uint16_t tableId = type->definition.field(objd::field::kInteractionTableId);
            if (tableId == 0xFFFF) {
                const auto* master = code.multiTileMaster(*type);
                tableId = master ? master->definition.field(objd::field::kInteractionTableId) : 0;
            }
            if (tableId != 0 && tableId < 0x8000) {
                auto table = code.interactionTable(type->group, tableId);
                if (!table) {
                    table = code.interactionTable(code.semiGlobalGroup(type->group), tableId);
                }
                if (table) {
                    for (const auto& line : table->interactions) {
                        for (const std::uint16_t id : {line.action, line.test}) {
                            if (id != 0) {
                                start.emplace_back(type->group, id);
                            }
                        }
                    }
                }
            }
        }
        const auto run = [&](bool expandMode3) {
            c.expandMode3 = expandMode3;
            c.states.clear();
            c.trees.clear();
            c.namedSites = c.mode3Sites = c.mode3Reached = c.missingNames = 0;
            for (const auto& [owner, id] : start) {
                const auto [g, i] = c.resolve(id, owner);
                c.visit(g, i, owner);
            }
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
        seeds = start.size();
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

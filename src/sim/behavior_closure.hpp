#pragma once

#include "sim/code_source.hpp"

#include <cstdint>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace ts2::sim {

using Tree = std::pair<std::uint32_t, std::uint16_t>;  // group, instance

// Which behaviours (BHAV trees) objects can ever run, found statically. The closure starts from each object type's function table (OBJf) and interaction
// table (TTAB) and follows calls: by number (ids below 0x1000 are global, below 0x2000 the code owner's, the rest its semi-global group) and by name (Run Tree
// by Name: the name is a constant from a string table; only the group searched can depend on run-time values). It also counts the Run Tree by Name sites
// whose group comes from a GUID in temps (mode 3).
struct BehaviorClosure {
    CodeSource& code;
    std::set<std::uint32_t> groups;       // the groups of the types added
    std::set<std::uint32_t> allGroups;    // every object group of the install (for mode 3 sites)
    bool expandMode3 = false;
    std::set<std::tuple<std::uint32_t, std::uint16_t, std::uint32_t>> states;  // group, instance, code owner
    std::set<Tree> trees;
    std::size_t namedSites = 0, mode3Sites = 0, mode3Reached = 0, missingNames = 0;
    std::set<std::tuple<std::uint32_t, std::uint16_t, std::uint16_t>> mode3Where;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> start;  // owner group, tree: where the walk begins

    Tree resolve(std::uint16_t id, std::uint32_t owner);
    void visit(std::uint32_t group, std::uint16_t instance, std::uint32_t owner);
    // Adds the entry trees of the object types with these GUIDs (and their groups).
    void addTypes(const std::set<std::uint32_t>& guids);
    // Walks from every entry tree; the counts and `trees` start over.
    void run();

private:
    void named(const bhav::Instruction& in, std::uint32_t group, std::uint16_t instance, std::size_t pc, std::uint32_t owner);
};

} // namespace ts2::sim

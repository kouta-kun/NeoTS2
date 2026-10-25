#include "sim/behavior_closure.hpp"

#include "sim/vm_execution.hpp"

namespace ts2::sim {

Tree BehaviorClosure::resolve(std::uint16_t id, std::uint32_t owner) {
    return {id < 0x1000 ? kGlobalGroup : id < 0x2000 ? owner : code.semiGlobalGroup(owner), id};
}

void BehaviorClosure::visit(std::uint32_t group, std::uint16_t instance, std::uint32_t owner) {
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
        } else if (in.opcode == vm_detail::kRunTreeByNameOpcode) {
            named(in, group, instance, pc, owner);
        } else if (in.opcode == vm_detail::kCreateObjectOpcode && (in.operands[5] & vm_detail::kCreateGuidFromTemps) == 0) {
            const auto& o = in.operands;
            createdGuids.insert(static_cast<std::uint32_t>(o[0]) | static_cast<std::uint32_t>(o[1]) << 8 | static_cast<std::uint32_t>(o[2]) << 16 |
                                static_cast<std::uint32_t>(o[3]) << 24);
        }
    }
}

void BehaviorClosure::named(const bhav::Instruction& in, std::uint32_t group, std::uint16_t instance, std::size_t pc, std::uint32_t owner) {
    const auto& o = in.operands;
    const std::uint8_t mode = o[5], flags = o[2];
    const std::size_t number = o[4] | ((flags & 0x40) ? 0x100 : 0);
    if (number == 0) {
        return;
    }
    ++namedSites;
    const std::uint32_t tableGroup = (flags & 0x01) ? kGlobalGroup : (flags & 0x02) ? code.semiGlobalGroup(owner) : owner;
    const auto name = code.string(tableGroup, vm_detail::kTreeNameTable, number - 1);
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
        for (const std::uint32_t g : {treeGroup, code.semiGlobalGroup(treeGroup), kGlobalGroup}) {
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

void BehaviorClosure::addTypes(const std::set<std::uint32_t>& guids) {
    for (const std::uint32_t guid : guids) {
        const auto* type = code.objectType(guid);
        if (!type) {
            continue;
        }
        groups.insert(type->group);
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
}

void BehaviorClosure::run() {
    states.clear();
    trees.clear();
    createdGuids.clear();
    namedSites = mode3Sites = mode3Reached = missingNames = 0;
    for (const auto& [owner, id] : start) {
        const auto [g, i] = resolve(id, owner);
        visit(g, i, owner);
    }
}

} // namespace ts2::sim

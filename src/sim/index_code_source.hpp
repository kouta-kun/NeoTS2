#pragma once

#include "content/resource_index.hpp"
#include "sim/code_source.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ts2::sim {

// A bank's table of the behaviours looked up by name: a resource of this type (group 0, instance 0) in the bank's code package, which lets
// behaviorByName answer without reading every behaviour of a group to learn its name.
constexpr std::uint32_t kNameTableType = 0x4E414D45;
// Entries are (group, name) -> instance.
using NameTable = std::map<std::pair<std::uint32_t, std::string>, std::uint16_t>;
std::vector<std::uint8_t> writeNameTable(const NameTable& names);
NameTable readNameTable(const std::vector<std::uint8_t>& bytes);

// A bank's list of the behaviours, interaction tables and constants the recorded run read, which a console reads and parses before the first tick instead of
// in the middle of one (group 0, instance 0 of this type): the keys as (type, group, instance) triples of 32-bit numbers, after a count.
constexpr std::uint32_t kWarmListType = 0x5741524D;
std::vector<std::uint8_t> writeWarmList(const std::vector<std::array<std::uint32_t, 3>>& keys);

class IndexCodeSource : public CodeSource {
public:
    // Called with each name lookup and its answer (for tools that gather a name table).
    static inline std::function<void(std::uint32_t, const std::string&, std::optional<std::uint16_t>)> nameObserver;

    explicit IndexCodeSource(content::ResourceIndex& index);

    // Reads and parses what the bank's warm list names (nothing without one); returns how many.
    std::size_t warm();

    std::shared_ptr<const bhav::Behavior> behavior(std::uint32_t group,
                                                   std::uint16_t instance) override;
    const std::vector<std::int16_t>* constants(std::uint32_t group,
                                               std::uint16_t instance) override;
    std::uint32_t semiGlobalGroup(std::uint32_t objectGroup) override;
    const ObjectType* objectType(std::uint32_t guid) override;
    const ObjectType* multiTileMaster(const ObjectType& part) override;
    const std::vector<std::uint32_t>& objectTypeGuids() override;
    const std::map<std::string, std::string>* ageData(std::uint32_t group) override;
    [[nodiscard]] std::optional<std::string> string(std::uint32_t group, std::uint16_t instance,
                                      std::size_t index) override;
    [[nodiscard]] std::optional<std::string> stringDescription(std::uint32_t group, std::uint16_t instance, std::size_t index) override;
    [[nodiscard]] std::optional<std::string> catalogString(std::uint32_t group, std::uint16_t instance, std::size_t index) override;
    [[nodiscard]] std::optional<tga::Pixels> image(std::uint32_t group, std::uint32_t instance, std::uint32_t maxSide) override;
    [[nodiscard]] std::optional<std::string> interactionString(std::uint32_t group, std::uint16_t instance,
                                                 std::size_t index) override;
    [[nodiscard]] std::optional<std::uint16_t> behaviorByName(std::uint32_t group, const std::string& name) override;
    std::shared_ptr<const ttab::InteractionTable> interactionTable(std::uint32_t group,
                                                                  std::uint16_t instance) override;
    const std::vector<objf::Function>* objectFunctions(const ObjectType& type) override;
    const fcns::Tunings* tunings() override;
    const std::vector<slot::Entry>& globalSlots() override;

    const std::unordered_map<std::uint32_t, std::uint32_t>& semiGlobalGroups() const {
        return semiGlobals_;
    }

private:
    content::ResourceIndex& index_;
    std::unordered_map<std::uint64_t, std::shared_ptr<const bhav::Behavior>> behaviors_;
    std::unordered_map<std::uint64_t, std::shared_ptr<const ttab::InteractionTable>> interactionTables_;
    std::unordered_map<std::uint64_t, std::optional<std::vector<std::int16_t>>> constants_;
    std::unordered_map<std::uint32_t, std::uint32_t> semiGlobals_;
    std::unordered_map<std::uint32_t, ObjectType> objectTypes_;
    std::vector<std::uint32_t> objectTypeGuids_;
    // Multi-tile masters by (group << 16 | multi-tile id): their GUIDs.
    std::unordered_map<std::uint64_t, std::uint32_t> multiTileMasters_;
    std::unordered_map<std::uint64_t, std::optional<std::vector<objf::Function>>> objectFunctions_;
    bool objectTypesLoaded_ = false;
    const std::vector<std::string>& strings(std::uint32_t type, std::uint32_t group, std::uint16_t instance,
                                           bool descriptions = false);
    // Each table's US English texts, by (type, group, instance).
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint16_t>, std::vector<std::string>> strings_;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint16_t>, std::vector<std::string>> descriptions_;
    std::map<std::uint32_t, std::optional<std::map<std::string, std::string>>> ageData_;
    std::unordered_map<std::uint32_t, std::unordered_map<std::string, std::uint16_t>> behaviorNames_;
    bool behaviorNamesLoaded_ = false;
    std::optional<std::optional<fcns::Tunings>> tunings_;
    std::optional<std::vector<slot::Entry>> globalSlots_;
};

} // namespace ts2::sim

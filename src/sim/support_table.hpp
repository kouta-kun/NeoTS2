// What the simulation asks of the game's models and animations (routing's footprints, animation lengths and clips, outfit properties), recorded while a
// lot runs on a computer that has the game files and replayed on a console that has not. Whatever a replay is asked that the recording was not asked
// gets the answer "unknown", as a missing model or animation does.
#pragma once

#include "sim/animation.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/route_world.hpp"
#include "sim/walk_plan.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ts2::sim {

struct SupportTable {
    std::map<std::string, RouteModel> routeModels;  // by model name
    std::map<std::uint32_t, std::optional<RouteRect>> posts;  // fence post footprints by fence GUID
    std::map<std::uint32_t, bool> fenceBlocks;                // whether a fence blocks Sims, by fence GUID
    std::map<std::string, std::optional<float>> animationLengths;
    std::map<std::string, bool> animationCarries;
    std::map<std::string, std::optional<LocoClip>> locoClips;
    std::map<std::pair<std::string, int>, std::optional<float>> eventTimes;
    std::map<std::string, std::vector<AnimationEvent>> events;
    std::map<std::pair<std::string, int>, float> turns;  // an animation's root turn by milliseconds (whole), as asked
    std::map<std::tuple<int, int, int>, std::optional<int>> outfitProperties;  // by neighbor, category, property
};

[[nodiscard]] std::vector<std::uint8_t> writeSupportTable(const SupportTable& table);
// Throws std::runtime_error when the bytes are not a support table.
[[nodiscard]] SupportTable readSupportTable(std::span<const std::uint8_t> bytes);

// Routing's sources and the world's animation and outfit callbacks, answered from `table` (which the simulation keeps alive).
[[nodiscard]] RouteSources routeSourcesFrom(std::shared_ptr<const SupportTable> table);
void installSupport(LotSimulation& simulation, std::shared_ptr<const SupportTable> table);

} // namespace ts2::sim

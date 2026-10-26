#include "console_life.hpp"

#include "sim/clock.hpp"
#include "sim/turn.hpp"

#include <algorithm>

namespace app {

namespace {

// A tick that takes long (a route plan) must not be followed by a pile of them to catch up.
constexpr double kMaxTicksPerUpdate = 2;

} // namespace

ConsoleLife::ConsoleLife(std::unique_ptr<ts2::sim::LotSimulation> sim, const ts2::scene::Scene& scene) : sim_(std::move(sim)) {
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        if (scene.instances[i].name == "sim-marker" && scene.instances[i].object != 0) {
            markers_.emplace(scene.instances[i].object, i);
        }
    }
}

void ConsoleLife::update(double ms, ts2::scene::Scene& scene, ts2::render::IRenderer& renderer) {
    using ts2::sim::LotSimulation;
    constexpr double tickMs = LotSimulation::kNormalTickMs;
    if (!sim_) {
        return;
    }
    pendingMs_ = std::min(pendingMs_ + ms, kMaxTicksPerUpdate * tickMs);
    bool ticked = false;
    while (pendingMs_ >= tickMs) {
        sim_->tick();
        pendingMs_ -= tickMs;
        ++ticks_;
        ticked = true;
    }
    if (!ticked || !scene.floorHeight) {
        return;
    }
    for (const auto& [object, instance] : markers_) {
        const ts2::sim::SimObject* sim = sim_->world().find(static_cast<std::int16_t>(object));
        if (sim == nullptr || sim->x == ts2::sim::kOffLot || sim->y == ts2::sim::kOffLot) {
            continue;
        }
        const float x = static_cast<float>(sim->y) / 65536.0f;  // the lot's x and y are the position's y and x
        const float y = static_cast<float>(sim->x) / 65536.0f;
        const auto place = ts2::scene::Transform::at(x, y, scene.floorHeight(sim->level, x, y), sim->heading ? *sim->heading : ts2::sim::compassHeading(sim->data[1]));
        scene.instances[instance].transform = place;
        scene.instances[instance].level = scene.groundLevel + sim->level - 1;
        renderer.setPlace(instance, place);
    }
}

std::string ConsoleLife::clockText() {
    if (!sim_) {
        return "no simulation";
    }
    return ts2::sim::clockText(sim_->globals());
}

} // namespace app

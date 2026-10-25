#pragma once

#include "render/irenderer.hpp"
#include "scene/scene.hpp"
#include "sim/lot_simulation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace app {

// A lot's simulation on the console, run in step with real time, and a marker standing where each Sim does (a baked scene has an instance named
// "sim-marker" for every Sim, filed under its object id; ts2bake makes them) until the console draws the Sims' own models. This is LotLife without the
// game's assets: its scripts' routes and animation lengths come from the bank (see tools/simbank).
class ConsoleLife {
public:
    // `sim` is the lot's simulation, loaded from a bank with its support table installed. `scene` the baked scene to find the markers in.
    ConsoleLife(std::unique_ptr<ts2::sim::LotSimulation> sim, const ts2::scene::Scene& scene);

    // Runs the simulation for `ms` of real time at the game's normal speed (whole ticks, the rest waits for the next call; at most a few ticks a call
    // so that a slow tick cannot make the next ones pile up), then moves the markers of Sims that moved.
    void update(double ms, ts2::scene::Scene& scene, ts2::render::IRenderer& renderer);

    ts2::sim::LotSimulation& simulation() { return *sim_; }
    // The Sim clock as the live panel shows it.
    std::string clockText();
    std::size_t ticks() const { return ticks_; }
    std::size_t markerCount() const { return markers_.size(); }

private:
    std::unique_ptr<ts2::sim::LotSimulation> sim_;
    std::unordered_map<std::uint32_t, std::size_t> markers_;  // object id -> scene instance
    double pendingMs_ = 0;
    std::size_t ticks_ = 0;
};

} // namespace app

#pragma once

#include "render/irenderer.hpp"
#include "scene/scene.hpp"
#include "sim/lot_simulation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace app {

// A lot's simulation on the console, run in step with real time, and each Sim's models (its hair, head and body: instances of the baked scene that ts2bake
// files under the Sim's object id) moved to where the Sim stands and turned as it is, in the pose they were baked in (no animation yet). This is LotLife
// without the game's assets: its scripts' routes and animation lengths come from the bank (see tools/simbank).
class ConsoleLife {
public:
    // `sim` is the lot's simulation, loaded from a bank with its support table installed. `scene` the baked scene to find the markers in.
    ConsoleLife(std::unique_ptr<ts2::sim::LotSimulation> sim, const ts2::scene::Scene& scene);

    // Runs the simulation for `ms` of real time at the game's normal speed (whole ticks, the rest waits for the next call; at most a few ticks a call
    // so that a slow tick cannot make the next ones pile up), then moves the Sims' models.
    void update(double ms, ts2::scene::Scene& scene, ts2::render::IRenderer& renderer);

    ts2::sim::LotSimulation& simulation() { return *sim_; }
    bool hasSimulation() const { return sim_ != nullptr; }
    // The Sim clock as the live panel shows it.
    std::string clockText();
    std::size_t ticks() const { return ticks_; }
    std::size_t simCount() const;

private:
    std::unique_ptr<ts2::sim::LotSimulation> sim_;
    std::vector<std::pair<std::uint32_t, std::size_t>> parts_;  // object id, scene instance: a Sim has several
    double pendingMs_ = 0;
    std::size_t ticks_ = 0;
};

} // namespace app

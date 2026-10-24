#pragma once

#include "scene/scene.hpp"

#include <string>

namespace ts2::scene {

// What a scene holds in memory, by kind, in one line of text: its images and their smaller levels, its meshes (vertices
// and indices), its painted terrain, its instances and materials. For finding what takes the memory (neots2 logs it
// with a lot shown). Sizes are what the arrays hold, not what the allocator keeps.
[[nodiscard]] std::string describeMemory(const Scene& scene);

// Where a scene's triangles are: those drawn if every instance were in view (an instance of a mesh draws all of it),
// by kind of instance, and the meshes that draw the most. For sizing the geometry a smaller machine could draw.
[[nodiscard]] std::string describeGeometry(const Scene& scene);

} // namespace ts2::scene

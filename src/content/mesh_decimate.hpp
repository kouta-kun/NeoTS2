// Fewer triangles for a mesh, for converters: edge collapses by quadric error (Garland and Heckbert), each moving one vertex
// onto the other so no attribute needs blending, with the vertices on texture seams and open edges left where they are.
// Skinned groups and groups of other primitives than triangle lists are left alone, as are very small ones.
#pragma once

#include "formats/scenegraph.hpp"

#include <cstddef>

namespace ts2::content {

struct DecimateStats {
    std::size_t groups = 0;          // triangle-list groups seen
    std::size_t groupsReduced = 0;
    std::size_t groupsSkipped = 0;   // the next four together
    std::size_t skippedSkinned = 0, skippedSmall = 0, skippedData = 0, skippedStuck = 0;  // stuck: nothing could be collapsed
    std::size_t trianglesSkinned = 0, trianglesSmall = 0, trianglesData = 0, trianglesStuck = 0;  // in the groups left alone, by reason
    std::size_t trianglesBefore = 0;
    std::size_t trianglesAfter = 0;
};

// The mesh with about `keep` (0 to 1) of each reducible group's triangles, fewer being taken out where more would move the
// surface further than `maxError` times the group's size (the root of the mean squared distance over the surface a collapse
// touches). Unused links and elements are dropped.
[[nodiscard]] scenegraph::Mesh decimate(const scenegraph::Mesh& mesh, double keep, double maxError, DecimateStats& stats);

} // namespace ts2::content

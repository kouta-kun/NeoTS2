// What colours a draw's vertices to what scene.frag.hlsl makes of them, apart from the texture the colour is multiplied by,
// worked out on the CPU for renderers with no pixel shaders (the OpenGL 1 one and the PlayStation 2's).
#pragma once

#include "render/frame_plan.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <vector>

namespace ts2::render {

// `lightMapShare` is the share of a light map's gain that draws a surface when the light map itself can't be multiplied in.
// `scale` is what the colours are multiplied by on their way to bytes: 0.5 where the texture stage multiplies by 2 again
// (OpenGL's texture environment scale), which is also how the Graphics Synthesizer reads a vertex colour (0x80 is 1).
// Four bytes (rgba) are written per vertex, alpha unscaled (255 is opaque).
void vertexColors(const VertexUniforms& vu, const FragmentUniforms& fu, const float* vertices, unsigned count, float lightMapShare, float scale,
                  const scene::Image* lightImage, const scene::Image* lightFrom, std::vector<std::uint8_t>& out);

} // namespace ts2::render

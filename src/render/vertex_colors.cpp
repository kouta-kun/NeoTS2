#include "render/vertex_colors.hpp"

#include "render/cpu_light.hpp"
#include "render/fixed_layout.hpp"
#include "render/mesh_pack.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ts2::render {

namespace {

// Where a vertex keeps what (see kVertexFloats).
constexpr std::size_t kNormalAt = 3, kAlphaAt = 8, kLightUvAt = 9, kColorAt = 11;

Mat4 fromRows(const float* rows) {
    Mat4 r;
    std::copy_n(rows, 16, r.m.begin());
    return r;
}

std::uint8_t byteOf(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255));
}

} // namespace

void vertexColors(const VertexUniforms& vu, const FragmentUniforms& fu, const float* vertices, unsigned count, float lightMapShare, float scale,
                  const scene::Image* lightImage, const scene::Image* lightFrom, std::vector<std::uint8_t>& out) {
    const Mat4 model = fromRows(vu.model);
    const std::array<float, 16>& m = model.m;
    const bool lightMap = fu.lightMap[0] > 0;
    const bool hasTexture = fu.hasTexture > 0.5f;
    const bool emissive = fu.emissive[3] > 0.5f;
    const float* color = fu.color;
    out.resize(std::size_t{count} * 4);
    for (unsigned i = 0; i < count; ++i) {
        const float* v = vertices + std::size_t{i} * kVertexFloats;
        const float* vc = v + kColorAt;
        float rgb[3];
        if (lightMap) {
            // Floors and walls: the texture alone or times the material's colour, then the light map. Taken from
            // the light map's pixels at the vertex where it has them, else `lightMapShare` of its gain.
            std::array<float, 3> lit{1, 1, 1};
            if (lightImage != nullptr) {
                lit = sampleLightMap(*lightImage, v[kLightUvAt], v[kLightUvAt + 1]);
                if (lightFrom != nullptr && fu.lightMapFade < 1) {
                    const auto from = sampleLightMap(*lightFrom, v[kLightUvAt], v[kLightUvAt + 1]);
                    for (std::size_t k = 0; k < 3; ++k) {
                        lit[k] = from[k] + (lit[k] - from[k]) * fu.lightMapFade;
                    }
                }
            }
            for (std::size_t k = 0; k < 3; ++k) {
                float factor = lightMapShare;
                if (lightImage != nullptr) {
                    factor = lit[k] * fu.lightMap[0];
                    if (fu.lightMap[2] > 0.5f) {  // the lot's terrain: darkened by its light map, never brightened
                        factor = std::min(1.0f, factor);
                    }
                }
                rgb[k] = (fu.lightMap[1] > 0.5f && hasTexture ? 1.0f : color[k] * vc[k]) * factor;
            }
        } else if (fu.unlit > 0.5f) {
            for (std::size_t k = 0; k < 3; ++k) {
                rgb[k] = color[k] * vc[k];
            }
        } else {
            const Vec3 world{m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3], m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7],
                             m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11]};
            const Vec3 normal{m[0] * v[kNormalAt] + m[1] * v[kNormalAt + 1] + m[2] * v[kNormalAt + 2],
                              m[4] * v[kNormalAt] + m[5] * v[kNormalAt + 1] + m[6] * v[kNormalAt + 2],
                              m[8] * v[kNormalAt] + m[9] * v[kNormalAt + 1] + m[10] * v[kNormalAt + 2]};
            // Without normal maps the lights they would take are lit here with the others.
            const std::array<float, 4> light = gameLight(vu, normal, world, true);
            if (light[3] > 1.5f) {  // a family thumbnail: the texture times the light, which holds the material's diffuse
                for (std::size_t k = 0; k < 3; ++k) {
                    rgb[k] = (hasTexture ? 1.0f : color[k]) * light[k];
                }
            } else if (light[3] > 0.5f) {
                for (std::size_t k = 0; k < 3; ++k) {
                    // A Sim: the vertex light times the diffuse scaled by 0.8; others: the material's colour times the light.
                    rgb[k] = fu.simLighting > 0.5f ? (hasTexture ? 1.0f : color[k]) * vc[k] * light[k] * color[k] * 0.8f
                                                   : color[k] * vc[k] * light[k];
                }
            } else {  // a sky and a sun
                const Vec3 n = normalize(normal);
                const float sky = 0.45f + (0.85f - 0.45f) * std::clamp(n[2] * 0.5f + 0.5f, 0.0f, 1.0f);
                const float sun = std::clamp(dot(n, {fu.sunDirection[0], fu.sunDirection[1], fu.sunDirection[2]}), 0.0f, 1.0f) * 0.75f;
                for (std::size_t k = 0; k < 3; ++k) {
                    rgb[k] = color[k] * vc[k] * (sky + sun);
                }
            }
            if (emissive) {  // added over the lit colour, times the texture and the vertex colour
                for (std::size_t k = 0; k < 3; ++k) {
                    rgb[k] += vc[k] * fu.emissive[k];
                }
            }
        }
        std::uint8_t* at = &out[std::size_t{i} * 4];
        at[0] = byteOf(rgb[0] * scale);
        at[1] = byteOf(rgb[1] * scale);
        at[2] = byteOf(rgb[2] * scale);
        at[3] = byteOf(color[3] * v[kAlphaAt]);
    }
}

} // namespace ts2::render

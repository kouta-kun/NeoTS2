#include "render/console_scene.hpp"

#include "content/mesh_decimate.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>
#include <stdexcept>

namespace ts2::render {

namespace {

constexpr std::uint32_t kMagic = 0x31435343;  // "CSC1"
constexpr std::uint32_t kVersion = 5;

class Out {
public:
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { raw(&v, 2); }
    void align4() { while (bytes.size() % 4 != 0) bytes.push_back(0); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void flag(bool v) { u8(v ? 1 : 0); }
    template <std::size_t N> void floats(const std::array<float, N>& a) { for (const float f : a) f32(f); }
    void str(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); raw(s.data(), s.size()); }
    void floatList(const std::vector<float>& v) { u32(static_cast<std::uint32_t>(v.size())); raw(v.data(), v.size() * 4); }
    void bytesList(const std::vector<std::uint8_t>& v) { u32(static_cast<std::uint32_t>(v.size())); raw(v.data(), v.size()); }
    void intList(const std::vector<int>& v) { u32(static_cast<std::uint32_t>(v.size())); raw(v.data(), v.size() * 4); }
    void transform(const scene::Transform& t) { floats(t.m); }
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    std::vector<std::uint8_t> bytes;
};

class In {
public:
    explicit In(std::span<const std::uint8_t> data) : data_(data) {}
    std::uint8_t u8() { std::uint8_t v; raw(&v, 1); return v; }
    std::uint16_t u16() { std::uint16_t v; raw(&v, 2); return v; }
    std::uint32_t u32() { std::uint32_t v; raw(&v, 4); return v; }
    std::int32_t i32() { std::int32_t v; raw(&v, 4); return v; }
    float f32() { float v; raw(&v, 4); return v; }
    bool flag() { return u8() != 0; }
    template <std::size_t N> std::array<float, N> floats() { std::array<float, N> a; for (auto& f : a) f = f32(); return a; }
    std::string str() { const std::size_t n = count(1); std::string s(n, '\0'); raw(s.data(), n); return s; }
    std::vector<float> floatList() { const std::size_t n = count(4); std::vector<float> v(n); raw(v.data(), n * 4); return v; }
    std::vector<std::uint8_t> bytesList() { const std::size_t n = count(1); std::vector<std::uint8_t> v(n); raw(v.data(), n); return v; }
    std::vector<int> intList() { const std::size_t n = count(4); std::vector<int> v(n); raw(v.data(), n * 4); return v; }
    scene::Transform transform() { scene::Transform t; t.m = floats<12>(); return t; }
    std::size_t count(std::size_t unit) {
        const std::size_t n = u32();
        if (n * unit > data_.size() - pos_) {
            throw std::runtime_error("baked scene: bad count");
        }
        return n;
    }
    void raw(void* p, std::size_t n) {
        if (n > data_.size() - pos_) {
            throw std::runtime_error("baked scene: truncated");
        }
        std::memcpy(p, data_.data() + pos_, n);
        pos_ += n;
    }
    void skip(std::size_t n) {
        if (n > data_.size() - pos_) {
            throw std::runtime_error("baked scene: truncated");
        }
        pos_ += n;
    }
    std::size_t position() const { return pos_; }
    void align4() { skip((4 - pos_ % 4) % 4); }
    const std::uint8_t* here() const { return data_.data() + pos_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

void writeLighting(Out& o, const scene::ObjectLighting& l) {
    o.floats(l.ambient);
    o.u32(static_cast<std::uint32_t>(l.lights.size()));
    for (const auto& x : l.lights) {
        o.floats(x.direction);
        o.floats(x.color);
        o.flag(x.sun);
        o.flag(x.point);
        o.floats(x.position);
        o.f32(x.falloff);
        o.f32(x.range);
        o.flag(x.spot);
        o.f32(x.coneInner);
        o.f32(x.coneOuter);
    }
    o.f32(l.thumbnailScale);
}

scene::ObjectLighting readLighting(In& in) {
    scene::ObjectLighting l;
    l.ambient = in.floats<3>();
    l.lights.resize(in.count(60));
    for (auto& x : l.lights) {
        x.direction = in.floats<3>();
        x.color = in.floats<3>();
        x.sun = in.flag();
        x.point = in.flag();
        x.position = in.floats<3>();
        x.falloff = in.f32();
        x.range = in.f32();
        x.spot = in.flag();
        x.coneInner = in.f32();
        x.coneOuter = in.f32();
    }
    l.thumbnailScale = in.f32();
    return l;
}

void writeMaterial(Out& o, const scene::Material& m) {
    o.str(m.name);
    o.floats(m.color);
    o.i32(m.image);
    o.u32(static_cast<std::uint32_t>(m.alpha));
    o.f32(m.alphaReference);
    o.flag(m.doubleSided);
    o.flag(m.unlit);
    o.flag(m.nightDimmed);
    o.flag(m.vertexColor);
    o.flag(m.clamp);
    o.i32(m.layer);
    o.flag(m.roofLight);
    o.floats(m.floorTiles);
    o.flag(m.simLighting);
    o.floats(m.emissive);
}

scene::Material readMaterial(In& in) {
    scene::Material m;
    m.name = in.str();
    m.color = in.floats<4>();
    m.image = in.i32();
    m.alpha = static_cast<scene::AlphaMode>(in.u32());
    m.alphaReference = in.f32();
    m.doubleSided = in.flag();
    m.unlit = in.flag();
    m.nightDimmed = in.flag();
    m.vertexColor = in.flag();
    m.clamp = in.flag();
    m.layer = in.i32();
    m.roofLight = in.flag();
    m.floorTiles = in.floats<2>();
    m.simLighting = in.flag();
    m.emissive = in.floats<3>();
    return m;
}

// A list of floats as 16-bit values across their own range (a mesh's positions, texture coordinates): the range (min and
// max, 8 bytes), then a u16 for each.
void writeQuantized(Out& o, const std::vector<float>& v) {
    o.align4();
    o.u32(static_cast<std::uint32_t>(v.size()));
    if (v.empty()) {
        return;
    }
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    o.f32(*lo);
    o.f32(*hi);
    const float range = *hi - *lo;
    for (const float f : v) {
        o.u16(range > 0 ? static_cast<std::uint16_t>(std::lround((f - *lo) / range * 65535.0f)) : 0);
    }
}

std::vector<float> readQuantized(In& in) {
    in.align4();
    std::vector<float> v(in.count(2));
    if (v.empty()) {
        return v;
    }
    const float lo = in.f32(), hi = in.f32();
    for (auto& f : v) {
        f = lo + (hi - lo) * (static_cast<float>(in.u16()) / 65535.0f);
    }
    return v;
}

// Floats in -1..1 (normals) or 0..1 (colours, alphas, light-map coordinates) as bytes or 16-bit values.
void writeUnit(Out& o, const std::vector<float>& v, bool signedValues, bool wide) {
    o.align4();
    o.u32(static_cast<std::uint32_t>(v.size()));
    for (const float f : v) {
        const float c = std::clamp(f, signedValues ? -1.0f : 0.0f, 1.0f);
        if (wide) {
            o.u16(static_cast<std::uint16_t>(std::lround(c * 65535.0f)));
        } else if (signedValues) {
            o.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(c * 127.0f))));
        } else {
            o.u8(static_cast<std::uint8_t>(std::lround(c * 255.0f)));
        }
    }
}

std::vector<float> readUnit(In& in, bool signedValues, bool wide) {
    in.align4();
    std::vector<float> v(in.count(wide ? 2 : 1));
    for (auto& f : v) {
        if (wide) {
            f = static_cast<float>(in.u16()) / 65535.0f;
        } else if (signedValues) {
            f = static_cast<float>(static_cast<std::int8_t>(in.u8())) / 127.0f;
        } else {
            f = static_cast<float>(in.u8()) / 255.0f;
        }
    }
    return v;
}

// Positions as 16-bit values over each axis's own range: the count of floats, then the lowest and highest x, y and z (24 bytes), then
// a u16 each.
void writePositions(Out& o, const std::vector<float>& v) {
    o.align4();
    o.u32(static_cast<std::uint32_t>(v.size()));
    if (v.empty()) {
        return;
    }
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        lo[k] = hi[k] = v[static_cast<std::size_t>(k)];
    }
    for (std::size_t i = 0; i + 2 < v.size(); i += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], v[i + k]);
            hi[k] = std::max(hi[k], v[i + k]);
        }
    }
    for (int k = 0; k < 3; ++k) {
        o.f32(lo[k]);
        o.f32(hi[k]);
    }
    for (std::size_t i = 0; i + 2 < v.size(); i += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            const float range = hi[k] - lo[k];
            o.u16(range > 0 ? static_cast<std::uint16_t>(std::lround((v[i + k] - lo[k]) / range * 65535.0f)) : 0);
        }
    }
}

std::vector<float> readPositions(In& in) {
    in.align4();
    std::vector<float> v(in.count(2));
    if (v.empty()) {
        return v;
    }
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        lo[k] = in.f32();
        hi[k] = in.f32();
    }
    for (std::size_t i = 0; i + 2 < v.size(); i += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            v[i + k] = lo[k] + (hi[k] - lo[k]) * (static_cast<float>(in.u16()) / 65535.0f);
        }
    }
    return v;
}

void writePrimitive(Out& o, const scene::Primitive& p) {
    o.align4();
    o.i32(p.material);
    o.str(p.group);
    writePositions(o, p.positions);
    writeUnit(o, p.normals, true, false);
    writeQuantized(o, p.uvs);
    writeUnit(o, p.alphas, false, false);
    writeUnit(o, p.colors, false, false);
    writeUnit(o, p.lightUvs, false, true);
    writeUnit(o, p.weights, false, false);
    o.align4();
    o.bytesList(p.joints);
    // Indices fit 16 bits for nearly everything.
    const bool wide = !p.positions.empty() && p.positions.size() / 3 > 65535;
    o.align4();
    o.u32(wide ? 1 : 0);
    o.u32(static_cast<std::uint32_t>(p.indices.size()));
    for (const std::uint32_t i : p.indices) {
        if (wide) {
            o.u32(i);
        } else {
            o.u16(static_cast<std::uint16_t>(i));
        }
    }
    o.align4();
    o.floats(p.origin);
}

scene::Primitive readPrimitive(In& in) {
    scene::Primitive p;
    in.align4();
    p.material = in.i32();
    p.group = in.str();
    p.positions = readPositions(in);
    p.normals = readUnit(in, true, false);
    p.uvs = readQuantized(in);
    p.alphas = readUnit(in, false, false);
    p.colors = readUnit(in, false, false);
    p.lightUvs = readUnit(in, false, true);
    p.weights = readUnit(in, false, false);
    in.align4();
    p.joints = in.bytesList();
    in.align4();
    const bool wide = in.u32() != 0;
    p.indices.resize(in.count(wide ? 4 : 2));
    for (auto& i : p.indices) {
        i = wide ? in.u32() : in.u16();
    }
    in.align4();
    p.origin = in.floats<3>();
    return p;
}

void writeInstance(Out& o, const scene::Instance& x) {
    o.i32(x.mesh);
    o.str(x.name);
    o.transform(x.transform);
    o.u32(static_cast<std::uint32_t>(x.part));
    o.i32(x.level);
    o.u32(x.object);
    o.flag(x.cut);
    o.intList(x.walls);
    o.i32(x.lightMap);
    o.i32(x.incidenceMap);
    o.f32(x.lightMapGain);
    o.flag(x.lightMapOnly);
    o.flag(x.lightMapMultiply);
    o.f32(x.skirtAmbient);
    o.flag(x.skirtRoad);
    o.floats(x.skirtScale);
    o.u32(static_cast<std::uint32_t>(x.sunTest));
    o.floatList(x.sun);
    o.flag(x.litOutdoors);
    o.u32(static_cast<std::uint32_t>(x.lightGroup));
    o.i32(x.lightSet);
    o.intList(x.partLightSets);
}

scene::Instance readInstance(In& in) {
    scene::Instance x;
    x.mesh = in.i32();
    x.name = in.str();
    x.transform = in.transform();
    x.part = static_cast<scene::Part>(in.u32());
    x.level = in.i32();
    x.object = in.u32();
    x.cut = in.flag();
    x.walls = in.intList();
    x.lightMap = in.i32();
    x.incidenceMap = in.i32();
    x.lightMapGain = in.f32();
    x.lightMapOnly = in.flag();
    x.lightMapMultiply = in.flag();
    x.skirtAmbient = in.f32();
    x.skirtRoad = in.flag();
    x.skirtScale = in.floats<3>();
    x.sunTest = static_cast<scene::SunTest>(in.u32());
    x.sun = in.floatList();
    x.litOutdoors = in.flag();
    x.lightGroup = static_cast<scene::LightGroup>(in.u32());
    x.lightSet = in.i32();
    x.partLightSets = in.intList();
    return x;
}

// The primitive with identical vertices (all of their data equal) made one, and the indices following. Meshes come with a vertex for each
// corner of a face when the modeller left them unwelded; the console pays for a vertex every frame, a triangle's indices only once.
scene::Primitive weldedPrimitive(const scene::Primitive& p) {
    const std::size_t n = p.positions.size() / 3;
    if (n == 0 || p.indices.empty()) {
        return p;
    }
    struct Part {
        const std::vector<float>* data;
        std::size_t stride;
    };
    const Part floats[] = {{&p.positions, 3}, {&p.normals, 3}, {&p.uvs, 2}, {&p.alphas, 1}, {&p.colors, 3}, {&p.lightUvs, 2}, {&p.tangents, 3}, {&p.weights, 4}};
    std::size_t stride = 0;
    for (const Part& part : floats) {
        stride += part.data->size() == n * part.stride ? part.stride : 0;
    }
    const bool joints = p.joints.size() == n * 4;
    stride += joints ? 4 : 0;
    std::vector<float> rows(n * stride);
    for (std::size_t v = 0; v < n; ++v) {
        float* row = &rows[v * stride];
        for (const Part& part : floats) {
            if (part.data->size() == n * part.stride) {
                row = std::copy_n(part.data->begin() + static_cast<std::ptrdiff_t>(v * part.stride), part.stride, row);
            }
        }
        if (joints) {
            for (std::size_t j = 0; j < 4; ++j) {
                *row++ = p.joints[v * 4 + j];
            }
        }
    }
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> buckets;
    std::vector<std::uint32_t> remap(n);
    std::vector<std::uint32_t> keep;
    for (std::size_t v = 0; v < n; ++v) {
        std::uint64_t h = 0xCBF29CE484222325ull;
        const auto* words = reinterpret_cast<const std::uint32_t*>(&rows[v * stride]);
        for (std::size_t k = 0; k < stride; ++k) {
            h = (h ^ words[k]) * 0x100000001B3ull;
        }
        auto& bucket = buckets[h];
        std::uint32_t found = 0xFFFFFFFFu;
        for (const std::uint32_t other : bucket) {
            if (std::memcmp(&rows[keep[other] * stride], &rows[v * stride], stride * sizeof(float)) == 0) {
                found = other;
                break;
            }
        }
        if (found == 0xFFFFFFFFu) {
            found = static_cast<std::uint32_t>(keep.size());
            keep.push_back(static_cast<std::uint32_t>(v));
            bucket.push_back(found);
        }
        remap[v] = found;
    }
    if (keep.size() == n) {
        return p;
    }
    scene::Primitive q = p;
    for (auto& i : q.indices) {
        i = i < n ? remap[i] : i;
    }
    const auto gather = [&](const std::vector<float>& from, std::size_t perVertex) {
        std::vector<float> out;
        if (from.size() != n * perVertex) {
            return from;
        }
        out.reserve(keep.size() * perVertex);
        for (const std::uint32_t v : keep) {
            out.insert(out.end(), from.begin() + static_cast<std::ptrdiff_t>(v * perVertex), from.begin() + static_cast<std::ptrdiff_t>((v + 1) * perVertex));
        }
        return out;
    };
    q.positions = gather(p.positions, 3);
    q.normals = gather(p.normals, 3);
    q.uvs = gather(p.uvs, 2);
    q.alphas = gather(p.alphas, 1);
    q.colors = gather(p.colors, 3);
    q.lightUvs = gather(p.lightUvs, 2);
    q.tangents = gather(p.tangents, 3);
    q.weights = gather(p.weights, 4);
    if (joints) {
        q.joints.clear();
        for (const std::uint32_t v : keep) {
            q.joints.insert(q.joints.end(), p.joints.begin() + static_cast<std::ptrdiff_t>(v * 4), p.joints.begin() + static_cast<std::ptrdiff_t>(v * 4 + 4));
        }
    }
    return q;
}

// The primitive with fewer triangles, its per-vertex data carried along to the vertices that are left.
scene::Primitive reducedPrimitive(const scene::Primitive& p, const ConvertOptions& options) {
    const std::size_t vertices = p.positions.size() / 3;
    if (vertices == 0 || p.indices.size() < 72) {
        return p;
    }
    content::IndexedReduction r = content::reduceIndexed(p.positions, p.indices, options.meshKeep, options.meshError);
    if (!r.reduced) {
        return p;
    }
    std::size_t kept = 0;
    for (const auto v : r.remap) {
        kept = std::max<std::size_t>(kept, v < 0 ? 0 : static_cast<std::size_t>(v) + 1);
    }
    scene::Primitive q = p;
    q.indices = std::move(r.indices);
    const auto carry = [&](const std::vector<float>& from, std::size_t stride) {
        std::vector<float> out;
        if (from.size() != vertices * stride) {
            return from.empty() ? out : from;  // not per vertex as expected: leave as it is
        }
        out.resize(kept * stride);
        for (std::size_t v = 0; v < vertices; ++v) {
            if (r.remap[v] >= 0) {
                std::copy_n(&from[v * stride], stride, &out[static_cast<std::size_t>(r.remap[v]) * stride]);
            }
        }
        return out;
    };
    q.positions = carry(p.positions, 3);
    q.normals = carry(p.normals, 3);
    q.uvs = carry(p.uvs, 2);
    q.alphas = carry(p.alphas, 1);
    q.colors = carry(p.colors, 3);
    q.lightUvs = carry(p.lightUvs, 2);
    q.tangents = carry(p.tangents, 3);
    q.weights = carry(p.weights, 4);
    if (p.joints.size() == vertices * 4) {
        q.joints.assign(kept * 4, 0);
        for (std::size_t v = 0; v < vertices; ++v) {
            if (r.remap[v] >= 0) {
                std::copy_n(&p.joints[v * 4], 4, &q.joints[static_cast<std::size_t>(r.remap[v]) * 4]);
            }
        }
    }
    return q;
}

} // namespace

std::vector<std::uint8_t> bakeScene(const scene::Scene& scene, const ConvertOptions& options, BakeStats* stats) {
    BakeStats local;
    BakeStats& st = stats ? *stats : local;
    // Pictures that change (light maps) or that the console doesn't use (normal maps).
    std::set<int> lightMaps, normalMaps;
    for (const scene::Instance& x : scene.instances) {
        if (x.lightMap >= 0) lightMaps.insert(x.lightMap);
        if (x.incidenceMap >= 0) normalMaps.insert(x.incidenceMap);
    }
    for (const scene::Material& m : scene.materials) {
        if (m.normalMap >= 0) normalMaps.insert(m.normalMap);
    }
    if (scene.lightMaps) {
        for (const int i : scene.lightMaps->floors) lightMaps.insert(i);
        if (scene.lightMaps->walls >= 0) lightMaps.insert(scene.lightMaps->walls);
    }

    Out o;
    std::vector<std::vector<std::uint8_t>> blobs;
    o.flag(scene.objectLighting.has_value());
    if (scene.objectLighting) writeLighting(o, *scene.objectLighting);
    o.u32(static_cast<std::uint32_t>(scene.lightSets.size()));
    for (const auto& l : scene.lightSets) writeLighting(o, l);
    o.floats(scene.lotSize);
    o.i32(scene.groundLevel);
    o.u32(static_cast<std::uint32_t>(scene.lightMaps ? scene.lightMaps->floors.size() : 0));
    if (scene.lightMaps && !scene.lightMaps->floors.empty()) {
        for (const int i : scene.lightMaps->floors) o.i32(i);
        o.i32(scene.lightMaps->lowest);
        o.i32(scene.lightMaps->walls);
    }
    const std::size_t afterHeader = o.bytes.size();

    o.u32(static_cast<std::uint32_t>(scene.images.size()));
    const std::size_t imageTable = o.bytes.size();
    o.bytes.resize(o.bytes.size() + scene.images.size() * 16);  // each image's offset, size, width and height, filled in below
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        const scene::Image& image = scene.images[i];
        content::ConsoleTexture texture;
        if (normalMaps.contains(static_cast<int>(i)) || image.rgba.empty()) {
            texture.width = texture.height = 1;
            texture.bits = 32;
            texture.texels = {255, 255, 255, 255};
        } else if (lightMaps.contains(static_cast<int>(i))) {
            ConvertOptions plain = options;
            plain.palettes = false;
            plain.mips = false;
            plain.maxSide = 1024;
            texture = convertTexture(image, plain).texture;
        } else {
            texture = convertTexture(image, options).texture;
        }
        ++st.byBits[texture.bits == 4 ? 0 : texture.bits == 8 ? 1 : 2];
        blobs.push_back(texture.serialize());
        st.imageBytes += blobs.back().size();
        ++st.images;
    }

    o.u32(static_cast<std::uint32_t>(scene.materials.size()));
    for (scene::Material m : scene.materials) {
        if (m.normalMap >= 0) m.normalMap = -1;
        writeMaterial(o, m);
    }
    o.u32(static_cast<std::uint32_t>(scene.meshes.size()));
    const std::size_t beforeMeshes = o.bytes.size();
    for (const scene::Mesh& mesh : scene.meshes) {
        o.str(mesh.name);
        o.u32(static_cast<std::uint32_t>(mesh.primitives.size()));
        for (const auto& original : mesh.primitives) {
            const scene::Primitive welded = options.weld ? weldedPrimitive(original) : original;
            const scene::Primitive p = options.meshKeep < 1 ? reducedPrimitive(welded, options) : welded;
            writePrimitive(o, p);
            ++st.primitives;
            st.trianglesBefore += original.indices.size() / 3;
            st.triangles += p.indices.size() / 3;
            st.verticesBefore += original.positions.size() / 3;
            st.vertices += p.positions.size() / 3;
        }
        o.u32(static_cast<std::uint32_t>(mesh.skin.size()));
        for (const auto& j : mesh.skin) {
            o.str(j.joint);
            o.transform(j.inverseBind);
            o.transform(j.placement);
            o.flag(j.rest.has_value());
            if (j.rest) o.transform(*j.rest);
        }
        ++st.meshes;
    }
    st.meshBytes = o.bytes.size() - beforeMeshes;
    o.u32(static_cast<std::uint32_t>(scene.instances.size()));
    for (const auto& x : scene.instances) {
        writeInstance(o, x);
        ++st.instances;
    }
    st.otherBytes = o.bytes.size() - st.meshBytes - scene.images.size() * 16;
    (void)afterHeader;
    // The images follow the head, each starting on a 16-byte boundary (the DMA reads them).
    std::uint32_t at = 0;
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        const auto info = content::ConsoleTextureView::of(blobs[i]);
        const std::uint32_t fields[4] = {at, static_cast<std::uint32_t>(blobs[i].size()), info.width, info.height};
        std::memcpy(&o.bytes[imageTable + i * 16], fields, 16);
        at += static_cast<std::uint32_t>((blobs[i].size() + 15) / 16 * 16);
    }
    // The images start on a 16-byte boundary of the file: the head is padded to end on one.
    o.bytes.resize(o.bytes.size() + (16 - (kBakedPreambleSize + o.bytes.size()) % 16) % 16, 0);
    std::vector<std::uint8_t> file;
    const std::uint32_t preamble[3] = {kMagic, kVersion, static_cast<std::uint32_t>(o.bytes.size())};
    file.resize(kBakedPreambleSize);
    std::memcpy(file.data(), preamble, 12);
    file.insert(file.end(), o.bytes.begin(), o.bytes.end());
    for (const auto& blob : blobs) {
        file.insert(file.end(), blob.begin(), blob.end());
        file.resize((file.size() + 15) / 16 * 16, 0);
    }
    return file;
}

namespace {

// Reads the head from `in`, which stands just after the preamble; the images' offsets are made absolute with `imageArea`.
// Reads the head's one primitive without expanding it: what a draw needs to know, and where its bytes end.
void scanPrimitive(In& in, std::size_t base, BakedPrimitive& out) {
    in.align4();
    const std::size_t start = in.position();
    out.material = in.i32();
    in.skip(in.count(1));  // the group's name
    in.align4();
    const std::size_t floatsOfPositions = in.count(2);
    if (floatsOfPositions > 0) {
        float range[6];
        for (auto& r : range) {
            r = in.f32();
        }
        for (std::size_t k = 0; k < 3; ++k) {
            out.low[k] = range[k * 2];
            out.high[k] = range[k * 2 + 1];
        }
        in.skip(floatsOfPositions * 2);
    }
    out.vertices = static_cast<std::uint32_t>(floatsOfPositions / 3);
    const std::size_t n = out.vertices;
    in.align4();
    in.skip(in.count(1));  // normals
    in.align4();
    if (const std::size_t uv = in.count(2); uv > 0) {
        in.skip(8 + uv * 2);
    }
    in.align4();
    in.skip(in.count(1));  // alphas
    in.align4();
    in.skip(in.count(1));  // colours
    in.align4();
    const std::size_t lightUvs = in.count(2);
    in.skip(lightUvs * 2);
    out.lightMapped = n > 0 && lightUvs == n * 2;
    in.align4();
    const std::size_t weights = in.count(1);
    in.skip(weights);
    in.align4();
    const std::size_t joints = in.count(1);
    in.skip(joints);
    out.skinned = n > 0 && weights == n * 4 && joints == n * 4;
    in.align4();
    const bool wide = in.u32() != 0;
    out.indices = static_cast<std::uint32_t>(in.count(wide ? 4 : 2));
    in.skip(std::size_t{out.indices} * (wide ? 4 : 2));
    in.align4();
    in.skip(12);  // the origin
    out.offset = base + start;
    out.size = in.position() - start;
}

scene::Scene readHead(In& in, std::uint64_t imageArea, BakedMeshes* compact = nullptr, std::size_t base = 0) {
    scene::Scene s;
    if (in.flag()) s.objectLighting = readLighting(in);
    s.lightSets.resize(in.count(40));
    for (auto& l : s.lightSets) l = readLighting(in);
    s.lotSize = in.floats<2>();
    s.groundLevel = in.i32();
    if (const std::size_t floors = in.count(4); floors > 0) {
        scene::LightMaps maps;
        for (std::size_t i = 0; i < floors; ++i) maps.floors.push_back(in.i32());
        maps.lowest = in.i32();
        maps.walls = in.i32();
        s.lightMaps = std::move(maps);
    }
    s.images.resize(in.count(16));
    for (auto& image : s.images) {
        image.consoleOffset = imageArea + in.u32();
        image.consoleSize = in.u32();
        image.width = in.u32();
        image.height = in.u32();
    }
    s.materials.resize(in.count(40));
    for (auto& m : s.materials) m = readMaterial(in);
    const std::size_t meshCount = in.count(8);
    if (compact) {
        compact->meshes.resize(meshCount);
    } else {
        s.meshes.resize(meshCount);
    }
    for (std::size_t m = 0; m < meshCount; ++m) {
        scene::Mesh scratch;
        scene::Mesh& mesh = compact ? scratch : s.meshes[m];
        mesh.name = in.str();
        const std::size_t primitives = in.count(20);
        if (compact) {
            compact->meshes[m].resize(primitives);
            for (auto& p : compact->meshes[m]) scanPrimitive(in, base, p);
        } else {
            mesh.primitives.resize(primitives);
            for (auto& p : mesh.primitives) p = readPrimitive(in);
        }
        mesh.skin.resize(in.count(100));
        for (auto& j : mesh.skin) {
            j.joint = in.str();
            j.inverseBind = in.transform();
            j.placement = in.transform();
            if (in.flag()) j.rest = in.transform();
        }
    }
    s.instances.resize(in.count(100));
    for (auto& x : s.instances) x = readInstance(in);
    return s;
}

std::uint32_t checkedHeadSize(std::span<const std::uint8_t> preamble) {
    std::uint32_t fields[3];
    if (preamble.size() < kBakedPreambleSize) {
        throw std::runtime_error("not a baked scene: too short");
    }
    std::memcpy(fields, preamble.data(), 12);
    if (fields[0] != kMagic || fields[1] != kVersion) {
        throw std::runtime_error("not a baked scene");
    }
    return fields[2];
}

} // namespace

std::size_t bakedHeadSize(std::span<const std::uint8_t> preamble) {
    return kBakedPreambleSize + checkedHeadSize(preamble);
}

scene::Scene readBakedHead(std::span<const std::uint8_t> head) {
    const std::uint32_t size = checkedHeadSize(head);
    if (head.size() < kBakedPreambleSize + size) {
        throw std::runtime_error("baked scene: head truncated");
    }
    In in(head.subspan(kBakedPreambleSize, size));
    return readHead(in, kBakedPreambleSize + size);
}

scene::Scene readBakedHeadCompact(std::vector<std::uint8_t> head, BakedMeshes& meshes) {
    const std::uint32_t size = checkedHeadSize(head);
    if (head.size() < kBakedPreambleSize + size) {
        throw std::runtime_error("baked scene: head truncated");
    }
    meshes.bytes = std::move(head);
    In in(std::span<const std::uint8_t>(meshes.bytes).subspan(kBakedPreambleSize, size));
    return readHead(in, kBakedPreambleSize + size, &meshes, kBakedPreambleSize);
}

void viewPrimitive(const BakedMeshes& meshes, const BakedPrimitive& primitive, PrimitiveView& v) {
    In in(std::span<const std::uint8_t>(meshes.bytes).subspan(primitive.offset, primitive.size));
    v = PrimitiveView{};
    in.align4();
    in.skip(4);                 // the material
    in.skip(in.count(1));       // the group's name
    in.align4();
    const std::size_t floatsOfPositions = in.count(2);
    v.vertices = static_cast<std::uint32_t>(floatsOfPositions / 3);
    if (floatsOfPositions > 0) {
        for (int k = 0; k < 3; ++k) {
            v.lo[k] = in.f32();
            v.hi[k] = in.f32();
        }
        v.positions = reinterpret_cast<const std::uint16_t*>(in.here());
        in.skip(floatsOfPositions * 2);
    }
    in.align4();
    const std::size_t normals = in.count(1);
    v.normals = normals == floatsOfPositions && normals > 0 ? reinterpret_cast<const std::int8_t*>(in.here()) : nullptr;
    in.skip(normals);
    in.align4();
    if (const std::size_t uv = in.count(2); uv > 0) {
        v.uvLo = in.f32();
        v.uvHi = in.f32();
        v.uvs = uv == std::size_t{v.vertices} * 2 ? reinterpret_cast<const std::uint16_t*>(in.here()) : nullptr;
        in.skip(uv * 2);
    }
    in.align4();
    const std::size_t alphas = in.count(1);
    v.alphas = alphas == v.vertices && alphas > 0 ? in.here() : nullptr;
    in.skip(alphas);
    in.align4();
    const std::size_t colors = in.count(1);
    v.colors = colors == floatsOfPositions && colors > 0 ? in.here() : nullptr;
    in.skip(colors);
    in.align4();
    const std::size_t lightUvs = in.count(2);
    v.lightUvs = lightUvs == std::size_t{v.vertices} * 2 && lightUvs > 0 ? reinterpret_cast<const std::uint16_t*>(in.here()) : nullptr;
    in.skip(lightUvs * 2);
    in.align4();
    in.skip(in.count(1));  // weights
    in.align4();
    in.skip(in.count(1));  // joints
    in.align4();
    v.wideIndices = in.u32() != 0;
    v.indices = static_cast<std::uint32_t>(in.count(v.wideIndices ? 4 : 2));
    v.indexData = in.here();
}

void expandPrimitive(const BakedMeshes& meshes, const BakedPrimitive& primitive, ExpandedPrimitive& out) {
    In in(std::span<const std::uint8_t>(meshes.bytes).subspan(primitive.offset, primitive.size));
    const scene::Primitive p = readPrimitive(in);
    const std::size_t n = p.positions.size() / 3;
    out.vertices.resize(n * kVertexFloats);
    packPrimitiveVertices(p, out.vertices.data());
    out.indices = p.indices;
}

scene::Scene readBakedScene(std::span<const std::uint8_t> bytes) {
    scene::Scene s = readBakedHead(bytes);
    for (auto& image : s.images) {
        if (image.consoleOffset + image.consoleSize > bytes.size()) {
            throw std::runtime_error("baked scene: image past the end");
        }
        image.console.assign(bytes.begin() + static_cast<std::ptrdiff_t>(image.consoleOffset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(image.consoleOffset + image.consoleSize));
        image.consoleSize = 0;
    }
    return s;
}

} // namespace ts2::render

#include "render/console_scene.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace ts2::render {

namespace {

constexpr std::uint32_t kMagic = 0x31435343;  // "CSC1"
constexpr std::uint32_t kVersion = 1;

class Out {
public:
    void u8(std::uint8_t v) { bytes.push_back(v); }
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

void writePrimitive(Out& o, const scene::Primitive& p) {
    o.i32(p.material);
    o.str(p.group);
    o.floatList(p.positions);
    o.floatList(p.normals);
    o.floatList(p.uvs);
    o.floatList(p.alphas);
    o.floatList(p.colors);
    o.floatList(p.lightUvs);
    o.floatList(p.weights);
    o.bytesList(p.joints);
    // Indices fit 16 bits for nearly everything.
    const bool wide = !p.positions.empty() && p.positions.size() / 3 > 65535;
    o.flag(wide);
    o.u32(static_cast<std::uint32_t>(p.indices.size()));
    for (const std::uint32_t i : p.indices) {
        if (wide) {
            o.u32(i);
        } else {
            o.u8(static_cast<std::uint8_t>(i));
            o.u8(static_cast<std::uint8_t>(i >> 8));
        }
    }
    o.floats(p.origin);
}

scene::Primitive readPrimitive(In& in) {
    scene::Primitive p;
    p.material = in.i32();
    p.group = in.str();
    p.positions = in.floatList();
    p.normals = in.floatList();
    p.uvs = in.floatList();
    p.alphas = in.floatList();
    p.colors = in.floatList();
    p.lightUvs = in.floatList();
    p.weights = in.floatList();
    p.joints = in.bytesList();
    const bool wide = in.flag();
    p.indices.resize(in.count(wide ? 4 : 2));
    for (auto& i : p.indices) {
        if (wide) {
            i = in.u32();
        } else {
            const std::uint32_t lo = in.u8();
            i = lo | (static_cast<std::uint32_t>(in.u8()) << 8);
        }
    }
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
    o.u32(kMagic);
    o.u32(kVersion);
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
        const auto blob = texture.serialize();
        o.bytesList(blob);
        st.imageBytes += blob.size();
        ++st.images;
    }
    const std::size_t afterImages = o.bytes.size();

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
        for (const auto& p : mesh.primitives) {
            writePrimitive(o, p);
            ++st.primitives;
            st.triangles += p.indices.size() / 3;
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
    st.otherBytes = o.bytes.size() - st.meshBytes - st.imageBytes;
    (void)afterHeader;
    (void)afterImages;
    return std::move(o.bytes);
}

scene::Scene readBakedScene(std::span<const std::uint8_t> bytes) {
    In in(bytes);
    if (in.u32() != kMagic || in.u32() != kVersion) {
        throw std::runtime_error("not a baked scene");
    }
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
    s.images.resize(in.count(4));
    for (auto& image : s.images) {
        image.console = in.bytesList();
        const auto texture = content::ConsoleTexture::parse(image.console);
        image.width = texture.width;
        image.height = texture.height;
    }
    s.materials.resize(in.count(40));
    for (auto& m : s.materials) m = readMaterial(in);
    s.meshes.resize(in.count(8));
    for (auto& mesh : s.meshes) {
        mesh.name = in.str();
        mesh.primitives.resize(in.count(20));
        for (auto& p : mesh.primitives) p = readPrimitive(in);
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

} // namespace ts2::render

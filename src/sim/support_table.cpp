#include "sim/support_table.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kMagic = 0x31505553;  // "SUP1"

class Out {
public:
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void flag(bool v) { u8(v ? 1 : 0); }
    void str(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); raw(s.data(), s.size()); }
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    template <class T, class F>
    void optional(const std::optional<T>& v, F&& write) {
        flag(v.has_value());
        if (v) {
            write(*v);
        }
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
    std::size_t count(std::size_t unit) {
        const std::size_t n = u32();
        if (n * unit > data_.size() - pos_) {
            throw std::runtime_error("support table: bad count");
        }
        return n;
    }
    std::string str() { const std::size_t n = count(1); std::string s(n, '\0'); raw(s.data(), n); return s; }
    void raw(void* p, std::size_t n) {
        if (n > data_.size() - pos_) {
            throw std::runtime_error("support table: truncated");
        }
        std::memcpy(p, data_.data() + pos_, n);
        pos_ += n;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

void writeRect(Out& o, const RouteRect& r) {
    o.f32(r.x0);
    o.f32(r.y0);
    o.f32(r.x1);
    o.f32(r.y1);
}

RouteRect readRect(In& in) {
    RouteRect r;
    r.x0 = in.f32();
    r.y0 = in.f32();
    r.x1 = in.f32();
    r.y1 = in.f32();
    return r;
}

void writeValue(Out& o, const scenegraph::DataValue& v) {
    o.u8(static_cast<std::uint8_t>(v.type));
    o.str(v.name);
    o.i32(v.integer);
    o.u32(static_cast<std::uint32_t>(v.floats.size()));
    for (const float f : v.floats) {
        o.f32(f);
    }
    o.str(v.text);
    o.u32(static_cast<std::uint32_t>(v.bytes.size()));
    o.raw(v.bytes.data(), v.bytes.size());
    o.u32(static_cast<std::uint32_t>(v.items.size()));
    for (const auto& item : v.items) {
        writeValue(o, item);
    }
}

scenegraph::DataValue readValue(In& in, int depth = 0) {
    if (depth > 16) {
        throw std::runtime_error("support table: data nested too deep");
    }
    scenegraph::DataValue v;
    v.type = static_cast<scenegraph::DataValue::Type>(in.u8());
    v.name = in.str();
    v.integer = in.i32();
    v.floats.resize(in.count(4));
    for (auto& f : v.floats) {
        f = in.f32();
    }
    v.text = in.str();
    v.bytes.resize(in.count(1));
    in.raw(v.bytes.data(), v.bytes.size());
    v.items.resize(in.count(10));
    for (auto& item : v.items) {
        item = readValue(in, depth + 1);
    }
    return v;
}

void writeModel(Out& o, const RouteModel& m) {
    o.optional(m.footprint, [&](const auto& v) { writeValue(o, v); });
    o.optional(m.extent, [&](const auto& r) { writeRect(o, r); });
    o.u32(static_cast<std::uint32_t>(m.bones.size()));
    for (const auto& [name, bone] : m.bones) {
        o.str(name);
        o.f32(bone.x);
        o.f32(bone.y);
        o.f32(bone.yaw);
    }
    o.u32(static_cast<std::uint32_t>(m.nodes.size()));
    for (const auto& n : m.nodes) {
        o.str(n.name);
        o.i32(n.parent);
        for (const float t : n.translation) o.f32(t);
        for (const float q : n.rotation) o.f32(q);
    }
}

RouteModel readModel(In& in) {
    RouteModel m;
    if (in.flag()) m.footprint = readValue(in);
    if (in.flag()) m.extent = readRect(in);
    for (std::size_t i = in.count(16); i > 0; --i) {
        std::string name = in.str();
        ModelBone b;
        b.x = in.f32();
        b.y = in.f32();
        b.yaw = in.f32();
        m.bones.emplace(std::move(name), b);
    }
    m.nodes.resize(in.count(40));
    for (auto& n : m.nodes) {
        n.name = in.str();
        n.parent = in.i32();
        for (float& t : n.translation) t = in.f32();
        for (float& q : n.rotation) q = in.f32();
    }
    return m;
}

} // namespace

std::vector<std::uint8_t> writeSupportTable(const SupportTable& t) {
    Out o;
    o.u32(kMagic);
    o.u32(static_cast<std::uint32_t>(t.routeModels.size()));
    for (const auto& [name, model] : t.routeModels) {
        o.str(name);
        writeModel(o, model);
    }
    o.u32(static_cast<std::uint32_t>(t.posts.size()));
    for (const auto& [guid, rect] : t.posts) {
        o.u32(guid);
        o.optional(rect, [&](const auto& r) { writeRect(o, r); });
    }
    o.u32(static_cast<std::uint32_t>(t.fenceBlocks.size()));
    for (const auto& [guid, blocks] : t.fenceBlocks) {
        o.u32(guid);
        o.flag(blocks);
    }
    o.u32(static_cast<std::uint32_t>(t.animationLengths.size()));
    for (const auto& [name, length] : t.animationLengths) {
        o.str(name);
        o.optional(length, [&](float v) { o.f32(v); });
    }
    o.u32(static_cast<std::uint32_t>(t.animationCarries.size()));
    for (const auto& [name, carries] : t.animationCarries) {
        o.str(name);
        o.flag(carries);
    }
    o.u32(static_cast<std::uint32_t>(t.locoClips.size()));
    for (const auto& [name, clip] : t.locoClips) {
        o.str(name);
        o.optional(clip, [&](const LocoClip& c) {
            o.f32(c.lengthMs);
            o.f32(c.travel);
            o.f32(c.loopPhase);
            o.f32(c.match);
            o.f32(c.speed);
            o.f32(c.rootTurn);
        });
    }
    o.u32(static_cast<std::uint32_t>(t.eventTimes.size()));
    for (const auto& [key, time] : t.eventTimes) {
        o.str(key.first);
        o.i32(key.second);
        o.optional(time, [&](float v) { o.f32(v); });
    }
    o.u32(static_cast<std::uint32_t>(t.events.size()));
    for (const auto& [name, events] : t.events) {
        o.str(name);
        o.u32(static_cast<std::uint32_t>(events.size()));
        for (const auto& e : events) {
            o.f32(e.timeMs);
            o.i32(e.id);
        }
    }
    o.u32(static_cast<std::uint32_t>(t.turns.size()));
    for (const auto& [key, turn] : t.turns) {
        o.str(key.first);
        o.i32(key.second);
        o.f32(turn);
    }
    o.u32(static_cast<std::uint32_t>(t.outfitProperties.size()));
    for (const auto& [key, value] : t.outfitProperties) {
        o.i32(std::get<0>(key));
        o.i32(std::get<1>(key));
        o.i32(std::get<2>(key));
        o.optional(value, [&](int v) { o.i32(v); });
    }
    return std::move(o.bytes);
}

SupportTable readSupportTable(std::span<const std::uint8_t> bytes) {
    In in(bytes);
    if (in.u32() != kMagic) {
        throw std::runtime_error("not a support table");
    }
    SupportTable t;
    for (std::size_t i = in.count(8); i > 0; --i) {
        std::string name = in.str();
        t.routeModels.emplace(std::move(name), readModel(in));
    }
    for (std::size_t i = in.count(5); i > 0; --i) {
        const std::uint32_t guid = in.u32();
        t.posts[guid] = in.flag() ? std::optional<RouteRect>(readRect(in)) : std::nullopt;
    }
    for (std::size_t i = in.count(5); i > 0; --i) {
        const std::uint32_t guid = in.u32();
        t.fenceBlocks[guid] = in.flag();
    }
    for (std::size_t i = in.count(5); i > 0; --i) {
        std::string name = in.str();
        t.animationLengths[std::move(name)] = in.flag() ? std::optional<float>(in.f32()) : std::nullopt;
    }
    for (std::size_t i = in.count(5); i > 0; --i) {
        std::string name = in.str();
        t.animationCarries[std::move(name)] = in.flag();
    }
    for (std::size_t i = in.count(5); i > 0; --i) {
        std::string name = in.str();
        std::optional<LocoClip> clip;
        if (in.flag()) {
            LocoClip c;
            c.name = name;
            c.lengthMs = in.f32();
            c.travel = in.f32();
            c.loopPhase = in.f32();
            c.match = in.f32();
            c.speed = in.f32();
            c.rootTurn = in.f32();
            clip = std::move(c);
        }
        t.locoClips[std::move(name)] = std::move(clip);
    }
    for (std::size_t i = in.count(9); i > 0; --i) {
        std::string name = in.str();
        const int id = in.i32();
        t.eventTimes[{std::move(name), id}] = in.flag() ? std::optional<float>(in.f32()) : std::nullopt;
    }
    for (std::size_t i = in.count(8); i > 0; --i) {
        std::string name = in.str();
        std::vector<AnimationEvent> events(in.count(8));
        for (auto& e : events) {
            e.timeMs = in.f32();
            e.id = in.i32();
        }
        t.events[std::move(name)] = std::move(events);
    }
    for (std::size_t i = in.count(12); i > 0; --i) {
        std::string name = in.str();
        const int ms = in.i32();
        t.turns[{std::move(name), ms}] = in.f32();
    }
    for (std::size_t i = in.count(13); i > 0; --i) {
        const int a = in.i32(), b = in.i32(), c = in.i32();
        t.outfitProperties[{a, b, c}] = in.flag() ? std::optional<int>(in.i32()) : std::nullopt;
    }
    return t;
}

RouteSources routeSourcesFrom(std::shared_ptr<const SupportTable> table) {
    RouteSources sources;
    sources.fenceBlocks = [table](std::uint32_t guid) {
        const auto it = table->fenceBlocks.find(guid);
        return it != table->fenceBlocks.end() && it->second;
    };
    sources.postFootprint = [table](std::uint32_t guid) -> std::optional<RouteRect> {
        const auto it = table->posts.find(guid);
        return it != table->posts.end() ? it->second : std::nullopt;
    };
    sources.models = [table](const std::string& name) -> const RouteModel* {
        const auto it = table->routeModels.find(name);
        return it != table->routeModels.end() ? &it->second : nullptr;
    };
    return sources;
}

void installSupport(LotSimulation& simulation, std::shared_ptr<const SupportTable> table) {
    simulation.setRouteSources(routeSourcesFrom(table));
    World& world = simulation.world();
    world.setAnimationLengths([table](const std::string& name) -> std::optional<float> {
        const auto it = table->animationLengths.find(name);
        return it != table->animationLengths.end() ? it->second : std::nullopt;
    });
    world.setAnimationCarries([table](const std::string& name) {
        const auto it = table->animationCarries.find(name);
        return it != table->animationCarries.end() && it->second;
    });
    world.setLocoClips([table](const std::string& name) -> std::optional<LocoClip> {
        const auto it = table->locoClips.find(name);
        return it != table->locoClips.end() ? it->second : std::nullopt;
    });
    world.setAnimationEventTime([table](const std::string& name, int id) -> std::optional<float> {
        const auto it = table->eventTimes.find({name, id});
        return it != table->eventTimes.end() ? it->second : std::nullopt;
    });
    world.setAnimationEvents([table](const std::string& name) {
        const auto it = table->events.find(name);
        return it != table->events.end() ? it->second : std::vector<AnimationEvent>{};
    });
    // The root turn at a time: the nearest time that was asked while recording (the table has what a run asked; others are given the closest).
    world.setAnimationTurns([table](const std::string& name, float ms) -> std::optional<float> {
        auto it = table->turns.lower_bound({name, static_cast<int>(std::lround(ms))});
        const auto before = it == table->turns.begin() ? table->turns.end() : std::prev(it);
        const auto usable = [&](decltype(it) i) { return i != table->turns.end() && i->first.first == name; };
        if (usable(it) && usable(before)) {
            return std::abs(it->first.second - ms) < std::abs(before->first.second - ms) ? it->second : before->second;
        }
        return usable(it) ? std::optional<float>(it->second) : usable(before) ? std::optional<float>(before->second) : std::nullopt;
    });
    world.setOutfitProperty([table](std::int16_t neighbor, std::uint16_t category, std::uint16_t property) -> std::optional<std::int16_t> {
        const auto it = table->outfitProperties.find({neighbor, category, property});
        if (it == table->outfitProperties.end() || !it->second) {
            return std::nullopt;
        }
        return static_cast<std::int16_t>(*it->second);
    });
}

} // namespace ts2::sim

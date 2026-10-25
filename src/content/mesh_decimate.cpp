#include "content/mesh_decimate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace ts2::content {

namespace sg = scenegraph;

namespace {

constexpr std::size_t kMinTriangles = 24;  // groups with fewer are left as they are

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(Vec3 a) { return std::sqrt(dot(a, a)); }

// A quadric: the sum of squared distances to a set of planes, as the symmetric matrix of (a, b, c, d).
struct Quadric {
    std::array<double, 10> q{};
    double area = 0;  // the weight of the face planes in it (not the penalties), to turn an error into a distance
    void add(double a, double b, double c, double d, double weight) {
        q[0] += weight * a * a; q[1] += weight * a * b; q[2] += weight * a * c; q[3] += weight * a * d;
        q[4] += weight * b * b; q[5] += weight * b * c; q[6] += weight * b * d;
        q[7] += weight * c * c; q[8] += weight * c * d;
        q[9] += weight * d * d;
    }
    Quadric& operator+=(const Quadric& o) {
        for (std::size_t i = 0; i < q.size(); ++i) {
            q[i] += o.q[i];
        }
        area += o.area;
        return *this;
    }
    double at(Vec3 p) const {
        return q[0] * p.x * p.x + 2 * q[1] * p.x * p.y + 2 * q[2] * p.x * p.z + 2 * q[3] * p.x + q[4] * p.y * p.y +
               2 * q[5] * p.y * p.z + 2 * q[6] * p.y + q[7] * p.z * p.z + 2 * q[8] * p.z + q[9];
    }
};

struct Triangle {
    std::array<std::uint32_t, 3> v;
    bool alive = true;
};

struct Edge {
    double cost;
    std::uint32_t from, to;  // from collapses onto to
    std::uint32_t versionFrom, versionTo;
    bool operator<(const Edge& o) const { return cost > o.cost; }  // the cheapest first
};

class Simplifier {
public:
    Simplifier(std::vector<Vec3> positions, const std::vector<std::uint32_t>& indices, double errorFraction, double partKeep)
        : pos_(std::move(positions)) {
        Vec3 lo = pos_.empty() ? Vec3{} : pos_[0], hi = lo;
        for (const Vec3& p : pos_) {
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const double limit = errorFraction * length(hi - lo);
        maxError2_ = limit * limit;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const std::array<std::uint32_t, 3> t{indices[i], indices[i + 1], indices[i + 2]};
            if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2] && t[0] < pos_.size() && t[1] < pos_.size() && t[2] < pos_.size()) {
                tris_.push_back({t, true});
            }
        }
        alive_ = tris_.size();
        const std::size_t n = pos_.size();
        around_.resize(n);
        quadric_.resize(n);
        version_.assign(n, 0);
        for (std::size_t t = 0; t < tris_.size(); ++t) {
            for (const auto v : tris_[t].v) {
                around_[v].push_back(static_cast<std::uint32_t>(t));
            }
            const Vec3 normal = faceNormal(tris_[t]);
            const double area = length(normal);
            if (area <= 1e-20) {
                continue;
            }
            const Vec3 nrm{normal.x / area, normal.y / area, normal.z / area};
            const double d = -dot(nrm, pos_[tris_[t].v[0]]);
            for (const auto v : tris_[t].v) {
                quadric_[v].add(nrm.x, nrm.y, nrm.z, d, area * 0.5);
                quadric_[v].area += area * 0.5;
            }
        }
        groupByPosition();
        penaliseOpenEdges();
        findParts(partKeep);
    }

    std::size_t triangles() const { return alive_; }

    void reduceTo(std::size_t target) {
        std::priority_queue<Edge> heap;
        for (std::size_t t = 0; t < tris_.size(); ++t) {
            pushEdges(heap, tris_[t]);
        }
        while (alive_ > target && !heap.empty()) {
            const Edge e = heap.top();
            heap.pop();
            if (!hasLive(e.from) || !hasLive(e.to) || version_[e.from] != e.versionFrom || version_[e.to] != e.versionTo) {
                continue;
            }
            std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
            if (!plan(e.from, e.to, pairs)) {
                continue;
            }
            for (const auto& [u, w] : pairs) {
                collapse(u, w);
            }
            // Edges around the places that took the others in have new costs.
            for (const auto& pair : pairs) {
                for (const auto t : around_[pair.second]) {
                    if (tris_[t].alive) {
                        pushEdges(heap, tris_[t]);
                    }
                }
            }
        }
    }

    // The triangles left, and for each vertex that is used, its new index (others are -1).
    std::vector<std::uint32_t> result(std::vector<std::int32_t>& remap) const {
        remap.assign(pos_.size(), -1);
        std::vector<std::uint32_t> out;
        std::int32_t next = 0;
        for (const Triangle& t : tris_) {
            if (!t.alive) {
                continue;
            }
            for (const auto v : t.v) {
                if (remap[v] < 0) {
                    remap[v] = next++;
                }
                out.push_back(static_cast<std::uint32_t>(remap[v]));
            }
        }
        return out;
    }

private:
    bool hasLive(std::uint32_t v) const {
        for (const auto t : around_[v]) {
            if (tris_[t].alive) {
                return true;
            }
        }
        return false;
    }

    Vec3 faceNormal(const Triangle& t) const {
        return cross(pos_[t.v[1]] - pos_[t.v[0]], pos_[t.v[2]] - pos_[t.v[0]]);
    }

    struct KeyHash {
        std::size_t operator()(const std::array<std::uint32_t, 3>& k) const noexcept {
            std::uint64_t h = 0xCBF29CE484222325ull;
            for (const auto p : k) {
                h = (h ^ p) * 0x100000001B3ull;
            }
            return static_cast<std::size_t>(h);
        }
    };

    // Vertices at one place (a texture or normal seam splits a vertex in several) form a class, and move together.
    void groupByPosition() {
        class_.assign(pos_.size(), 0);
        std::unordered_map<std::array<std::uint32_t, 3>, std::uint32_t, KeyHash> ids;
        for (std::uint32_t v = 0; v < pos_.size(); ++v) {
            const float p[3] = {static_cast<float>(pos_[v].x), static_cast<float>(pos_[v].y), static_cast<float>(pos_[v].z)};
            std::array<std::uint32_t, 3> key;
            std::memcpy(key.data(), p, sizeof key);
            const auto [it, inserted] = ids.emplace(key, static_cast<std::uint32_t>(members_.size()));
            if (inserted) {
                members_.emplace_back();
            }
            class_[v] = it->second;
            members_[it->second].push_back(v);
        }
    }

    // Parts: triangles joined through vertices at one place. Each keeps at least a share of its triangles (and 4), so that a
    // thin post or plank is made coarser but never collapsed away.
    void findParts(double partKeep) {
        std::vector<std::uint32_t> parent(members_.size());
        for (std::size_t i = 0; i < parent.size(); ++i) {
            parent[i] = static_cast<std::uint32_t>(i);
        }
        const auto find = [&](std::uint32_t x) {
            while (parent[x] != x) {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        };
        for (const Triangle& t : tris_) {
            parent[find(class_[t.v[0]])] = find(class_[t.v[1]]);
            parent[find(class_[t.v[1]])] = find(class_[t.v[2]]);
        }
        part_.assign(members_.size(), 0);
        std::unordered_map<std::uint32_t, std::uint32_t> ids;
        for (std::uint32_t c = 0; c < members_.size(); ++c) {
            const auto [it, inserted] = ids.emplace(find(c), static_cast<std::uint32_t>(ids.size()));
            part_[c] = it->second;
        }
        partAlive_.assign(ids.size(), 0);
        for (const Triangle& t : tris_) {
            ++partAlive_[part_[class_[t.v[0]]]];
        }
        partFloor_.resize(partAlive_.size());
        for (std::size_t i = 0; i < partAlive_.size(); ++i) {
            partFloor_[i] = std::max<std::size_t>(4, static_cast<std::size_t>(std::ceil(double(partAlive_[i]) * partKeep)));
        }
    }

    // An edge only one triangle has is the surface's edge (or a texture island's): a plane through it, square to the
    // triangle, makes moving it sideways expensive and sliding it along cheap.
    void penaliseOpenEdges() {
        std::unordered_map<std::uint64_t, int> uses;
        const auto key = [](std::uint32_t a, std::uint32_t b) { return (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b); };
        for (const Triangle& t : tris_) {
            for (int i = 0; i < 3; ++i) {
                ++uses[key(t.v[i], t.v[(i + 1) % 3])];
            }
        }
        for (const Triangle& t : tris_) {
            const Vec3 normal = faceNormal(t);
            const double area = length(normal);
            if (area <= 1e-20) {
                continue;
            }
            const Vec3 n{normal.x / area, normal.y / area, normal.z / area};
            for (int i = 0; i < 3; ++i) {
                const std::uint32_t a = t.v[i], b = t.v[(i + 1) % 3];
                if (uses[key(a, b)] != 1) {
                    continue;
                }
                const Vec3 e = pos_[b] - pos_[a];
                const double len = length(e);
                if (len <= 1e-12) {
                    continue;
                }
                const Vec3 m = cross({e.x / len, e.y / len, e.z / len}, n);
                const double d = -dot(m, pos_[a]);
                const double weight = 100.0 * len * len;
                quadric_[a].add(m.x, m.y, m.z, d, weight);
                quadric_[b].add(m.x, m.y, m.z, d, weight);
            }
        }
    }

    // The vertex of class `cls` that shares a triangle with `u`, or none.
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    std::uint32_t partner(std::uint32_t u, std::uint32_t cls) const {
        for (const auto t : around_[u]) {
            if (!tris_[t].alive) {
                continue;
            }
            for (const auto v : tris_[t].v) {
                if (class_[v] == cls && v != u) {
                    return v;
                }
            }
        }
        return kNone;
    }

    // The error of moving `from` onto `to` alone, or a negative number when that would turn a triangle over or squash one.
    double pairCost(std::uint32_t from, std::uint32_t to) const {
        for (const auto t : around_[from]) {
            const Triangle& tri = tris_[t];
            if (!tri.alive || std::find(tri.v.begin(), tri.v.end(), to) != tri.v.end()) {
                continue;
            }
            Triangle moved = tri;
            for (auto& v : moved.v) {
                if (v == from) {
                    v = to;
                }
            }
            const Vec3 before = faceNormal(tri), after = faceNormal(moved);
            const double lb = length(before), la = length(after);
            if (lb > 1e-20 && (la <= 1e-20 || dot(before, after) / (lb * la) < 0.2)) {
                return -1;
            }
        }
        Quadric sum = quadric_[from];
        sum += quadric_[to];
        return sum.at(pos_[to]);
    }

    // Moving `from` onto `to` moves every vertex at from's place onto the one at to's place it shares a triangle with, so that
    // seams stay closed. Returns the pairs, and false when that can't be done.
    bool plan(std::uint32_t from, std::uint32_t to, std::vector<std::pair<std::uint32_t, std::uint32_t>>& pairs, double* total = nullptr) const {
        const std::uint32_t cf = class_[from], ct = class_[to];
        if (cf == ct) {
            return false;
        }
        double sum = 0, weight = 0;
        for (const std::uint32_t u : members_[cf]) {
            if (!hasLive(u)) {
                continue;
            }
            const std::uint32_t w = u == from ? to : partner(u, ct);
            if (w == kNone) {
                return false;
            }
            const double c = pairCost(u, w);
            if (c < 0) {
                return false;
            }
            std::size_t gone = 0;  // triangles this pair removes
            for (const auto t : around_[u]) {
                gone += tris_[t].alive && std::find(tris_[t].v.begin(), tris_[t].v.end(), w) != tris_[t].v.end();
            }
            if (partAlive_[part_[class_[u]]] < partFloor_[part_[class_[u]]] + gone) {
                return false;
            }
            sum += c;
            weight += quadric_[u].area + quadric_[w].area;
            pairs.emplace_back(u, w);
        }
        const double mean = weight > 1e-18 ? sum / weight : sum;
        if (pairs.empty() || mean > maxError2_) {
            return false;
        }
        if (total) {
            *total = mean;
        }
        return true;
    }

    void pushEdges(std::priority_queue<Edge>& heap, const Triangle& t) {
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t a = t.v[i], b = t.v[(i + 1) % 3];
            for (const auto& [from, to] : {std::pair{a, b}, std::pair{b, a}}) {
                std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
                double c = 0;
                if (plan(from, to, pairs, &c)) {
                    heap.push({c, from, to, version_[from], version_[to]});
                }
            }
        }
    }

    void collapse(std::uint32_t from, std::uint32_t to) {
        for (const auto t : around_[from]) {
            Triangle& tri = tris_[t];
            if (!tri.alive) {
                continue;
            }
            if (std::find(tri.v.begin(), tri.v.end(), to) != tri.v.end()) {
                tri.alive = false;
                --alive_;
                --partAlive_[part_[class_[to]]];
                continue;
            }
            for (auto& v : tri.v) {
                if (v == from) {
                    v = to;
                }
            }
            around_[to].push_back(t);
        }
        around_[from].clear();
        quadric_[to] += quadric_[from];
        ++version_[to];
        ++version_[from];
    }

    std::vector<Vec3> pos_;
    std::vector<Triangle> tris_;
    std::vector<std::vector<std::uint32_t>> around_;
    std::vector<Quadric> quadric_;
    std::vector<std::uint32_t> version_;
    std::vector<std::uint32_t> class_;
    std::vector<std::vector<std::uint32_t>> members_;
    std::size_t alive_ = 0;
    std::vector<std::uint32_t> part_;      // per position class
    std::vector<std::size_t> partAlive_, partFloor_;
    double maxError2_ = 0;  // the most a collapse may move things, squared, as a mean over the surface it touches
};

std::size_t bytesPerItem(std::uint32_t blockFormat) {
    switch (blockFormat) {
    case 0: return 4;
    case 1: return 8;
    case 2: return 12;
    case 4: return 4;
    default: return 0;
    }
}

// One element of `link` as one item per vertex, in bytes: through the link's aliases for positions, normals and
// texture coordinates. Empty when it can't be had.
// Whether the engine reads elements of this kind (see scene/ and render/); others are carried only if easy.
bool readByEngine(std::uint32_t identity) {
    return identity == sg::kPositions || identity == sg::kNormals || identity == sg::kTextureCoordinates || identity == sg::kColors ||
           identity == sg::kBlendIndices || identity == sg::kBlendWeights || identity == sg::kTangents;
}

std::optional<std::vector<std::uint8_t>> perVertex(const sg::Mesh& mesh, const sg::ElementLink& link, std::size_t which, std::size_t nth) {
    const sg::Element& element = mesh.elements[link.elements[which]];
    const std::size_t stride = bytesPerItem(element.blockFormat);
    if (stride == 0) {
        return std::nullopt;
    }
    if (element.data.empty()) {
        return std::vector<std::uint8_t>{};  // an element that holds no data (only its count) is carried as it is
    }
    const bool aliased = element.identity == sg::kPositions || element.identity == sg::kNormals || element.identity == sg::kTextureCoordinates;
    if (aliased && element.blockFormat <= 2) {
        const std::vector<float> items = sg::vertexFloats(mesh, link, element.identity, nth);
        std::vector<std::uint8_t> out(items.size() * 4);
        std::memcpy(out.data(), items.data(), out.size());
        if (out.size() != link.vertexCount * stride) {
            return std::nullopt;
        }
        return out;
    }
    if (element.data.size() != link.vertexCount * stride) {
        return std::nullopt;
    }
    return element.data;
}

} // namespace

IndexedReduction reduceIndexed(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices, double keep, double maxError) {
    IndexedReduction r;
    std::vector<Vec3> p(positions.size() / 3);
    for (std::size_t v = 0; v < p.size(); ++v) {
        p[v] = {positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
    }
    Simplifier simplifier(std::move(p), indices, maxError, keep * 0.75);
    const std::size_t before = simplifier.triangles();
    if (before < kMinTriangles) {
        return r;
    }
    simplifier.reduceTo(std::max<std::size_t>(kMinTriangles / 2, static_cast<std::size_t>(std::ceil(double(before) * keep))));
    r.indices = simplifier.result(r.remap);
    r.reduced = r.indices.size() / 3 < before;
    return r;
}

sg::Mesh decimate(const sg::Mesh& mesh, double keep, double maxError, DecimateStats& stats) {
    sg::Mesh out = mesh;
    std::vector<sg::ElementLink> links = mesh.links;
    std::vector<sg::Element> elements = mesh.elements;
    for (sg::Group& group : out.groups) {
        if (group.primitive != sg::kTriangleList) {
            continue;
        }
        ++stats.groups;
        const std::size_t before = group.indices.size() / 3;
        stats.trianglesBefore += before;
        stats.trianglesAfter += before;  // corrected below when the group is reduced
        const auto skip = [&](std::size_t& reason, std::size_t& triangles) { ++stats.groupsSkipped; ++reason; triangles += before; };
        if (before < kMinTriangles) {
            skip(stats.skippedSmall, stats.trianglesSmall);
            continue;
        }
        if (group.link >= mesh.links.size()) {
            skip(stats.skippedData, stats.trianglesData);
            continue;
        }
        const sg::ElementLink& link = mesh.links[group.link];
        bool carryable = true;
        std::vector<std::vector<std::uint8_t>> items;
        std::vector<bool> kept;  // per element of the link: whether it goes on to the reduced group
        std::map<std::uint32_t, std::size_t> seen;
        for (std::size_t i = 0; i < link.elements.size() && carryable; ++i) {
            if (link.elements[i] >= mesh.elements.size()) {
                carryable = false;
                break;
            }
            auto item = perVertex(mesh, link, i, seen[mesh.elements[link.elements[i]].identity]++);
            // Elements nothing reads (morph data, which is per few vertices) are dropped when they can't follow the vertices.
            kept.push_back(item.has_value());
            carryable = item.has_value() || !readByEngine(mesh.elements[link.elements[i]].identity);
            items.push_back(item ? std::move(*item) : std::vector<std::uint8_t>{});

        }
        const std::vector<float> positions = sg::vertexFloats(mesh, link, sg::kPositions);
        if (!carryable || positions.size() != std::size_t(link.vertexCount) * 3 || link.vertexCount > 65535) {
            skip(stats.skippedData, stats.trianglesData);
            continue;
        }
        std::vector<Vec3> p(link.vertexCount);
        for (std::size_t v = 0; v < p.size(); ++v) {
            p[v] = {positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
        }
        Simplifier simplifier(std::move(p), std::vector<std::uint32_t>(group.indices.begin(), group.indices.end()), maxError, keep * 0.75);
        const std::size_t target = std::max<std::size_t>(kMinTriangles / 2, static_cast<std::size_t>(std::ceil(double(simplifier.triangles()) * keep)));
        simplifier.reduceTo(target);
        std::vector<std::int32_t> remap;
        const std::vector<std::uint32_t> reducedIndices = simplifier.result(remap);
        std::vector<std::uint16_t> indices(reducedIndices.begin(), reducedIndices.end());
        if (indices.size() / 3 >= before) {
            skip(stats.skippedStuck, stats.trianglesStuck);
            continue;
        }
        // The group gets its own link and elements: the vertices that are left, in the new order.
        const std::size_t vertices = static_cast<std::size_t>(*std::max_element(remap.begin(), remap.end())) + 1;
        sg::ElementLink fresh;
        fresh.vertexCount = static_cast<std::uint32_t>(vertices);
        for (std::size_t i = 0; i < link.elements.size(); ++i) {
            if (!kept[i]) {
                continue;
            }
            const sg::Element& old = mesh.elements[link.elements[i]];
            const std::size_t stride = bytesPerItem(old.blockFormat);
            sg::Element e = old;
            e.count = static_cast<std::uint32_t>(vertices);
            e.data.assign(old.data.empty() ? 0 : vertices * stride, 0);
            for (std::size_t v = 0; v < remap.size() && !old.data.empty(); ++v) {
                if (remap[v] >= 0) {
                    std::memcpy(&e.data[std::size_t(remap[v]) * stride], &items[i][v * stride], stride);
                }
            }
            fresh.elements.push_back(static_cast<std::uint16_t>(elements.size()));
            elements.push_back(std::move(e));
        }
        stats.trianglesAfter -= before - indices.size() / 3;
        ++stats.groupsReduced;
        group.indices = std::move(indices);
        group.link = static_cast<std::uint32_t>(links.size());
        links.push_back(std::move(fresh));
    }
    // Keep only the links and elements some group still draws from.
    std::vector<std::int32_t> linkMap(links.size(), -1), elementMap(elements.size(), -1);
    out.links.clear();
    out.elements.clear();
    for (sg::Group& g : out.groups) {
        if (g.link >= links.size()) {
            continue;
        }
        if (linkMap[g.link] < 0) {
            sg::ElementLink l = links[g.link];
            for (auto& e : l.elements) {
                if (elementMap[e] < 0) {
                    elementMap[e] = static_cast<std::int32_t>(out.elements.size());
                    out.elements.push_back(elements[e]);
                }
                e = static_cast<std::uint16_t>(elementMap[e]);
            }
            linkMap[g.link] = static_cast<std::int32_t>(out.links.size());
            out.links.push_back(std::move(l));
        }
        g.link = static_cast<std::uint32_t>(linkMap[g.link]);
    }
    return out;
}

} // namespace ts2::content

// The converter's first step: builds the scenes of some lots against an install, notes every resource they read, and
// writes those to a bank file (src/content/bank.hpp) with a report of what the bank holds. Textures are stored as
// the game has them for now; converting them is the next step.
#include "content/bank.hpp"
#include "content/console_texture.hpp"
#include "content/mesh_decimate.hpp"
#include "formats/scenegraph.hpp"
#include "render/texture_convert.hpp"
#include "render/texture_fit.hpp"
#include "content/lot.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/lot_scene.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct Key {
    std::uint32_t type, group, instance, instanceHigh;
    bool operator<(const Key& o) const {
        return std::tie(type, group, instance, instanceHigh) < std::tie(o.type, o.group, o.instance, o.instanceHigh);
    }
};

constexpr std::uint32_t kTxtr = 0x1C4A276C;
constexpr std::uint32_t kLifo = 0xED534136;

struct TextureOptions {
    bool convert = false;
    unsigned maxSide = 256;
    double paletteError = 8;  // RMS levels a 4-bit palette may change a texture by; half that for 8-bit
    bool dither = false;
    std::string sheetDir;  // when set: pairs of raw RGBA files (original, converted) of the textures worth looking at, and a manifest
};

constexpr std::uint32_t kGmdc = 0xAC4F8687;

struct MeshOptions {
    double maxError = 0.01;  // of a group's size: how far reducing may move its surface
    double keep = 0;  // 0: meshes are left alone; 1: only written back, to check the writer
    std::string dumpDir;  // OBJ files of the meshes that were reduced, before and after, for looking at
};

bool sameMesh(const ts2::scenegraph::Mesh& a, const ts2::scenegraph::Mesh& b) {
    const auto elementsEqual = [](const ts2::scenegraph::Element& x, const ts2::scenegraph::Element& y) {
        return x.identity == y.identity && x.blockFormat == y.blockFormat && x.setFormat == y.setFormat && x.count == y.count && x.data == y.data;
    };
    if (a.name != b.name || a.elements.size() != b.elements.size() || a.links.size() != b.links.size() || a.groups.size() != b.groups.size() ||
        a.inverseBinds.size() != b.inverseBinds.size() || a.container.links.size() != b.container.links.size() ||
        a.container.blockTypes != b.container.blockTypes) {
        return false;
    }
    for (std::size_t i = 0; i < a.elements.size(); ++i) {
        if (!elementsEqual(a.elements[i], b.elements[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.links.size(); ++i) {
        if (a.links[i].elements != b.links[i].elements || a.links[i].vertexCount != b.links[i].vertexCount || a.links[i].aliases != b.links[i].aliases) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.groups.size(); ++i) {
        const auto &x = a.groups[i], &y = b.groups[i];
        if (x.primitive != y.primitive || x.link != y.link || x.name != y.name || x.indices != y.indices || x.opacity != y.opacity || x.bones != y.bones) {
            return false;
        }
    }
    return true;
}

void writeObj(const std::string& path, const ts2::scenegraph::Mesh& mesh) {
    std::ofstream out(path);
    std::size_t base = 1;
    for (const auto& group : mesh.groups) {
        if (group.primitive != ts2::scenegraph::kTriangleList || group.link >= mesh.links.size()) {
            continue;
        }
        const auto positions = ts2::scenegraph::vertexFloats(mesh, mesh.links[group.link], ts2::scenegraph::kPositions);
        for (std::size_t v = 0; v + 2 < positions.size(); v += 3) {
            out << "v " << positions[v] << ' ' << positions[v + 1] << ' ' << positions[v + 2] << '\n';
        }
        for (std::size_t i = 0; i + 2 < group.indices.size(); i += 3) {
            out << "f " << base + group.indices[i] << ' ' << base + group.indices[i + 1] << ' ' << base + group.indices[i + 2] << '\n';
        }
        base += positions.size() / 3;
    }
}

struct MeshStats {
    std::size_t count = 0, failed = 0, mismatched = 0;
    std::size_t bytesBefore = 0, bytesAfter = 0;
    ts2::content::DecimateStats decimate;
};

void convertMeshes(std::map<Key, ts2::content::BankResource>& gathered, const MeshOptions& options, MeshStats& stats) {
    std::size_t dumped = 0;
    for (auto& [key, resource] : gathered) {
        if (key.type != kGmdc) {
            continue;
        }
        ++stats.count;
        try {
            const auto mesh = ts2::scenegraph::parseMesh(resource.bytes);
            stats.bytesBefore += resource.bytes.size();
            ts2::content::DecimateStats before = stats.decimate;
            const auto reduced = options.keep < 1 ? ts2::content::decimate(mesh, options.keep, options.maxError, stats.decimate) : mesh;
            const auto bytes = ts2::scenegraph::writeMesh(reduced);
            if (!sameMesh(ts2::scenegraph::parseMesh(bytes), reduced)) {
                ++stats.mismatched;
            }
            if (!options.dumpDir.empty() && stats.decimate.groupsReduced > before.groupsReduced && stats.count % 9 == 0) {
                const std::string base = options.dumpDir + "/" + std::to_string(stats.count);
                writeObj(base + "_orig.obj", mesh);
                writeObj(base + "_dec.obj", reduced);
                std::ofstream(options.dumpDir + "/manifest.tsv", std::ios::app)
                    << stats.count << '\t' << mesh.name << '\t' << (stats.decimate.trianglesBefore - before.trianglesBefore) << '\t'
                    << (stats.decimate.trianglesAfter - before.trianglesAfter) << '\n';
                ++dumped;
            }
            resource.bytes = bytes;
            stats.bytesAfter += bytes.size();
        } catch (const std::exception& e) {
            ++stats.failed;
            std::fprintf(stderr, "mesh failed: %s\n", e.what());
        }
    }
}

struct TextureStats {
    std::size_t count = 0, dropped = 0, failed = 0;
    std::size_t byBits[3] = {0, 0, 0};  // 4, 8, 32
    std::size_t bytesBefore = 0, bytesAfter = 0;
    std::size_t lifoDropped = 0, lifoBytes = 0;
    double errorSum = 0, errorMax = 0;
    std::size_t shrunk = 0;
};

// Replaces each texture of the bank by its console form, and drops the large-level resources it read from.
void convertTextures(ts2::scene::Assets& assets, std::map<Key, ts2::content::BankResource>& gathered, const TextureOptions& options,
                     TextureStats& stats) {
    using ts2::content::ConsoleTexture;
    namespace render = ts2::render;
    std::vector<Key> remove;
    for (auto& [key, resource] : gathered) {
        if (key.type != kTxtr) {
            continue;
        }
        ++stats.count;
        try {
            const auto info = ts2::scenegraph::parseImage(resource.bytes);
            std::string name = info.name;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, "_txtr") == 0) {
                name.resize(name.size() - 5);
            }
            stats.bytesBefore += resource.bytes.size();
            if (info.format == 6 || info.faces.size() > 1) {
                ++stats.dropped;  // bump maps and cube maps are not shown on the console
                remove.push_back(key);
                continue;
            }
            auto image = assets.texture(name);
            if (!image) {
                ++stats.failed;
                continue;
            }
            ts2::render::ConvertOptions convert;
            convert.maxSide = options.maxSide;
            convert.paletteError = options.paletteError;
            convert.dither = options.dither;
            const ts2::render::ConvertedTexture converted = ts2::render::convertTexture(*image, convert);
            const ConsoleTexture& out = converted.texture;
            const double error = converted.error;
            stats.shrunk += converted.resized ? 1 : 0;
            ++stats.byBits[out.bits == 4 ? 0 : out.bits == 8 ? 1 : 2];
            // What the console texture looks like, and the picture it was made from at its size, for the sheets.
            const ts2::scene::Image shown = ts2::render::decodeConsoleTexture(out);
            const ts2::scene::Image fitted = converted.resized ? render::stretched(*image, out.width, out.height) : *image;
            if (!options.sheetDir.empty() && (out.bits == 32 || error >= 5 || stats.count % 25 == 0)) {
                const std::string base = options.sheetDir + "/" + std::to_string(stats.count);
                std::ofstream(base + "_orig.rgba", std::ios::binary).write(reinterpret_cast<const char*>(fitted.rgba.data()), std::streamsize(fitted.rgba.size()));
                std::ofstream(base + "_conv.rgba", std::ios::binary).write(reinterpret_cast<const char*>(shown.rgba.data()), std::streamsize(shown.rgba.size()));
                std::ofstream(options.sheetDir + "/manifest.tsv", std::ios::app)
                    << stats.count << '\t' << name << '\t' << fitted.width << '\t' << fitted.height << '\t' << out.bits << '\t' << error
                    << '\t' << info.width << 'x' << info.height << '\n';
            }
            stats.errorSum += error;
            stats.errorMax = std::max(stats.errorMax, error);
            resource.bytes = out.serialize();
            stats.bytesAfter += resource.bytes.size();
        } catch (const std::exception& e) {
            ++stats.failed;
            std::fprintf(stderr, "texture failed: %s\n", e.what());
        }
    }
    for (const Key& k : remove) {
        gathered.erase(k);
    }
    for (auto it = gathered.begin(); it != gathered.end();) {
        if (it->first.type == kLifo) {
            ++stats.lifoDropped;
            stats.lifoBytes += it->second.bytes.size();
            it = gathered.erase(it);
        } else {
            ++it;
        }
    }
}

std::uint32_t sourceOf(const std::string& name) {
    using namespace ts2::content::bank;
    return name == "models" ? kModels : name == "objects" ? kObjects : name == "catalog" ? kCatalog : kSkins;
}

} // namespace

int main(int argc, char** argv) {
    TextureOptions textures;
    MeshOptions meshes;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--textures") {
            textures.convert = true;
        } else if (a == "--max-side" && i + 1 < argc) {
            textures.maxSide = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (a == "--palette-error" && i + 1 < argc) {
            textures.paletteError = std::stod(argv[++i]);
        } else if (a == "--sheet-dir" && i + 1 < argc) {
            textures.sheetDir = argv[++i];
        } else if (a == "--mesh-keep" && i + 1 < argc) {
            meshes.keep = std::stod(argv[++i]);
        } else if (a == "--mesh-error" && i + 1 < argc) {
            meshes.maxError = std::stod(argv[++i]);
        } else if (a == "--mesh-dump" && i + 1 < argc) {
            meshes.dumpDir = argv[++i];
        } else if (a == "--dither") {
            textures.dither = true;
        } else {
            args.push_back(a);
        }
    }
    if (args.size() < 3) {
        std::fprintf(stderr, "Usage: ts2bank <install-folder> <out.bank> <lot.package>... [--textures [--max-side N] [--palette-error E] [--dither]] [--mesh-keep K [--mesh-error F] [--mesh-dump DIR]]\n");
        return 1;
    }
    try {
        const std::filesystem::path install = args[0], out = args[1];
        ts2::scene::Assets assets(install);
        std::map<Key, ts2::content::BankResource> gathered;
        std::size_t differing = 0;
        assets.observeReads([&](const char* source, const ts2::dbpf::ResourceKey& key, const std::vector<std::uint8_t>& bytes) {
            auto [it, inserted] = gathered.try_emplace(Key{key.type, key.group, key.instance, key.instanceHigh});
            if (inserted) {
                it->second = {key, sourceOf(source), bytes};
            } else if (it->second.bytes != bytes) {
                ++differing;  // the same key in two sets of packages, with other contents: the bank keeps the first
            }
        });
        for (std::size_t i = 2; i < args.size(); ++i) {
            ts2::scene::SceneBuilder builder(assets);
            const auto lot = ts2::content::loadLot(args[i]);
            ts2::scene::addLot(builder, lot, args[i]);
            std::printf("%-28s %zu resources so far\n", std::filesystem::path(args[i]).stem().string().c_str(), gathered.size());
        }
        assets.observeReads(nullptr);
        if (textures.convert) {
            TextureStats stats;
            convertTextures(assets, gathered, textures, stats);
            const double n = static_cast<double>(stats.byBits[0] + stats.byBits[1] + stats.byBits[2]);
            std::printf("\ntextures: %zu read, %zu dropped (bump/cube maps), %zu failed; %zu resized to <= %u\n", stats.count, stats.dropped,
                        stats.failed, stats.shrunk, textures.maxSide);
            std::printf("  4-bit %zu, 8-bit %zu, 32-bit %zu; RMS error mean %.2f max %.2f levels\n", stats.byBits[0], stats.byBits[1], stats.byBits[2],
                        n > 0 ? stats.errorSum / n : 0.0, stats.errorMax);
            std::printf("  TXTR %.1f MiB -> %.1f MiB; %zu large-level resources (%.1f MiB) folded in\n", static_cast<double>(stats.bytesBefore) / 1048576.0,
                        static_cast<double>(stats.bytesAfter) / 1048576.0, stats.lifoDropped, static_cast<double>(stats.lifoBytes) / 1048576.0);
        }

        if (meshes.keep > 0) {
            MeshStats stats;
            convertMeshes(gathered, meshes, stats);
            std::printf("\nmeshes: %zu, %zu failed, %zu did not read back the same; %.1f MiB -> %.1f MiB\n", stats.count, stats.failed, stats.mismatched,
                        double(stats.bytesBefore) / 1048576.0, double(stats.bytesAfter) / 1048576.0);
            const auto& d = stats.decimate;
            std::printf("  left alone (groups/triangles): skinned %zu/%zu, small %zu/%zu, data not carried %zu/%zu, nothing could collapse %zu/%zu\n",
                        d.skippedSkinned, d.trianglesSkinned, d.skippedSmall, d.trianglesSmall, d.skippedData, d.trianglesData, d.skippedStuck, d.trianglesStuck);
            std::printf("  triangle-list groups %zu: %zu reduced, %zu left alone; triangles %zu -> %zu (%.0f%%)\n", d.groups, d.groupsReduced, d.groupsSkipped,
                        d.trianglesBefore, d.trianglesAfter, d.trianglesBefore ? 100.0 * double(d.trianglesAfter) / double(d.trianglesBefore) : 0.0);
        }
        std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> byType;
        std::size_t total = 0;
        std::vector<ts2::content::BankResource> resources;
        std::vector<ts2::dbpf::ResourceKey> keysOfType;
        for (auto& [key, r] : gathered) {
            if (key.type == kTxtr) {
                keysOfType.push_back(r.key);
            }
            auto& t = byType[key.type];
            ++t.first;
            t.second += r.bytes.size();
            total += r.bytes.size();
            resources.push_back(std::move(r));
        }
        const std::size_t fileSize = ts2::content::writeBank(out, std::move(resources));
        std::printf("\n%-12s %8s %12s\n", "type", "count", "bytes");
        for (const auto& [type, t] : byType) {
            std::printf("0x%08x   %8zu %12zu\n", type, t.first, t.second);
        }
        std::printf("\n%zu resources, %.2f MiB of data, file %.2f MiB; %zu keys read with different contents; %zu asset warnings\n",
                    gathered.size(), static_cast<double>(total) / 1048576.0, static_cast<double>(fileSize) / 1048576.0, differing,
                    assets.warnings.size());

        // Read it back through the reader and compare.
        std::ifstream in(out, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        const ts2::content::Bank bank(bytes);
        std::printf("bank reads back: %zu entries\n", bank.size());
        if (textures.convert) {
            std::size_t parsed = 0;
            for (const auto& entry : keysOfType) {
                const auto blob = bank.find(entry);
                if (ts2::content::ConsoleTexture::matches(blob)) {
                    (void)ts2::content::ConsoleTexture::parse(blob);
                    ++parsed;
                }
            }
            std::printf("%zu console textures parse back\n", parsed);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "failed: %s\n", error.what());
        return 1;
    }
    return 0;
}

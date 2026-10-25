// The converter's first step: builds the scenes of some lots against an install, notes every resource they read, and
// writes those to a bank file (src/content/bank.hpp) with a report of what the bank holds. Textures are stored as
// the game has them for now; converting them is the next step.
#include "content/bank.hpp"
#include "content/console_texture.hpp"
#include "formats/scenegraph.hpp"
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

struct TextureStats {
    std::size_t count = 0, dropped = 0, failed = 0;
    std::size_t byBits[3] = {0, 0, 0};  // 4, 8, 32
    std::size_t bytesBefore = 0, bytesAfter = 0;
    std::size_t lifoDropped = 0, lifoBytes = 0;
    double errorSum = 0, errorMax = 0;
    std::size_t shrunk = 0;
};

// Palette and indices of an image that has no more than 256 colours.
bool toIndexed(const ts2::scene::Image& image, unsigned bits, ts2::content::ConsoleTexture& out) {
    std::unordered_map<std::uint32_t, std::uint8_t> index;
    out.palette.clear();
    const std::size_t pixels = std::size_t(image.width) * image.height;
    std::vector<std::uint8_t> indices(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint32_t c;
        std::memcpy(&c, &image.rgba[i * 4], 4);
        auto it = index.find(c);
        if (it == index.end()) {
            if (index.size() >= (1u << bits)) {
                return false;
            }
            it = index.emplace(c, static_cast<std::uint8_t>(index.size())).first;
            out.palette.insert(out.palette.end(), image.rgba.begin() + std::ptrdiff_t(i * 4), image.rgba.begin() + std::ptrdiff_t(i * 4 + 4));
        }
        indices[i] = it->second;
    }
    out.bits = bits;
    if (bits == 4) {
        out.texels.assign((pixels + 1) / 2, 0);
        for (std::size_t i = 0; i < pixels; ++i) {
            out.texels[i / 2] = static_cast<std::uint8_t>(out.texels[i / 2] | (indices[i] << ((i & 1) * 4)));
        }
    } else {
        out.texels = std::move(indices);
    }
    return true;
}

// Replaces each texture of the bank by its console form, and drops the large-level resources it read from.
void convertTextures(ts2::scene::Assets& assets, std::map<Key, ts2::content::BankResource>& gathered, const TextureOptions& options,
                     TextureStats& stats) {
    using ts2::content::ConsoleTexture;
    namespace render = ts2::render;
    const render::TextureLimits limits{options.maxSide, 8};
    ts2::render::QuantizeOptions quant;
    quant.iterations = 3;
    quant.dither = options.dither;
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
            const unsigned tw = render::fitSide(image->width, limits), th = render::fitSide(image->height, limits);
            ts2::scene::Image fitted = (tw == image->width && th == image->height) ? std::move(*image) : render::stretched(*image, tw, th);
            stats.shrunk += (tw != info.width || th != info.height) ? 1 : 0;
            bool alpha = false;
            for (std::size_t i = 3; i < fitted.rgba.size(); i += 4) {
                alpha = alpha || fitted.rgba[i] != 255;
            }
            ts2::scene::Image shown;  // what the console texture looks like
            ConsoleTexture out;
            out.width = fitted.width;
            out.height = fitted.height;
            out.hasAlpha = alpha;
            double error = 0;
            bool done = false;
            for (unsigned bits : {4u, 8u}) {
                const ts2::scene::Image reduced = render::quantized(fitted, 1u << bits, quant);
                error = render::imageError(fitted, reduced);
                if (error <= (bits == 4 ? options.paletteError : options.paletteError / 2) && toIndexed(reduced, bits, out)) {
                    ++stats.byBits[bits == 4 ? 0 : 1];
                    shown = reduced;
                    done = true;
                    break;
                }
            }
            if (!done) {
                out.bits = 32;
                out.palette.clear();
                out.texels = fitted.rgba;
                shown = fitted;
                error = 0;
                ++stats.byBits[2];
            }
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
        } else if (a == "--dither") {
            textures.dither = true;
        } else {
            args.push_back(a);
        }
    }
    if (args.size() < 3) {
        std::fprintf(stderr, "Usage: ts2bank <install-folder> <out.bank> <lot.package>... [--textures [--max-side N] [--palette-error E] [--dither]]\n");
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

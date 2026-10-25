// Gathers what a lot's simulation reads into a small tree a console can run it from:
//   simbank <install-folder> <lot.package> <out-folder> [ticks]
// out/support.bin holds what the simulation asked the game's models and animations (see sim/support_table.hpp), recorded while it ran.
// out/code.package holds every resource the simulation read while the lot loaded and during `ticks` ticks (decompressed, under the keys the index gave them,
// the neighborhood's characters included), out/sim.cfg the installed-packs mask, and the lot's and its neighborhood's packages are copied to where the
// neighborhood folders put them. The console runs LotSimulation(packages, mask, lot) on that tree.
#include "content/animation_lists.hpp"
#include "content/install.hpp"
#include "content/neighborhood.hpp"
#include "formats/resource_types.hpp"
#include "content/lot.hpp"
#include "content/wants.hpp"
#include "content/resource_index.hpp"
#include "../../app/route_sources.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/neighborhood_id.hpp"
#include "formats/qfs.hpp"
#include "scene/animation_library.hpp"
#include "scene/assets.hpp"
#include "scene/outfit_properties.hpp"
#include "sim/behavior_closure.hpp"
#include "sim/index_code_source.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/support_table.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: simbank <install-folder> <lot.package> <out-folder> [ticks]\n");
        return 1;
    }
    namespace fs = std::filesystem;
    try {
        const fs::path install = argv[1], lot = argv[2], out = argv[3];
        const std::size_t ticks = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 3000;
        using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
        std::map<Key, std::vector<std::uint8_t>> gathered;
        // Only what the simulation itself reads goes in the code package: what the game's models and animations are asked for goes to support.bin.
        bool recording = true;
        struct Pause {
            bool& flag;
            bool was;
            explicit Pause(bool& f) : flag(f), was(f) { flag = false; }
            ~Pause() { flag = was; }
        };
        ts2::content::ResourceIndex::sharedObserver = [&](const ts2::content::ResourceIndex::Resource& r, const std::vector<std::uint8_t>& bytes) {
            if (!recording) {
                return;
            }
            const auto& k = r.entry.key;
            gathered.try_emplace(Key{k.type, k.group, k.instance, k.instanceHigh}, bytes);
        };
        // The behaviours looked up by name (Run Tree by Name and the like), for the bank to answer without scanning.
        ts2::sim::NameTable names;
        ts2::sim::IndexCodeSource::nameObserver = [&](std::uint32_t group, const std::string& name, std::optional<std::uint16_t> found) {
            if (found && recording) {
                names.emplace(std::pair{group, name}, *found);
            }
        };
        auto support = std::make_shared<ts2::sim::SupportTable>();
        {
            ts2::sim::LotSimulation simulation(install, lot, {});
            std::optional<Pause> pause(recording);
            ts2::scene::Assets assets(install);
            ts2::scene::AnimationLibrary animations(assets);
            ts2::scene::OutfitProperties outfits(assets);
            // Routing's models and fences as the app has them, every model of the lot's objects taken at once; what the run asks of them is recorded as well.
            const auto models = app::readRouteModels(assets, lot);
            for (const auto& [name, model] : models->models) {
                support->routeModels.emplace(name, model);
            }
            support->posts = models->posts;
            ts2::sim::RouteSources real = app::routeSources(assets, models);
            ts2::sim::RouteSources recording_;
            recording_.fenceBlocks = [&, real](std::uint32_t guid) {
                const bool blocks = real.fenceBlocks(guid);
                support->fenceBlocks[guid] = blocks;
                return blocks;
            };
            recording_.postFootprint = [&, real](std::uint32_t guid) {
                auto footprint = real.postFootprint(guid);
                support->posts[guid] = footprint;
                return footprint;
            };
            recording_.models = [&, real](const std::string& name) -> const ts2::sim::RouteModel* {
                const auto* model = real.models(name);
                if (model != nullptr) {
                    support->routeModels.try_emplace(name, *model);
                }
                return model;
            };
            simulation.setRouteSources(recording_);
            pause.reset();
            auto& world = simulation.world();
            world.setAnimationLengths([&](const std::string& name) {
                Pause pause(recording);
                auto length = animations.length(name);
                support->animationLengths[name] = length;
                return length;
            });
            world.setAnimationCarries([&](const std::string& name) {
                Pause pause(recording);
                const bool carries = animations.hasTargetType(name, 2);
                support->animationCarries[name] = carries;
                return carries;
            });
            world.setLocoClips([&](const std::string& name) -> std::optional<ts2::sim::LocoClip> {
                Pause pause(recording);
                const auto a = animations.find(name);
                std::optional<ts2::sim::LocoClip> clip;
                if (a) {
                    clip = ts2::sim::LocoClip{name, a->duration, a->travel, a->loopPhase, a->match, a->speed, a->rootTurn};
                }
                support->locoClips[name] = clip;
                return clip;
            });
            world.setAnimationEventTime([&](const std::string& name, int id) {
                Pause pause(recording);
                auto time = animations.eventTime(name, id);
                support->eventTimes[{name, id}] = time;
                return time;
            });
            world.setAnimationEvents([&](const std::string& name) {
                Pause pause(recording);
                std::vector<ts2::sim::AnimationEvent> events;
                for (const auto& [ms, id] : animations.simAnticsEvents(name)) {
                    events.push_back({ms, id});
                }
                support->events[name] = events;
                return events;
            });
            world.setAnimationTurns([&](const std::string& name, float ms) {
                Pause pause(recording);
                auto turn = animations.rootTurn(name, ms);
                if (turn) {
                    support->turns[{name, static_cast<int>(std::lround(ms))}] = *turn;
                }
                return turn;
            });
            world.setOutfitProperty([&](std::int16_t neighbor, std::uint16_t category, std::uint16_t property) -> std::optional<std::int16_t> {
                Pause pause(recording);
                std::optional<std::int16_t> value;
                if (const auto file = simulation.characterFile(neighbor)) {
                    value = outfits.get(*file, category, property);
                } else if (const auto group = simulation.characterGroup(neighbor)) {
                    value = outfits.get(*group, category, property);
                }
                support->outfitProperties[{neighbor, category, property}] = value;
                return value;
            });
            {
                // Every animation of the global age lists, asked once: a Sim that takes another path on the console plays animations the run here never did, and an
                // animation the table does not know is taken for one that does not exist.
                ts2::content::ResourceIndex listIndex;
                for (const auto& path : ts2::content::objectLoadOrder(ts2::content::findProducts(install))) {
                    listIndex.addPackage(path);
                }
                ts2::sim::IndexCodeSource listStrings(listIndex);
                std::size_t names = 0;
                for (const std::uint16_t list : {0x81, 0x82, 0x89, 0x91}) {
                    for (std::size_t entry = 0; entry < 400; ++entry) {
                        const auto text = listStrings.string(ts2::sim::kGlobalGroup, list, entry);
                        if (!text) {
                            continue;
                        }
                        const std::string name = ts2::content::animationName(*text);
                        if (name.empty()) {
                            continue;
                        }
                        world.animationLengths()(name);
                        world.animationCarries()(name);
                        world.locoClips()(name);
                        world.animationEvents()(name);
                        ++names;
                    }
                }
                std::printf("simbank: %zu animations of the global age lists asked once\n", names);
            }
            for (std::size_t i = 0; i < ticks; ++i) {
                simulation.tick();
            }
            // The same fingerprint tools/simmem prints, so that a run on the console can be compared with this one that had the game's files.
            unsigned checksum = 0, walkers = 0;
            simulation.world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
                checksum = checksum * 31 + static_cast<unsigned>(o.x) + 7 * static_cast<unsigned>(o.y) + static_cast<unsigned>(o.level);
                walkers += o.walking ? 1 : 0;
                return false;
            });
            std::printf("simbank: fingerprint after %zu ticks: %08x, %u Sims walking\n", ticks, checksum, walkers);
        }
        if (std::getenv("SIMBANK_NO_CLOSURE") == nullptr) {
            // Everything the lot's objects could run, not only what these ticks did: a Sim that takes another path (the console's floats differ) needs behaviours
            // this run never reached. The reads and name lookups of the walk are recorded like the run's.
            ts2::content::ResourceIndex index;
            for (const auto& path : ts2::content::objectLoadOrder(ts2::content::findProducts(install))) {
                index.addPackage(path);
            }
            ts2::content::addCharacters(index, ts2::content::neighborhoodOfLot(lot));
            ts2::sim::IndexCodeSource code(index);
            const auto lotContent = ts2::content::loadLot(lot);
            std::set<std::uint32_t> guids;
            for (const auto& object : lotContent.objects) {
                if (object.type) {
                    guids.insert(object.type->guid);
                }
            }
            for (const std::uint32_t guid : code.objectTypeGuids()) {
                // (the objects Sims' interactions create at run time)
                if (const auto* t = code.objectType(guid); t != nullptr && t->definition.name.rfind("Social", 0) == 0) {
                    guids.insert(guid);
                }
            }
            ts2::sim::BehaviorClosure closure{code};
            closure.addTypes(guids);
            closure.run();
            std::set<std::uint32_t> groups{ts2::sim::kGlobalGroup};
            for (const auto& [g, i] : closure.trees) {
                groups.insert(g);
            }
            for (const auto& r : index.resources()) {
                if (r.entry.key.type == 0x42434F4Eu /* BCON */ && groups.contains(r.entry.key.group)) {
                    index.read(r);
                }
            }
            // The global string tables (the animation lists of every age among them): a Sim that takes another path reads other entries of them.
            for (const auto& r : index.resources()) {
                if (r.entry.key.type == ts2::types::kStringTable && r.entry.key.group == ts2::sim::kGlobalGroup) {
                    index.read(r);
                }
            }
            std::printf("simbank: closure %zu trees in %zu groups (%zu object types, %zu Run Tree by Name sites)\n", closure.trees.size(), groups.size(), guids.size(), closure.namedSites);
        }
        ts2::content::ResourceIndex::sharedObserver = nullptr;
        ts2::sim::IndexCodeSource::nameObserver = nullptr;
        {
            // The neighborhoods' identities, which the simulation otherwise finds by opening every neighborhood's packages.
            std::uint32_t number = 0;
            std::error_code ec;
            for (const auto& folder : fs::directory_iterator(ts2::content::neighborhoodOfLot(lot).parent_path(), ec)) {
                for (const auto& item : fs::directory_iterator(folder.path(), ec)) {
                    if (item.path().extension() != ".package" || !item.is_regular_file(ec)) {
                        continue;
                    }
                    try {
                        ts2::dbpf::Package package{item.path()};
                        for (const auto& e : package.entries()) {
                            if (e.key.type == ts2::nhoodid::kType) {
                                gathered.emplace(Key{ts2::nhoodid::kType, 0, number++, 0}, package.read(e));
                                break;
                            }
                        }
                    } catch (const std::exception&) {
                    }
                }
            }
        }
        gathered.emplace(Key{ts2::sim::kNameTableType, 0, 0, 0}, ts2::sim::writeNameTable(names));

        const fs::path hood = ts2::content::neighborhoodOfLot(lot);
        const fs::path hoodOut = out / "Neighborhoods" / hood.filename();
        fs::create_directories(hoodOut / "Lots");
        {
            std::ofstream file(out / "code.package", std::ios::binary | std::ios::trunc);
            ts2::dbpf::PackageWriter writer(file);
            std::size_t bytes = 0;
            std::size_t stored = 0, compressedCount = 0;
            for (const auto& [key, data] : gathered) {
                const ts2::dbpf::ResourceKey resourceKey{std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key)};
                // Compressed when that saves space (the console keeps the whole package in memory), after checking it unpacks to the same bytes.
                if (data.size() >= 64) {
                    const auto packed = ts2::qfs::compress(data);
                    if (packed.size() < data.size() && ts2::qfs::decompress(packed) == data) {
                        writer.add(resourceKey, packed, true, static_cast<std::uint32_t>(data.size()));
                        stored += packed.size();
                        ++compressedCount;
                        bytes += data.size();
                        continue;
                    }
                }
                writer.add(resourceKey, data, false, static_cast<std::uint32_t>(data.size()));
                stored += data.size();
                bytes += data.size();
            }
            writer.finish(0);
            std::printf("code.package: %zu resources, %.2f MiB (%.2f MiB stored, %zu compressed)\n", gathered.size(), static_cast<double>(bytes) / 1048576.0,
                        static_cast<double>(stored) / 1048576.0, compressedCount);
        }
        {
            const auto bytes = ts2::sim::writeSupportTable(*support);
            std::ofstream(out / "support.bin", std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            std::printf("support.bin: %zu route models, %zu animation lengths, %zu loco clips, %zu root turns, %.2f MiB\n", support->routeModels.size(), support->animationLengths.size(),
                        support->locoClips.size(), support->turns.size(), static_cast<double>(bytes.size()) / 1048576.0);
        }
        std::string extra;  // sim.cfg lines for the files copied
        if (const auto wants = ts2::content::wantsPackage(install)) {
            fs::copy_file(*wants, out / "wants.package", fs::copy_options::overwrite_existing);
            extra += "wants=wants.package\n";
        }
        if (const auto tuning = ts2::content::wantTuningPackage(install)) {
            fs::copy_file(*tuning, out / "wanttuning.package", fs::copy_options::overwrite_existing);
            extra += "tuning=wanttuning.package\n";
        }
        fs::copy_file(lot, hoodOut / "Lots" / lot.filename(), fs::copy_options::overwrite_existing);
        const fs::path hoodPackage = ts2::content::neighborhoodPackage(hood);
        std::size_t droppedWants = 0, keptWants = 0;
        if (fs::exists(hoodPackage)) {
            // The neighborhood without the wants and fears of the neighbors who are not on the lot (nearly all of them; they are the largest part of what it holds in memory):
            // a neighbor without a saved record gets an empty one, as the game gives a new Sim.
            const ts2::content::Neighborhood record = ts2::content::loadNeighborhood(hoodPackage, false);
            const auto lotContent = ts2::content::loadLot(lot);
            std::set<std::uint32_t> present;
            for (const auto& object : lotContent.objects) {
                if (object.type) {
                    present.insert(object.type->guid);
                }
            }
            std::set<std::uint32_t> keep;
            for (const auto& [id, neighbor] : record.neighbors) {
                if (present.contains(neighbor.guid)) {
                    keep.insert(static_cast<std::uint32_t>(id));
                }
            }
            ts2::dbpf::Package source(hoodPackage);
            std::ofstream file(hoodOut / hoodPackage.filename(), std::ios::binary | std::ios::trunc);
            ts2::dbpf::PackageWriter writer(file);
            for (const auto& e : source.entries()) {
                if (e.key.type == ts2::types::kDirectory) {
                    continue;  // the writer makes its own
                }
                if (e.key.type == ts2::types::kWantsAndFears) {
                    if (!keep.contains(e.key.instance)) {
                        ++droppedWants;
                        continue;
                    }
                    ++keptWants;
                }
                const auto data = source.read(e);
                if (data.size() >= 64) {
                    const auto packed = ts2::qfs::compress(data);
                    if (packed.size() < data.size() && ts2::qfs::decompress(packed) == data) {
                        writer.add(e.key, packed, true, static_cast<std::uint32_t>(data.size()));
                        continue;
                    }
                }
                writer.add(e.key, data, false, static_cast<std::uint32_t>(data.size()));
            }
            writer.finish(0);
            std::printf("neighborhood: wants and fears kept for %zu neighbors, dropped for %zu\n", keptWants, droppedWants);
        }
        std::ofstream(out / "sim.cfg") << "packs=" << ts2::content::installedPacksMask(ts2::content::findProducts(install)) << "\nlot=Neighborhoods/" << hood.filename().string() << "/Lots/"
                                       << lot.filename().string() << "\n" << extra;
        std::printf("tree written to %s (lot %.2f MiB, neighborhood %.2f MiB)\n", out.c_str(), static_cast<double>(fs::file_size(lot)) / 1048576.0,
                    fs::exists(hoodOut / hoodPackage.filename()) ? static_cast<double>(fs::file_size(hoodOut / hoodPackage.filename())) / 1048576.0 : 0.0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}

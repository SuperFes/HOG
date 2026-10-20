#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/anchors.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

// What trees and bushes share in anchoring themselves: sampling the world at their own spots, and
// keeping tiles of anchors round the eye in an anchor pool's slots.
namespace hog::client::tiles {

// The ground at a spot given by its face and its s and t, 0 to 1 across it.
struct Ground {
    DVec3 direction;
    double elevation; // metres above sea level
    planet::CellState state;
};

[[nodiscard]] Ground ground_at(const planet::World& world, std::uint32_t face, double s, double t);

// The ground's normal leaning over the hillside `half` each way of a spot in s and t, so a plant
// minds the hill it stands on, not the bumps between its roots.
[[nodiscard]] DVec3 hillside(const planet::World& world, std::uint32_t face, double s, double t,
                             double half, const Ground& ground);

// Metres across a tile of `level`, about.
[[nodiscard]] double span_of(double radius, int level);

// Metres from the eye to the tile's nearest ground, generously.
[[nodiscard]] double distance_to(WorldPosition eye, WorldPosition origin, double span);

// The tiles of `level` in reach, flooded outward from the eye's own so face seams are crossed.
[[nodiscard]] std::set<planet::CellId> wanted(WorldPosition eye, double radius, int level,
                                              double reach);

// Whether a tile and a cell share any ground: one lies within the other.
[[nodiscard]] bool over(planet::CellId tile, planet::CellId cell);

// The tiles of one level kept round the eye, each bound to a slot of an anchor pool. `Tile` keeps
// its `slot` and a `stale` flag; `Built` is what building one gives, before it is staged.
template <class Tile, class Built>
class Keeper {
public:
    explicit Keeper(int level) noexcept : level_{level} {}

    // Keeps the tiles within `reach` of the eye built and staged, nearest first; tiles well beyond
    // the reach give their slots back, and stale ones are built again into their own slots, drawn
    // as they were until then. `make(cell, origin)` builds a tile from the world: on the job pool
    // when there is one, so it must hold copies of whatever it reads besides the world, which must
    // not change while builds are in flight. `hold(built, slot, origin)` stages what was built into
    // the slot, on this thread, and gives back the tile to hold. At most `builds` tiles are staged
    // a call.
    template <class Make, class Hold>
    Result<> keep(render::AnchorPool& pool, jobs::JobSystem* jobs, const planet::World& world,
                  WorldPosition eye, double reach, int builds, const Make& make, const Hold& hold) {
        const double radius = world.geology().planet_radius();
        const double span = span_of(radius, level_);
        const std::set<planet::CellId> in_reach = wanted(eye, radius, level_, reach);
        const auto origin_of = [&](planet::CellId cell) {
            return WorldPosition{planet::cell_center(cell) * radius};
        };

        for (auto held = tiles_.begin(); held != tiles_.end();) {
            if (!in_reach.contains(held->first) &&
                distance_to(eye, origin_of(held->first), span) >= reach + (2.0 * span)) {
                pool.release(held->second.slot);
                held = tiles_.erase(held);
            } else {
                ++held;
            }
        }

        // Those built since the last call into their slots, unless they've gone out of reach.
        int staged = 0;
        bool full = false;
        for (auto at = building_.begin(); at != building_.end() && staged < builds;) {
            if (!at->second.ready && !at->second.job->done()) {
                ++at;
                continue;
            }
            const planet::CellId cell = at->first;
            Building arrived = std::move(at->second);
            at = building_.erase(at);
            if (!in_reach.contains(cell) && !tiles_.contains(cell)) {
                continue;
            }
            Built built = arrived.ready ? std::move(*arrived.ready) : arrived.job->take();
            auto put = put_in(pool, cell, arrived.origin, std::move(built), arrived.stale, hold);
            if (!put) {
                return std::unexpected(std::move(put).error());
            }
            staged += *put ? 1 : 0;
            full = full || !*put;
        }

        // The rest still to build, nearest first.
        std::vector<std::pair<double, planet::CellId>> missing;
        for (const planet::CellId cell : in_reach) {
            const auto held = tiles_.find(cell);
            if (!building_.contains(cell) && (held == tiles_.end() || held->second.stale)) {
                missing.emplace_back(distance_to(eye, origin_of(cell), span), cell);
            }
        }
        std::ranges::sort(missing);
        // Kept few, so the nearest wanted go first as the eye moves on.
        const auto most_building = static_cast<std::size_t>(2 * builds);
        std::size_t left = 0;
        for (const auto& [distance, cell] : missing) {
            const bool held = tiles_.contains(cell);
            // A new tile needs a slot to land in.
            const auto claimed = static_cast<std::size_t>(std::ranges::count_if(
                building_, [&](const auto& building) { return !tiles_.contains(building.first); }));
            if (full || (!held && claimed >= pool.free_slots())) {
                full = true;
                continue;
            }
            if (jobs == nullptr) {
                if (staged >= builds) {
                    ++left;
                    continue;
                }
                auto put =
                    put_in(pool, cell, origin_of(cell), make(cell, origin_of(cell)), false, hold);
                if (!put) {
                    return std::unexpected(std::move(put).error());
                }
                staged += *put ? 1 : 0;
                full = !*put;
                continue;
            }
            if (building_.size() >= most_building) {
                ++left;
                continue;
            }
            const WorldPosition origin = origin_of(cell);
            building_.emplace(
                cell, Building{.job = std::make_unique<jobs::Async<Built>>(
                                   *jobs, [make, cell, origin] { return make(cell, origin); }),
                               .origin = origin});
        }
        // A full pool leaves nothing more to wait for.
        settled_ = building_.empty() && left == 0 && staged == 0;
        return {};
    }

    // A cell of the world changed: the tiles over it are dropped, to be built again.
    void drop_over(render::AnchorPool& pool, planet::CellId cell) {
        for (auto held = tiles_.begin(); held != tiles_.end();) {
            if (over(held->first, cell)) {
                pool.release(held->second.slot);
                held = tiles_.erase(held);
            } else {
                ++held;
            }
        }
        std::erase_if(building_, [&](const auto& building) { return over(building.first, cell); });
    }

    // The tiles over any of the cells, and those building there, are to be built again, drawn as
    // they were until then.
    void stale_over(std::span<const planet::CellId> changed) {
        const auto touched = [&](planet::CellId tile) {
            return std::ranges::any_of(changed,
                                       [&](planet::CellId cell) { return over(tile, cell); });
        };
        for (auto& [tile, held] : tiles_) {
            held.stale = held.stale || touched(tile);
        }
        for (auto& [tile, building] : building_) {
            building.stale = building.stale || touched(tile);
        }
    }

    // Waits for the builds in flight, for the next keeps to stage.
    void finish() {
        for (auto& [tile, building] : building_) {
            if (!building.ready) {
                building.ready = building.job->take();
            }
        }
    }

    [[nodiscard]] const std::map<planet::CellId, Tile>& tiles() const noexcept { return tiles_; }
    // Whether the last keep found nothing more to build or stage within its reach.
    [[nodiscard]] bool settled() const noexcept { return settled_; }

private:
    struct Building {
        std::unique_ptr<jobs::Async<Built>> job;
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        std::optional<Built> ready = {}; // taken from the job already, by finish()
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        WorldPosition origin = {};
        bool stale = false; // what it was built from changed since it started
    };

    // Stages a tile built into its own slot, or a new one; false if the pool has none left.
    template <class Hold>
    Result<bool> put_in(render::AnchorPool& pool, planet::CellId cell, WorldPosition origin,
                        Built built, bool stale, const Hold& hold) {
        const auto held = tiles_.find(cell);
        const std::optional<std::uint32_t> slot =
            held != tiles_.end() ? std::optional{held->second.slot} : pool.acquire();
        if (!slot) {
            return false;
        }
        Result<Tile> tile = hold(std::move(built), *slot, origin);
        if (!tile) {
            if (held == tiles_.end()) {
                pool.release(*slot);
            }
            return std::unexpected(std::move(tile).error());
        }
        tile->stale = stale;
        tiles_.insert_or_assign(cell, std::move(tile).value());
        return true;
    }

    int level_;
    std::map<planet::CellId, Tile> tiles_;
    std::map<planet::CellId, Building> building_;
    bool settled_ = false;
};

} // namespace hog::client::tiles

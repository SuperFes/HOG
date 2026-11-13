#include <hog/flow/region.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace hog::flow {
namespace {

using globe::Material;
using globe::Site;
using globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// Passes a wash is stepped in, so the ground it changes steers the water after.
constexpr int wash_passes = 8;

// Earth that running water washes: loose, and not snow.
bool earthy(Material material) {
    return material != Material::Air && material != Material::Snow &&
           globe::properties(material).repose > 0.0;
}

// Where the water off a column runs. Runoff floods a hollow from its rim, so on the dry part of
// one, where the rain that falls runs down to what stands in it, that's its lowest neighbour.
int drains_to(const Runoff& runoff, std::size_t n) {
    if (!runoff.in_hollow(n)) {
        return runoff.drains(n);
    }
    int lowest = Wash::none;
    for (const int beside : runoff[n].next) {
        if (beside != Runoff::none &&
            runoff[static_cast<std::size_t>(beside)].ground < runoff[n].ground &&
            (lowest == Wash::none || runoff[static_cast<std::size_t>(beside)].ground <
                                         runoff[static_cast<std::size_t>(lowest)].ground)) {
            lowest = beside;
        }
    }
    return lowest;
}

} // namespace

std::int64_t Region::weather_column(Site top, std::int64_t ground, std::uint32_t low,
                                    const globe::Climate& climate, double depth) {
    const double thickness = world_->layers().thickness;
    const Voxel* surface = voxel(top);
    if (surface == nullptr) {
        return 0;
    }
    const globe::Horizons layers = globe::horizons(surface->solid, climate, depth);
    std::int64_t changed = 0;
    for (Site site = top; site.layer >= low; --site.layer) {
        Voxel* here = voxel(site);
        if (here == nullptr || here->solid == Material::Air) {
            break;
        }
        const double below =
            site.layer == top.layer
                ? 0.0
                : static_cast<double>(ground - (std::int64_t{site.layer + 1} * full)) /
                      static_cast<double>(full) * thickness;
        const Material weathered = layers.at(below);
        if (weathered == Material::Air) {
            break;
        }
        if (globe::horizons(here->solid, climate, depth).depth > 0.0) {
            here->solid = weathered;
            if (Held* const brick = held(globe::brick_of(site))) {
                brick->dirty = true;
            }
            ++changed;
        }
        if (site.layer == 0) {
            break;
        }
    }
    return changed;
}

Stepped Region::weather_ground(const ClimateAt& climate, double depth) {
    ++steps_;
    Stepped stepped;
    const globe::Layers& layers = world_->layers();
    for (const auto& [stack, layers_held] : stacks_) {
        const auto face = static_cast<std::uint8_t>(stack >> 42U);
        const auto bi = static_cast<std::uint32_t>((stack >> 21U) & 0x1FFFFFU);
        const auto bj = static_cast<std::uint32_t>(stack & 0x1FFFFFU);
        const std::uint32_t low = layers_held.front() * globe::brick_side;
        const std::uint32_t high = ((layers_held.back() + 1) * globe::brick_side) - 1;
        for (std::uint32_t a = 0; a < globe::brick_side; ++a) {
            for (std::uint32_t b = 0; b < globe::brick_side; ++b) {
                const std::uint32_t i = (bi * globe::brick_side) + a;
                const std::uint32_t j = (bj * globe::brick_side) + b;
                const std::vector<Scan> floors_up = floors(face, i, j, low, high);
                if (floors_up.empty() || floors_up.back().surface.first == 0) {
                    continue;
                }
                const Scan& top = floors_up.back();
                const Site site{.face = face, .i = i, .j = j, .layer = top.surface.first - 1};
                const double radius = layers.floor + (static_cast<double>(top.ground) /
                                                      static_cast<double>(full) * layers.thickness);
                const DVec3 up = glm::normalize(globe::middle_of(site, layers));
                stepped.changed +=
                    weather_column(site, top.ground, low, climate(up, radius), depth);
            }
        }
    }
    return stepped;
}

std::vector<Wash::Piece> Region::loose_under(const Top& top) {
    const double thickness = world_->layers().thickness;
    const auto metres = [&](std::int64_t units) {
        return static_cast<double>(units) / static_cast<double>(full) * thickness;
    };
    // Gathered top down.
    std::vector<Wash::Piece> pieces;
    if (top.grain_amount > 0) {
        pieces.push_back({.material = top.grain, .depth = metres(top.grain_amount)});
    }
    const std::size_t shaped = pieces.size();
    // The open voxel holds the top of the shaped matter under it.
    const Voxel* bed = voxel({.face = top.face, .i = top.i, .j = top.j, .layer = top.first - 1});
    for (std::uint32_t layer = top.first;; --layer) {
        const Voxel* here = voxel({.face = top.face, .i = top.i, .j = top.j, .layer = layer});
        if (here == nullptr || (layer != top.first && here->solid == Material::Air)) {
            break;
        }
        const Material material = layer == top.first ? bed->solid : here->solid;
        if (!earthy(material)) {
            break;
        }
        if (here->solid_amount > 0) {
            if (pieces.size() > shaped && pieces.back().material == material) {
                pieces.back().depth += metres(here->solid_amount);
            } else {
                pieces.push_back({.material = material, .depth = metres(here->solid_amount)});
            }
        }
        if (layer == 0) {
            break;
        }
    }
    std::ranges::reverse(pieces);
    return pieces;
}

void Region::pile_pieces(Site column, std::uint32_t voxels,
                         const std::vector<Wash::Piece>& pieces) {
    const double thickness = world_->layers().thickness;
    // Each piece's top, in units up from the column's ground.
    std::vector<std::int64_t> tops;
    std::int64_t total = 0;
    for (const Wash::Piece& piece : pieces) {
        total += std::llround(piece.depth / thickness * static_cast<double>(full));
        tops.push_back(total);
    }
    std::int64_t below = 0;
    for (std::uint32_t up = 0; up < voxels; ++up) {
        const Site site{
            .face = column.face, .i = column.i, .j = column.j, .layer = column.layer + up};
        Voxel* here = voxel(site);
        if (here == nullptr) {
            break;
        }
        // Grain lies over the shaped matter the voxel holds.
        const std::int64_t room =
            full - (here->solid == Material::Air ? std::int64_t{here->solid_amount} : full);
        const std::int64_t amount = std::clamp<std::int64_t>(total - below, 0, room);
        // A voxel is what lies at the middle of the grain in it.
        const std::int64_t middle = below + (amount / 2);
        Material material = Material::Air;
        for (std::size_t k = 0; k < pieces.size() && amount > 0; ++k) {
            if (middle < tops[k] || k + 1 == pieces.size()) {
                material = pieces[k].material;
                break;
            }
        }
        if (std::cmp_not_equal(here->grain_amount, amount) || here->grain != material) {
            here->grain = material;
            here->grain_amount = static_cast<std::uint16_t>(amount);
            if (Held* const brick = held(globe::brick_of(site))) {
                brick->dirty = true;
            }
        }
        below += amount;
    }
}

std::int64_t Region::relay(const Top& top, const Wash::Node& washed, double taken) {
    const double thickness = world_->layers().thickness;
    const Site column{.face = top.face, .i = top.i, .j = top.j, .layer = top.first};
    const std::int64_t off = dithered(taken / thickness * static_cast<double>(full), column, 11);
    std::vector<Wash::Piece> laid;
    for (const Wash::Piece& piece : washed.loose) {
        if (piece.laid) {
            laid.push_back(piece);
        }
    }
    if (off <= 0 && laid.empty()) {
        return 0;
    }
    const std::int64_t from_grain = std::min(off, top.grain_amount);
    if (off > from_grain) {
        dig_units(column, off - from_grain);
    }
    std::vector<Wash::Piece> piled;
    if (top.grain_amount > from_grain) {
        piled.push_back({.material = top.grain,
                         .depth = static_cast<double>(top.grain_amount - from_grain) /
                                  static_cast<double>(full) * thickness});
    }
    std::int64_t moved = off;
    for (const Wash::Piece& piece : laid) {
        piled.push_back(piece);
        moved += std::llround(piece.depth / thickness * static_cast<double>(full));
    }
    // Down from the top of the open run to the ground as it now stands.
    const std::uint32_t highest = top.first + std::max<std::uint32_t>(top.open, 1) - 1;
    const auto reach = static_cast<std::uint32_t>(top.open + 2 + (off / full));
    if (const std::optional<Scan> now = scan(top.face, top.i, top.j, highest, reach)) {
        pile_pieces({.face = top.face, .i = top.i, .j = top.j, .layer = now->surface.first},
                    now->surface.open, piled);
    }
    return moved;
}

Stepped Region::wash(const Rainfall& rainfall, double years) {
    ++steps_;
    Stepped stepped;
    const std::vector<Top> held_tops = tops();
    if (held_tops.empty() || years <= 0.0) {
        return stepped;
    }
    const Runoff runoff = run_off(held_tops, rainfall);
    const std::optional<double> sea = world_->generator().sea_level();
    std::vector<Wash::Node> nodes(held_tops.size());
    for (std::size_t n = 0; n < held_tops.size(); ++n) {
        const int drains = drains_to(runoff, n);
        // On the dry side of a hollow Runoff sends the water straight to what stands in it.
        const bool dry_hollow = runoff.in_hollow(n) && !runoff.standing(n);
        nodes[n] = {.ground = held_tops[n].ground,
                    .loose = loose_under(held_tops[n]),
                    .discharge = runoff.discharge(n),
                    .gathers = dry_hollow,
                    .runs_off = runoff.runs_off(n),
                    .drains = drains,
                    .still = runoff.standing(n) || (sea && held_tops[n].ground < *sea) ||
                             (dry_hollow && drains == Wash::none)};
    }
    Wash washing{std::move(nodes), {.cell = column_width(held_tops.front())}};
    washing.run(years, wash_passes);
    for (std::size_t n = 0; n < held_tops.size(); ++n) {
        stepped.washed += relay(held_tops[n], washing[n], washing.taken(n));
    }
    return stepped;
}

} // namespace hog::flow

#include <hog/client/anchors.hpp>
#include <hog/flora/stand.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace hog::client {

namespace {

// Trees keep off ground this close to sea level, and off slopes steeper than this much up.
constexpr double shore = 1.0;
constexpr double steepest = 0.7;

// Lattice cells across a tile's side.
constexpr std::uint32_t tile_side = 1U << (render::tree_lattice_level - anchor_tile_level);

// The ground under a lattice spot: where it lies, how high, and what grows there.
struct Ground {
    DVec3 direction;
    double elevation;
    planet::Biome biome;
};

Ground ground_at(const planet::World& world, std::uint32_t face, double s, double t) {
    const DVec3 direction = glm::normalize(
        planet::face_uv_to_xyz(static_cast<int>(face), planet::st_to_uv(s), planet::st_to_uv(t)));
    const planet::CellState state = world.state(planet::cell_at(direction));
    return {.direction = direction,
            .elevation =
                static_cast<double>(state.elevation) / static_cast<double>(planet::fixed_one),
            .biome = state.biome};
}

// Metres from the eye to the tile's nearest ground, generously.
double distance_to(WorldPosition eye, WorldPosition origin, double span) {
    const DVec3 up = glm::normalize(eye.metres());
    const DVec3 away = origin - eye;
    return std::max(0.0, glm::length(away - (up * glm::dot(away, up))) - span);
}

// The tiles in reach, flooded outward from the eye's own so face seams are crossed.
std::set<planet::CellId> tiles_wanted(WorldPosition eye, double radius, double span, double reach) {
    std::set<planet::CellId> wanted;
    std::vector<planet::CellId> frontier{planet::cell_at(eye, anchor_tile_level)};
    while (!frontier.empty()) {
        const planet::CellId cell = frontier.back();
        frontier.pop_back();
        if (!wanted.insert(cell).second) {
            continue;
        }
        for (const planet::CellId beside : planet::edge_neighbors(cell)) {
            if (wanted.contains(beside)) {
                continue;
            }
            const WorldPosition origin{planet::cell_center(beside) * radius};
            if (distance_to(eye, origin, span) < reach) {
                frontier.push_back(beside);
            }
        }
    }
    return wanted;
}

} // namespace

std::vector<render::TreeAnchor> tile_anchors(const planet::World& world, planet::CellId tile,
                                             WorldPosition origin, const BiomePalette& needles) {
    std::vector<render::TreeAnchor> anchors;
    const double radius = world.geology().planet_radius();
    const planet::FaceIJ at = tile.to_face_ij();
    const auto face = static_cast<std::uint32_t>(at.face);
    const auto across = static_cast<double>(std::uint32_t{1} << render::tree_lattice_level);
    // The normal leans over the hillside around the spot, half a lattice cell each way: a tree
    // minds the hill it stands on, not the bumps between its roots.
    const double half = 0.5 / across;
    for (std::uint32_t a = 0; a < tile_side; ++a) {
        for (std::uint32_t b = 0; b < tile_side; ++b) {
            const std::uint32_t i = (static_cast<std::uint32_t>(at.i) * tile_side) + a;
            const std::uint32_t j = (static_cast<std::uint32_t>(at.j) * tile_side) + b;
            const Vec2 spot = flora::tree_spot(i, j, face);
            const double s = (static_cast<double>(i) + static_cast<double>(spot.x)) / across;
            const double t = (static_cast<double>(j) + static_cast<double>(spot.y)) / across;
            const Ground ground = ground_at(world, face, s, t);
            const std::optional<flora::Stood> stood = flora::tree_grown(i, j, face, ground.biome);
            if (!stood || ground.elevation < shore) {
                continue;
            }
            const Ground east = ground_at(world, face, s + half, t);
            const Ground west = ground_at(world, face, s - half, t);
            const Ground north = ground_at(world, face, s, t + half);
            const Ground south = ground_at(world, face, s, t - half);
            const DVec3 root = ground.direction * (radius + ground.elevation);
            const DVec3 to_east = (east.direction * (radius + east.elevation)) -
                                  (west.direction * (radius + west.elevation));
            const DVec3 to_north = (north.direction * (radius + north.elevation)) -
                                   (south.direction * (radius + south.elevation));
            DVec3 normal = glm::normalize(glm::cross(to_east, to_north));
            if (glm::dot(normal, ground.direction) < 0.0) {
                normal = -normal;
            }
            if (glm::dot(normal, ground.direction) < steepest) {
                continue;
            }
            anchors.push_back(
                {.position = Vec3{root - origin.metres()},
                 .height = stood->height,
                 .normal = Vec3{normal},
                 .hash = stood->hash,
                 .colour = flora::needle_colour(needles[static_cast<std::size_t>(ground.biome)]),
                 .wooded = stood->wooded});
        }
    }
    return anchors;
}

TreeAnchors::TreeAnchors(const BiomePalette& palette) {
    std::ranges::copy(palette, needles_.begin());
}

Result<> TreeAnchors::update(render::AnchorPool& pool, std::uint32_t frame,
                             const planet::World& world, WorldPosition eye, double reach,
                             int builds) {
    const double radius = world.geology().planet_radius();
    const double span = (std::numbers::pi / 2.0) * radius /
                        static_cast<double>(std::uint32_t{1} << anchor_tile_level);
    const std::set<planet::CellId> wanted = tiles_wanted(eye, radius, span, reach);

    // Tiles well beyond the reach give their slots back.
    for (auto held = tiles_.begin(); held != tiles_.end();) {
        const WorldPosition origin{planet::cell_center(held->first) * radius};
        if (!wanted.contains(held->first) &&
            distance_to(eye, origin, span) >= reach + (2.0 * span)) {
            pool.release(held->second.slot);
            held = tiles_.erase(held);
        } else {
            ++held;
        }
    }

    // The missing tiles, built nearest first, a few a call.
    std::vector<std::pair<double, planet::CellId>> missing;
    for (const planet::CellId cell : wanted) {
        if (!tiles_.contains(cell)) {
            const WorldPosition origin{planet::cell_center(cell) * radius};
            missing.emplace_back(distance_to(eye, origin, span), cell);
        }
    }
    std::ranges::sort(missing);
    settled_ = missing.empty();
    int built = 0;
    for (const auto& [distance, cell] : missing) {
        if (built >= builds) {
            break;
        }
        const std::optional<std::uint32_t> slot = pool.acquire();
        if (!slot) {
            // The pool is full: nothing more can be built, so nothing more is waited for.
            settled_ = true;
            break;
        }
        const WorldPosition origin{planet::cell_center(cell) * radius};
        const std::vector<render::TreeAnchor> anchors = tile_anchors(world, cell, origin, needles_);
        if (auto staged = pool.stage(frame, *slot, anchors); !staged) {
            pool.release(*slot);
            return staged;
        }
        tiles_.emplace(cell, Tile{.slot = *slot,
                                  .count = static_cast<std::uint32_t>(anchors.size()),
                                  .origin = origin});
        ++built;
    }
    return {};
}

void TreeAnchors::changed(render::AnchorPool& pool, planet::CellId cell) {
    for (auto held = tiles_.begin(); held != tiles_.end();) {
        const planet::CellId tile = held->first;
        const bool over = tile.level() <= cell.level() ? cell.parent(tile.level()) == tile
                                                       : tile.parent(cell.level()) == cell;
        if (over) {
            pool.release(held->second.slot);
            held = tiles_.erase(held);
        } else {
            ++held;
        }
    }
}

std::vector<render::StandDraw> TreeAnchors::draws(WorldPosition eye,
                                                  const GrassBudget& budget) const {
    std::vector<render::StandDraw> stands;
    stands.reserve(tiles_.size());
    for (const auto& [cell, tile] : tiles_) {
        if (tile.count == 0) {
            continue;
        }
        // The tile spans half its own width from its centre, plus the quarter-cell the normal
        // samples lean over.
        const double span = glm::length(tile.origin.metres()) * (std::numbers::pi / 4.0) /
                            static_cast<double>(std::uint32_t{1} << anchor_tile_level);
        const double distance = distance_to(eye, tile.origin, span);
        int detail = 2;
        if (distance < budget.forest_full) {
            detail = 0;
        } else if (distance < budget.forest_mid) {
            detail = 1;
        }
        stands.push_back({.slot = tile.slot,
                          .count = tile.count,
                          .origin = tile.origin,
                          .detail = detail,
                          .distance = distance});
    }
    return stands;
}

} // namespace hog::client

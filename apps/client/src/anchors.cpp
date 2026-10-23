#include <hog/client/anchors.hpp>
#include <hog/client/tiles.hpp>
#include <hog/core/profile.hpp>
#include <hog/flora/stand.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/forest.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace hog::client {

namespace {

// Trees keep off ground this close to sea level, and off slopes steeper than this much up.
constexpr double shore = 1.0;
constexpr double steepest = 0.7;

// Metres a grown tree reaches past its root, at most: its height, as the shader measures from the
// root.
constexpr double crown_spread = 25.0;

// Lattice cells across a tile's side, and across a block's.
constexpr std::uint32_t tile_side = 1U << (render::tree_lattice_level - anchor_tile_level);
constexpr std::uint32_t block_side = 4;
constexpr std::uint32_t blocks_across = tile_side / block_side;
static_assert(blocks_across * blocks_across == tile_blocks);

// The tree a lattice cell stands, if any, sampled from the world at its own spot.
std::optional<render::TreeAnchor> anchor_at(const planet::World& world, std::uint32_t face,
                                            std::uint32_t i, std::uint32_t j, WorldPosition origin,
                                            const BiomePalette& palette,
                                            const flora::Fates& fates) {
    const double radius = world.geology().planet_radius();
    const auto across = static_cast<double>(std::uint32_t{1} << render::tree_lattice_level);
    // The normal leans over the hillside around the spot, half a lattice cell each way: a tree
    // minds the hill it stands on, not the bumps between its roots.
    const double half = 0.5 / across;
    const Vec2 spot = flora::tree_spot(i, j, face);
    const double s = (static_cast<double>(i) + static_cast<double>(spot.x)) / across;
    const double t = (static_cast<double>(j) + static_cast<double>(spot.y)) / across;
    const tiles::Ground ground = tiles::ground_at(world, face, s, t);
    const std::optional<flora::Stood> stood =
        flora::tree_grown(i, j, face, ground.state.biome,
                          flora::fared(flora::habitat_of(ground.state), fates.at(face, s, t)));
    if (!stood || ground.elevation < shore) {
        return std::nullopt;
    }
    const DVec3 root = ground.direction * (radius + ground.elevation);
    const DVec3 normal = tiles::hillside(world, face, s, t, half, ground);
    if (glm::dot(normal, ground.direction) < steepest) {
        return std::nullopt;
    }
    return render::TreeAnchor{
        .position = Vec3{root - origin.metres()},
        .height = stood->height,
        .normal = Vec3{normal},
        .hash = stood->hash,
        .colour = render::packed_foliage(
            flora::foliage_colour(stood->genus,
                                  palette[static_cast<std::size_t>(ground.state.biome)]),
            stood->genus, stood->condition),
        .age = stood->age};
}

} // namespace

TileTrees tile_anchors(const planet::World& world, planet::CellId tile, WorldPosition origin,
                       const BiomePalette& ground, const flora::Fates& fates) {
    TileTrees trees;
    const planet::FaceIJ at = tile.to_face_ij();
    for (std::uint32_t block = 0; block < tile_blocks; ++block) {
        trees.starts.at(block) = static_cast<std::uint32_t>(trees.anchors.size());
        const std::uint32_t block_a = (block / blocks_across) * block_side;
        const std::uint32_t block_b = (block % blocks_across) * block_side;
        for (std::uint32_t a = block_a; a < block_a + block_side; ++a) {
            for (std::uint32_t b = block_b; b < block_b + block_side; ++b) {
                const std::optional<render::TreeAnchor> anchor = anchor_at(
                    world, static_cast<std::uint32_t>(at.face),
                    (static_cast<std::uint32_t>(at.i) * tile_side) + a,
                    (static_cast<std::uint32_t>(at.j) * tile_side) + b, origin, ground, fates);
                if (anchor) {
                    trees.anchors.push_back(*anchor);
                }
            }
        }
        // Each genus a run of its own, for the grown forest to draw by its own vertex count.
        std::stable_sort(
            std::next(trees.anchors.begin(), static_cast<std::ptrdiff_t>(trees.starts.at(block))),
            trees.anchors.end(),
            [](const render::TreeAnchor& left, const render::TreeAnchor& right) {
                return render::genus_of(left.colour) < render::genus_of(right.colour);
            });
    }
    trees.starts.back() = static_cast<std::uint32_t>(trees.anchors.size());
    return trees;
}

Sight sight_of(const render::Camera& camera, double aspect) {
    const double tall = std::tan(camera.vertical_fov * 0.5);
    return {.eye = camera.eye,
            .forward = camera.forward,
            .half_angle = std::atan(tall * std::sqrt(1.0 + (aspect * aspect)))};
}

bool within_sight(const Sight& sight, DVec3 from_eye, double radius) {
    if (sight.half_angle >= std::numbers::pi) {
        return true;
    }
    const double away = glm::length(from_eye);
    if (away <= radius) {
        return true;
    }
    const double off = std::acos(std::clamp(glm::dot(from_eye / away, sight.forward), -1.0, 1.0));
    return off - std::asin(radius / away) < sight.half_angle;
}

std::size_t anchors_in(std::span<const render::StandDraw> draws) noexcept {
    std::size_t count = 0;
    for (const render::StandDraw& draw : draws) {
        count += draw.count;
    }
    return count;
}

TreeAnchors::TreeAnchors(const BiomePalette& palette, jobs::JobSystem* jobs) : jobs_{jobs} {
    std::ranges::copy(palette, needles_.begin());
}

Result<> TreeAnchors::update(render::AnchorPool& pool, std::uint32_t frame,
                             const planet::World& world, WorldPosition eye, double reach,
                             int builds) {
    HOG_ZONE("tree anchors");
    return tiles_.keep(
        pool, jobs_, world, eye, reach, builds,
        [world = &world, needles = needles_, fates = fates_](planet::CellId cell,
                                                             WorldPosition origin) {
            return tile_anchors(*world, cell, origin, needles, *fates);
        },
        [&](TileTrees trees, std::uint32_t slot, WorldPosition origin) -> Result<Tile> {
            if (auto staged = pool.stage(frame, slot, trees.anchors); !staged) {
                return std::unexpected(std::move(staged).error());
            }
            return held_tile(slot, origin, trees);
        });
}

void TreeAnchors::changed(render::AnchorPool& pool, planet::CellId cell) {
    tiles_.drop_over(pool, cell);
}

void TreeAnchors::fare(std::shared_ptr<const flora::Fates> fates,
                       std::span<const planet::CellId> changed) {
    fates_ = std::move(fates);
    tiles_.stale_over(changed);
}

TreeAnchors::Tile TreeAnchors::held_tile(std::uint32_t slot, WorldPosition origin,
                                         const TileTrees& trees) {
    Tile tile{.slot = slot,
              .count = static_cast<std::uint32_t>(trees.anchors.size()),
              .origin = origin,
              .bounds = tiles::bounds_of(std::span<const render::TreeAnchor>{trees.anchors})};
    for (std::uint32_t at = 0; at < tile_blocks; ++at) {
        Block& block = tile.blocks.at(at);
        block.first = trees.starts.at(at);
        block.count = trees.starts.at(at + 1) - block.first;
        if (block.count == 0) {
            continue;
        }
        const auto stood =
            std::span<const render::TreeAnchor>{trees.anchors}.subspan(block.first, block.count);
        const tiles::Bounds bounds = tiles::bounds_of(stood);
        block.centre = bounds.centre;
        block.reach = bounds.radius;
        for (const render::TreeAnchor& anchor : stood) {
            ++block.genera.at(static_cast<std::size_t>(render::genus_of(anchor.colour)));
        }
    }
    return tile;
}

std::vector<render::StandDraw> TreeAnchors::draws(const Sight& sight) const {
    std::vector<render::StandDraw> stands;
    stands.reserve(tiles_.tiles().size());
    for (const auto& [cell, tile] : tiles_.tiles()) {
        if (tile.count == 0) {
            continue;
        }
        // The tile spans half its own width from its centre, out to its corners.
        const double span = glm::length(tile.origin.metres()) * (std::numbers::pi / 4.0) /
                            static_cast<double>(std::uint32_t{1} << anchor_tile_level);
        if (!within_sight(sight, tile.origin + DVec3{tile.bounds.centre} - sight.eye,
                          static_cast<double>(tile.bounds.radius) + crown_spread)) {
            continue;
        }
        stands.push_back({.slot = tile.slot,
                          .first = 0,
                          .count = tile.count,
                          .origin = tile.origin,
                          .detail = 0,
                          .distance = tiles::distance_to(sight.eye, tile.origin, span)});
    }
    return stands;
}

std::vector<render::StandDraw> TreeAnchors::forest_draws(const Sight& sight,
                                                         const GrassBudget& budget) const {
    std::vector<render::StandDraw> stands;
    for (const auto& [cell, tile] : tiles_.tiles()) {
        const DVec3 from_eye = tile.origin - sight.eye;
        for (const Block& block : tile.blocks) {
            if (block.count == 0) {
                continue;
            }
            // Through the air, not along the ground: from high up, no tree is near. A crown's
            // spread past its root is added in, generously.
            const double distance =
                std::max(0.0, glm::length(from_eye + DVec3{block.centre}) -
                                  static_cast<double>(block.reach) - crown_spread);
            if (distance >= budget.forest_far ||
                !within_sight(sight, from_eye + DVec3{block.centre},
                              static_cast<double>(block.reach) + crown_spread)) {
                continue;
            }
            int detail = 2;
            if (distance < budget.forest_full) {
                detail = 0;
            } else if (distance < budget.forest_mid) {
                detail = 1;
            }
            std::uint32_t first = block.first;
            for (std::size_t genus = 0; genus < block.genera.size(); ++genus) {
                const std::uint32_t count = block.genera.at(genus);
                if (count > 0) {
                    stands.push_back({.slot = tile.slot,
                                      .first = first,
                                      .count = count,
                                      .origin = tile.origin,
                                      .detail = detail,
                                      .genus = static_cast<flora::Genus>(genus),
                                      .distance = distance});
                }
                first += count;
            }
        }
    }
    return stands;
}

} // namespace hog::client

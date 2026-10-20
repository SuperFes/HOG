#include <hog/client/bushes.hpp>
#include <hog/client/tiles.hpp>
#include <hog/render/bushes.hpp>
#include <hog/terrain/patch.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <utility>

namespace hog::client {

namespace {

// Bushes keep off ground this close to sea level.
constexpr double shore = 0.3;

// Metres a bush reaches past its root, at most.
constexpr double bush_spread = 4.0;

constexpr std::uint32_t tile_side = 1U << (flora::bush_lattice_level - bush_tile_level);
static_assert(tile_side * tile_side == render::tile_trees);

// The bush a lattice cell grows, if any, sampled from the world at its own spot.
std::optional<render::BushAnchor> anchor_at(const planet::World& world, std::uint32_t face,
                                            std::uint32_t i, std::uint32_t j, WorldPosition origin,
                                            const BiomePalette& palette,
                                            const flora::Fates& fates) {
    const double radius = world.geology().planet_radius();
    const auto across = static_cast<double>(std::uint32_t{1} << flora::bush_lattice_level);
    const Vec2 spot = flora::bush_spot(i, j, face);
    const double s = (static_cast<double>(i) + static_cast<double>(spot.x)) / across;
    const double t = (static_cast<double>(j) + static_cast<double>(spot.y)) / across;
    const tiles::Ground ground = tiles::ground_at(world, face, s, t);
    if (ground.elevation < shore) {
        return std::nullopt;
    }
    // The biome it grows as, looked for a little way off, as the ground's colours fade into each
    // other there.
    const Vec2 stray = flora::bush_stray(i, j, face);
    const double reach = terrain::biome_blend_reach / tiles::span_of(radius, 0); // in s and t
    const planet::Biome biome =
        tiles::ground_at(world, face, s + (static_cast<double>(stray.x) * reach),
                         t + (static_cast<double>(stray.y) * reach))
            .state.biome;
    const std::optional<flora::Shrub> bush =
        flora::bush_grown(i, j, face, biome, fates.at(face, s, t));
    if (!bush) {
        return std::nullopt;
    }
    const DVec3 root = ground.direction * (radius + ground.elevation);
    return render::BushAnchor{
        .position = Vec3{root - origin.metres()},
        .height = bush->height,
        .normal = Vec3{tiles::hillside(world, face, s, t, 0.5 / across, ground)},
        .hash = bush->hash,
        .colour = render::packed_leaves(
            render::leaf_colour(palette[static_cast<std::size_t>(biome)]), bush->kind),
        .width = bush->width};
}

} // namespace

std::vector<render::BushAnchor> tile_bushes(const planet::World& world, planet::CellId tile,
                                            WorldPosition origin, const BiomePalette& ground,
                                            const flora::Fates& fates) {
    std::vector<render::BushAnchor> bushes;
    const planet::FaceIJ at = tile.to_face_ij();
    for (std::uint32_t a = 0; a < tile_side; ++a) {
        for (std::uint32_t b = 0; b < tile_side; ++b) {
            const std::optional<render::BushAnchor> bush = anchor_at(
                world, static_cast<std::uint32_t>(at.face),
                (static_cast<std::uint32_t>(at.i) * tile_side) + a,
                (static_cast<std::uint32_t>(at.j) * tile_side) + b, origin, ground, fates);
            if (bush) {
                bushes.push_back(*bush);
            }
        }
    }
    return bushes;
}

BushAnchors::BushAnchors(const BiomePalette& palette, jobs::JobSystem* jobs)
    : leaves_{palette}, jobs_{jobs} {}

Result<> BushAnchors::update(render::AnchorPool& pool, std::uint32_t frame,
                             const planet::World& world, WorldPosition eye, double reach,
                             int builds) {
    return tiles_.keep(
        pool, jobs_, world, eye, reach, builds,
        [world = &world, leaves = leaves_, fates = fates_](planet::CellId cell,
                                                           WorldPosition origin) {
            return tile_bushes(*world, cell, origin, leaves, *fates);
        },
        [&](const std::vector<render::BushAnchor>& bushes, std::uint32_t slot,
            WorldPosition origin) -> Result<Tile> {
            if (auto staged = pool.stage(frame, slot, std::span<const render::BushAnchor>{bushes});
                !staged) {
                return std::unexpected(std::move(staged).error());
            }
            return Tile{
                .slot = slot, .count = static_cast<std::uint32_t>(bushes.size()), .origin = origin};
        });
}

void BushAnchors::changed(render::AnchorPool& pool, planet::CellId cell) {
    tiles_.drop_over(pool, cell);
}

void BushAnchors::fare(std::shared_ptr<const flora::Fates> fates,
                       std::span<const planet::CellId> changed) {
    fates_ = std::move(fates);
    tiles_.stale_over(changed);
}

std::vector<render::StandDraw> BushAnchors::draws(const Sight& sight, double reach) const {
    std::vector<render::StandDraw> stands;
    for (const auto& [cell, tile] : tiles_.tiles()) {
        if (tile.count == 0) {
            continue;
        }
        // The tile spans half its own width from its centre, out to its corners.
        const double span = glm::length(tile.origin.metres()) * (std::numbers::pi / 4.0) /
                            static_cast<double>(std::uint32_t{1} << bush_tile_level);
        const DVec3 from_eye = tile.origin - sight.eye;
        // Through the air, as each bush shrinks away by its own distance.
        const double distance =
            std::max(0.0, glm::length(from_eye) - (span * std::numbers::sqrt2) - bush_spread);
        if (distance >= reach ||
            !within_sight(sight, from_eye, (span * std::numbers::sqrt2) + bush_spread)) {
            continue;
        }
        stands.push_back({.slot = tile.slot,
                          .first = 0,
                          .count = tile.count,
                          .origin = tile.origin,
                          .distance = distance});
    }
    return stands;
}

} // namespace hog::client

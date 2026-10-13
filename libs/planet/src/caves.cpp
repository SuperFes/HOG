#include <hog/core/hash.hpp>
#include <hog/planet/caves.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <algorithm>
#include <utility>

namespace hog::planet {

namespace {

constexpr std::uint64_t karst_salt = 0x6B61'7273'7400'0000ULL; // "karst"

// Metres between karst junctions, and across the cells whose ground a passage's ends are set in.
constexpr double karst_spacing = 400.0;
constexpr double point_spacing = 1.0;
// Limestone dissolves into caves where it is wet and thick enough, leaving rock above and below.
constexpr std::int64_t karst_moisture = fixed_one * 3 / 10;
constexpr std::int64_t thinnest_karst = 30 * fixed_one;
constexpr std::int64_t karst_margin = 3 * fixed_one;
// Junctions form around the water table, mostly a little above it.
constexpr std::int64_t above_water_table = 20;
constexpr std::int64_t below_water_table = 5;
constexpr std::uint64_t link_percent = 60;
// How far a passage's middle strays from the straight line, as a share of its length, and up or
// down in metres.
constexpr double karst_wander = 0.25;
constexpr double karst_rise = 5.0;
// Metres of passage radius. Most are crawls and squeezes, a few roomy enough to walk.
constexpr double narrowest_karst = 0.15;
constexpr double widest_karst = 6.0;
// Shallow junctions sometimes break through to the surface as a funnel.
constexpr std::int64_t deepest_sinkhole = 40 * fixed_one;
constexpr std::uint64_t sinkhole_percent = 15;
constexpr double narrowest_sinkhole = 5.0;
constexpr double widest_sinkhole = 15.0;
constexpr std::int64_t sinkhole_lip = 2 * fixed_one;

// A hash mapped to [0, 1).
[[nodiscard]] double fraction(std::uint64_t hash) {
    return static_cast<double>(hash >> 11U) * 0x1p-53;
}

// Mostly near `narrowest`, now and then up to `widest`.
[[nodiscard]] double skewed(double narrowest, double widest, std::uint64_t hash) {
    const double share = fraction(hash);
    return narrowest + ((widest - narrowest) * share * share * share * share);
}

[[nodiscard]] DVec3 point_over(DVec3 direction, double planet_radius, std::int64_t elevation,
                               std::int64_t depth) {
    return direction * (planet_radius + fixed_to_double(elevation - depth));
}

} // namespace

std::string_view to_string(Cave cave) noexcept {
    switch (cave) {
    case Cave::Karst:
        return "karst";
    case Cave::Sinkhole:
        return "sinkhole";
    case Cave::LavaTube:
        return "lava tube";
    case Cave::SeaCave:
        return "sea cave";
    }
    return "unknown";
}

std::optional<double> clearance_at(const Passage& passage, DVec3 point) noexcept {
    const DVec3 along = passage.to - passage.from;
    const double length = glm::dot(along, along);
    const double t =
        length > 0.0 ? std::clamp(glm::dot(point - passage.from, along) / length, 0.0, 1.0) : 0.0;
    const double radius = passage.from_radius + ((passage.to_radius - passage.from_radius) * t);
    if (glm::length(point - (passage.from + (along * t))) < radius) {
        return radius;
    }
    return std::nullopt;
}

Caves::Caves(const Geology& geology)
    : geology_{&geology}, planet_radius_{geology.planet_radius()},
      karst_level_{level_for(karst_spacing, geology.planet_radius())},
      point_level_{level_for(point_spacing, geology.planet_radius())},
      karst_seed_{hash_combine(geology.seed(), karst_salt)} {}

DVec3 Caves::point_at(CellId cell, std::int64_t depth) const {
    return point_over(cell_center(cell), planet_radius_, geology_->terrain().elevation(cell),
                      depth);
}

std::optional<CaveHit> Caves::cave_at(CellId cell, std::int64_t depth) const {
    Lookup lookup{*this};
    return lookup.cave_at(cell, depth);
}

std::vector<Passage> Caves::passages_near(CellId cell) const {
    Lookup lookup{*this};
    return lookup.passages_near(cell);
}

const Caves::Lookup::Junction& Caves::Lookup::junction(CellId tile) {
    if (const auto found = junctions_.find(tile.raw()); found != junctions_.end()) {
        return found->second;
    }
    const Caves& caves = *caves_;
    const Geology& geology = *caves.geology_;
    const std::uint64_t seed = hash_values(caves.karst_seed_, tile.raw());
    // Somewhere in the middle of the tile.
    const TangentFrame frame = tangent_frame(tile);
    const double reach = 0.8 * karst_spacing / caves.planet_radius_;
    const DVec3 direction =
        glm::normalize(frame.up + (frame.east * ((fraction(hash_values(seed, 0)) - 0.5) * reach)) +
                       (frame.north * ((fraction(hash_values(seed, 1)) - 0.5) * reach)));
    const CellId cell = cell_at(direction, caves.point_level_);

    Junction made;
    const std::int64_t elevation = geology.terrain().elevation(cell);
    const Strata strata = geology.strata(cell);
    if (elevation > 0 && strata.sediment == Rock::Limestone &&
        strata.sediment_thickness >= thinnest_karst &&
        geology.climate().moisture(cell) >= karst_moisture) {
        const std::int64_t wander =
            static_cast<std::int64_t>(
                hash_values(seed, 2) %
                static_cast<std::uint64_t>((above_water_table + below_water_table + 1) *
                                           fixed_one)) -
            (above_water_table * fixed_one);
        made.open = true;
        made.depth = std::clamp(geology.climate().water_table(cell) + wander,
                                strata.cover_thickness + karst_margin,
                                strata.bedrock_depth() - karst_margin);
        made.radius = skewed(narrowest_karst, widest_karst, hash_values(seed, 3));
        made.point = point_over(direction, caves.planet_radius_, elevation, made.depth);
    }
    return junctions_.emplace(tile.raw(), made).first->second;
}

const std::vector<Passage>& Caves::Lookup::karst(CellId tile) {
    if (const auto found = karst_.find(tile.raw()); found != karst_.end()) {
        return found->second;
    }
    const Caves& caves = *caves_;
    std::vector<Passage> passages;
    const Junction here = junction(tile);
    if (here.open) {
        for (const CellId neighbor : edge_neighbors(tile)) {
            const Junction there = junction(neighbor);
            // Both tiles make the same passage between them, from the lower-numbered one.
            const auto [low, high] = std::minmax(tile.raw(), neighbor.raw());
            const std::uint64_t seed = hash_values(caves.karst_seed_, low, high);
            if (!there.open || hash_values(seed, 0) % 100 >= link_percent) {
                continue;
            }
            const Junction& from = low == tile.raw() ? here : there;
            const Junction& to = low == tile.raw() ? there : here;
            const DVec3 along = to.point - from.point;
            const TangentFrame frame = tangent_frame(from.point + to.point);
            const DVec3 across = glm::normalize(glm::cross(frame.up, along));
            const DVec3 middle =
                ((from.point + to.point) * 0.5) +
                (across * ((fraction(hash_values(seed, 1)) - 0.5) * 2.0 * karst_wander *
                           glm::length(along))) +
                (frame.up * ((fraction(hash_values(seed, 2)) - 0.5) * 2.0 * karst_rise));
            const double radius = skewed(narrowest_karst, widest_karst, hash_values(seed, 3));
            passages.push_back({.from = from.point,
                                .to = middle,
                                .from_radius = from.radius,
                                .to_radius = radius,
                                .cave = Cave::Karst});
            passages.push_back({.from = middle,
                                .to = to.point,
                                .from_radius = radius,
                                .to_radius = to.radius,
                                .cave = Cave::Karst});
        }
        const std::uint64_t seed = hash_values(caves.karst_seed_, tile.raw(), 1);
        if (here.depth < deepest_sinkhole && hash_values(seed, 0) % 100 < sinkhole_percent) {
            const DVec3 up = glm::normalize(here.point);
            const double lip = fixed_to_double(here.depth + sinkhole_lip);
            passages.push_back(
                {.from = here.point,
                 .to = here.point + (up * lip),
                 .from_radius = here.radius,
                 .to_radius = narrowest_sinkhole + ((widest_sinkhole - narrowest_sinkhole) *
                                                    fraction(hash_values(seed, 1))),
                 .cave = Cave::Sinkhole});
        }
    }
    return karst_.emplace(tile.raw(), std::move(passages)).first->second;
}

std::vector<Passage> Caves::Lookup::passages_near(CellId cell) {
    const CellId tile = cell_at(cell_center(cell), caves_->karst_level_);
    std::vector<Passage> near = karst(tile);
    const VertexNeighbors around_tile = vertex_neighbors(tile);
    for (const CellId around : around_tile.cells()) {
        const std::vector<Passage>& theirs = karst(around);
        near.insert(near.end(), theirs.begin(), theirs.end());
    }
    return near;
}

std::optional<CaveHit> Caves::Lookup::cave_at(CellId cell, std::int64_t depth) {
    const Strata strata = caves_->geology_->strata(cell);
    // Only limestone dissolves, and sinkholes open through what lies over it.
    if (strata.sediment != Rock::Limestone || depth >= strata.bedrock_depth()) {
        return std::nullopt;
    }
    const bool in_limestone = strata.rock_at(depth) == Rock::Limestone;
    const DVec3 point = caves_->point_at(cell, depth);
    std::optional<CaveHit> best;
    for (const Passage& passage : passages_near(cell)) {
        if (passage.cave == Cave::Karst && !in_limestone) {
            continue;
        }
        const std::optional<double> clearance = clearance_at(passage, point);
        if (clearance && (!best || *clearance > best->clearance)) {
            best = CaveHit{.cave = passage.cave, .clearance = *clearance};
        }
    }
    return best;
}

} // namespace hog::planet

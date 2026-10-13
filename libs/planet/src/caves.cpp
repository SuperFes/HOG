#include <hog/core/hash.hpp>
#include <hog/planet/caves.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace hog::planet {

namespace {

constexpr std::uint64_t karst_salt = 0x6B61'7273'7400'0000ULL; // "karst"
constexpr std::uint64_t lava_salt = 0x6C61'7661'0000'0000ULL;  // "lava"
constexpr std::uint64_t sea_salt = 0x7365'6163'6176'6500ULL;   // "seacave"

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

// Each vent feeds a few tubes, each running downhill in steps until it reaches the sea or the
// ground stops falling, a few metres under the surface. Most are roomy, now and then pinched
// almost shut, and they close off at their far ends.
constexpr std::uint64_t fewest_tubes = 2;
constexpr std::uint64_t most_tubes = 4;
constexpr double lava_step = 100.0;
constexpr int longest_tube = 60;
// How far to either side a tube looks for the way down, against one step ahead, and up to how
// many metres of rise it shrugs off in choosing, so tubes from one vent part ways.
constexpr double lava_turn = 0.8;
constexpr std::int64_t lava_wobble = 3 * fixed_one;
constexpr std::int64_t shallowest_tube = 3;
constexpr std::int64_t deepest_tube = 15;
constexpr double narrowest_tube = 1.0;
constexpr double widest_tube = 6.0;
constexpr std::uint64_t pinch_percent = 15;
constexpr double narrowest_pinch = 0.2;
constexpr double widest_pinch = 0.5;
constexpr double tube_end = 0.3;
// Lava tubes lie no deeper than this, which spares deeper queries the search.
constexpr std::int64_t below_tubes = 40 * fixed_one;

// The sea hollows out caves where it beats on steep coasts: ground at least `cliff_height` up
// within `cliff_reach` of open water. A cave runs inland from its mouth at about sea level, wide at
// the mouth and narrowing to a crack.
constexpr double sea_spacing = 200.0;
constexpr std::int64_t cliff_height = 8 * fixed_one;
constexpr double cliff_reach = 80.0;
constexpr std::uint64_t sea_cave_percent = 40;
constexpr double shortest_sea_cave = 20.0;
constexpr double longest_sea_cave = 60.0;
constexpr double narrowest_mouth = 3.0;
constexpr double widest_mouth = 8.0;
constexpr double narrowest_crack = 0.2;
constexpr double widest_crack = 0.35;
constexpr double mouth_height = 1.0; // metres above the sea
// Eight ways around, as east and north.
constexpr std::array compass_east{1.0, 1.0, 0.0, -1.0, -1.0, -1.0, 0.0, 1.0};
constexpr std::array compass_north{0.0, 1.0, 1.0, 1.0, 0.0, -1.0, -1.0, -1.0};
// Metres above and below the sea that sea caves stay within, which spares other queries.
constexpr double above_sea_caves = mouth_height + widest_mouth;
constexpr double below_sea_caves = widest_mouth;

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
      sea_level_{level_for(sea_spacing, geology.planet_radius())},
      karst_seed_{hash_combine(geology.seed(), karst_salt)},
      sea_seed_{hash_combine(geology.seed(), sea_salt)} {
    const std::uint64_t seed = hash_combine(geology.seed(), lava_salt);
    const auto chambers = geology.chambers();
    for (std::size_t chamber = 0; chamber < chambers.size(); ++chamber) {
        const std::uint64_t count =
            fewest_tubes + (hash_values(seed, chamber) % (most_tubes - fewest_tubes + 1));
        for (std::uint64_t tube = 0; tube < count; ++tube) {
            LavaTube made = lava_tube(chambers[chamber].center, hash_values(seed, chamber, tube));
            if (!made.passages.empty()) {
                made.chamber = chamber;
                lava_tubes_.push_back(std::move(made));
            }
        }
    }
}

LavaTube Caves::lava_tube(DVec3 vent, std::uint64_t seed) const {
    const auto ground = [&](DVec3 direction) {
        return geology_->terrain().elevation(cell_at(direction, point_level_));
    };
    struct Node {
        DVec3 direction;
        std::int64_t elevation;
    };
    std::vector<Node> nodes{{.direction = vent, .elevation = ground(vent)}};
    const TangentFrame frame = tangent_frame(vent);
    DVec3 heading = glm::normalize((frame.east * ((2.0 * fraction(hash_values(seed, 0))) - 1.0)) +
                                   (frame.north * ((2.0 * fraction(hash_values(seed, 1))) - 1.0)));
    for (int step = 0; step < longest_tube && nodes.back().elevation > 0; ++step) {
        const Node& here = nodes.back();
        const DVec3 side = glm::normalize(glm::cross(here.direction, heading));
        // The lowest way down, give or take the wobble.
        std::optional<Node> lowest;
        std::int64_t lowest_choice = 0;
        DVec3 lowest_heading{};
        std::uint64_t way_index = 0;
        for (const double turn : {0.0, -lava_turn, lava_turn}) {
            const DVec3 way = glm::normalize(heading + (side * turn));
            const DVec3 next =
                glm::normalize(here.direction + (way * (lava_step / planet_radius_)));
            const std::int64_t elevation = ground(next);
            const std::int64_t choice =
                elevation + static_cast<std::int64_t>(hash_values(seed, 3, step, way_index++) %
                                                      static_cast<std::uint64_t>(lava_wobble + 1));
            if (elevation < here.elevation && (!lowest || choice < lowest_choice)) {
                lowest = Node{.direction = next, .elevation = elevation};
                lowest_choice = choice;
                lowest_heading = way;
            }
        }
        if (!lowest) {
            break;
        }
        heading = glm::normalize(lowest_heading -
                                 (lowest->direction * glm::dot(lowest_heading, lowest->direction)));
        nodes.push_back(*lowest);
    }

    LavaTube tube;
    if (nodes.size() < 2) {
        return tube;
    }
    std::vector<DVec3> points;
    std::vector<double> radii;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const std::uint64_t node = hash_values(seed, 2, index);
        const auto depth =
            (shallowest_tube * fixed_one) +
            static_cast<std::int64_t>(
                hash_values(node, 0) %
                static_cast<std::uint64_t>(((deepest_tube - shallowest_tube) * fixed_one) + 1));
        points.push_back(
            point_over(nodes[index].direction, planet_radius_, nodes[index].elevation, depth));
        if (index + 1 == nodes.size()) {
            radii.push_back(tube_end);
        } else if (hash_values(node, 1) % 100 < pinch_percent) {
            radii.push_back(narrowest_pinch +
                            ((widest_pinch - narrowest_pinch) * fraction(hash_values(node, 2))));
        } else {
            radii.push_back(narrowest_tube +
                            ((widest_tube - narrowest_tube) * fraction(hash_values(node, 2))));
        }
    }
    DVec3 middle{0.0};
    for (const DVec3 point : points) {
        middle += point / static_cast<double>(points.size());
    }
    for (std::size_t index = 0; index + 1 < points.size(); ++index) {
        tube.passages.push_back({.from = points[index],
                                 .to = points[index + 1],
                                 .from_radius = radii[index],
                                 .to_radius = radii[index + 1],
                                 .cave = Cave::LavaTube});
        tube.reach = std::max(tube.reach, glm::length(points[index] - middle) + widest_tube);
    }
    tube.reach = std::max(tube.reach, glm::length(points.back() - middle) + widest_tube);
    tube.middle = middle;
    return tube;
}

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

const std::vector<Passage>& Caves::Lookup::sea(CellId tile) {
    if (const auto found = sea_.find(tile.raw()); found != sea_.end()) {
        return found->second;
    }
    const Caves& caves = *caves_;
    const Elevation& terrain = caves.geology_->terrain();
    const std::uint64_t seed = hash_values(caves.sea_seed_, tile.raw());
    std::vector<Passage> passages;
    if (hash_values(seed, 0) % 100 < sea_cave_percent) {
        const TangentFrame frame = tangent_frame(tile);
        const double reach = 0.8 * sea_spacing / caves.planet_radius_;
        const DVec3 top = glm::normalize(
            frame.up + (frame.east * ((fraction(hash_values(seed, 1)) - 0.5) * reach)) +
            (frame.north * ((fraction(hash_values(seed, 2)) - 0.5) * reach)));
        const auto ground = [&](DVec3 direction) {
            return terrain.elevation(cell_at(direction, caves.point_level_));
        };
        if (ground(top) >= cliff_height) {
            // The first way, around from a seeded start, that finds the sea.
            const TangentFrame here = tangent_frame(top);
            const std::uint64_t start = hash_values(seed, 3) % 8;
            for (std::uint64_t turn = 0; turn < 8; ++turn) {
                const std::uint64_t way = (start + turn) % 8;
                const DVec3 out = glm::normalize((here.east * compass_east.at(way)) +
                                                 (here.north * compass_north.at(way)));
                const DVec3 sea =
                    glm::normalize(top + (out * (cliff_reach / caves.planet_radius_)));
                if (ground(sea) > 0) {
                    continue;
                }
                const double length = shortest_sea_cave + ((longest_sea_cave - shortest_sea_cave) *
                                                           fraction(hash_values(seed, 4)));
                const DVec3 inland = glm::normalize(top - (out * (length / caves.planet_radius_)));
                const double height = caves.planet_radius_ + mouth_height;
                passages.push_back(
                    {.from = sea * height,
                     .to = inland * height,
                     .from_radius = narrowest_mouth + ((widest_mouth - narrowest_mouth) *
                                                       fraction(hash_values(seed, 5))),
                     .to_radius = narrowest_crack + ((widest_crack - narrowest_crack) *
                                                     fraction(hash_values(seed, 6))),
                     .cave = Cave::SeaCave});
                break;
            }
        }
    }
    return sea_.emplace(tile.raw(), std::move(passages)).first->second;
}

std::vector<Passage> Caves::Lookup::passages_near(CellId cell) {
    const CellId tile = cell_at(cell_center(cell), caves_->karst_level_);
    std::vector<Passage> near = karst(tile);
    const VertexNeighbors around_tile = vertex_neighbors(tile);
    for (const CellId around : around_tile.cells()) {
        const std::vector<Passage>& theirs = karst(around);
        near.insert(near.end(), theirs.begin(), theirs.end());
    }
    const CellId coast = cell_at(cell_center(cell), caves_->sea_level_);
    const std::vector<Passage>& here = sea(coast);
    near.insert(near.end(), here.begin(), here.end());
    const VertexNeighbors around_coast = vertex_neighbors(coast);
    for (const CellId around : around_coast.cells()) {
        const std::vector<Passage>& theirs = sea(around);
        near.insert(near.end(), theirs.begin(), theirs.end());
    }
    const DVec3 point = caves_->point_at(cell, 0);
    for (const LavaTube& tube : caves_->lava_tubes_) {
        if (glm::length(point - tube.middle) <= tube.reach + karst_spacing) {
            near.insert(near.end(), tube.passages.begin(), tube.passages.end());
        }
    }
    return near;
}

std::optional<CaveHit> Caves::Lookup::cave_at(CellId cell, std::int64_t depth) {
    const Strata strata = caves_->geology_->strata(cell);
    const Rock rock = strata.rock_at(depth);
    // Only limestone dissolves, and sinkholes open through what lies over it.
    const bool karst = strata.sediment == Rock::Limestone && depth < strata.bedrock_depth();
    const bool lava = rock == Rock::Basalt && depth < below_tubes;
    // Sea caves are cut in solid rock, about level with the sea.
    const std::int64_t elevation = caves_->geology_->terrain().elevation(cell);
    const double height = fixed_to_double(elevation - depth);
    const bool solid = rock != Rock::Soil && rock != Rock::Sand && depth >= 0;
    const bool sea_cave =
        solid && elevation > 0 && height < above_sea_caves && height > -below_sea_caves;
    if (!karst && !lava && !sea_cave) {
        return std::nullopt;
    }
    const DVec3 point = point_over(cell_center(cell), caves_->planet_radius_, elevation, depth);
    std::optional<CaveHit> best;
    const auto consider = [&](const Passage& passage) {
        const std::optional<double> clearance = clearance_at(passage, point);
        if (clearance && (!best || *clearance > best->clearance)) {
            best = CaveHit{.cave = passage.cave, .clearance = *clearance};
        }
    };
    if (karst) {
        const auto dissolved = [&](CellId tile) {
            for (const Passage& passage : this->karst(tile)) {
                if (passage.cave == Cave::Sinkhole || rock == Rock::Limestone) {
                    consider(passage);
                }
            }
        };
        const CellId tile = cell_at(cell_center(cell), caves_->karst_level_);
        dissolved(tile);
        const VertexNeighbors around = vertex_neighbors(tile);
        std::ranges::for_each(around.cells(), dissolved);
    }
    if (sea_cave) {
        const CellId tile = cell_at(cell_center(cell), caves_->sea_level_);
        std::ranges::for_each(sea(tile), consider);
        const VertexNeighbors around = vertex_neighbors(tile);
        for (const CellId each : around.cells()) {
            std::ranges::for_each(sea(each), consider);
        }
    }
    if (lava) {
        for (const LavaTube& tube : caves_->lava_tubes_) {
            if (glm::length(point - tube.middle) <= tube.reach) {
                std::ranges::for_each(tube.passages, consider);
            }
        }
    }
    return best;
}

} // namespace hog::planet

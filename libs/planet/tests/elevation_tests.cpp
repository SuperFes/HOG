#include <hog/core/hash.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/tile.hpp>
#include <hog/testing/quick.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

using hog::planet::CellId;
using hog::planet::Elevation;
using hog::planet::fine_fraction_bits;
using hog::planet::fixed_one;
using hog::planet::fixed_to_double;
using hog::planet::Rock;

namespace {

hog::planet::PlanetConfig planet(std::uint64_t seed) {
    return {.seed = seed,
            .ocean_fraction = 0.7,
            .radius = 115'000.0,
            .erosion_cell = hog::testing::coarse_erosion_cell};
}

std::vector<CellId> cells_at(int level) {
    std::vector<CellId> cells;
    for (int face = 0; face < hog::planet::face_count; ++face) {
        const auto tile = hog::planet::sample_tile(CellId::from_face(face), level,
                                                   [](CellId cell) { return cell; });
        cells.insert(cells.end(), tile.begin(), tile.end());
    }
    return cells;
}

std::int64_t metres(std::int64_t value) {
    return value * fixed_one;
}

constexpr double radius = 115'000.0;

// Where coasts cross from land to sea: the first `count` level-8 land cells beside the sea, but for
// volcanoes', each with the sea cell beside it.
std::vector<std::pair<hog::DVec3, hog::DVec3>> coasts(const Elevation& terrain, int count) {
    std::vector<std::pair<hog::DVec3, hog::DVec3>> found;
    for (const CellId cell :
         hog::planet::sample_tile(CellId::from_face(4), 8, [](CellId cell) { return cell; })) {
        // Not volcanoes', whose flanks fall steeply into the sea.
        const auto shield = terrain.hotspots().surface(cell);
        if (!terrain.is_land(cell) || (shield && *shield > 0) ||
            terrain.volcanoes().lift(cell) > 0) {
            continue;
        }
        for (const CellId next : hog::planet::edge_neighbors(cell)) {
            if (!terrain.is_land(next)) {
                found.emplace_back(hog::planet::cell_center(cell), hog::planet::cell_center(next));
                break;
            }
        }
        if (std::cmp_equal(found.size(), count)) {
            break;
        }
    }
    return found;
}

// The shore between `land` and `sea`, found to within the finest cells.
hog::DVec3 shore(const Elevation& terrain, hog::DVec3 land, hog::DVec3 sea) {
    for (int halving = 0; halving < 40; ++halving) {
        const hog::DVec3 middle = glm::normalize(land + sea);
        (terrain.is_land(hog::planet::cell_at(middle)) ? land : sea) = middle;
    }
    return land;
}

// `metres` along the ground from `from`, towards `towards`.
hog::DVec3 along(hog::DVec3 from, hog::DVec3 towards, double metres) {
    const hog::DVec3 across = glm::normalize(towards - (from * glm::dot(towards, from)));
    return glm::normalize(from + (across * (metres / radius)));
}

} // namespace

TEST_CASE("land stays above sea level and the ocean floor below it, but for volcanoes and lakes",
          "[planet][elevation]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(2), values({1U, 2U})));
    const Elevation terrain{planet(seed)};
    const auto volcanoes = metres(static_cast<std::int64_t>(hog::planet::Volcanoes::max_height));
    for (const CellId cell : cells_at(6)) {
        const std::int64_t elevation = terrain.elevation(cell);
        if (terrain.height<hog::planet::fine_fraction_bits>(cell) > 0) {
            // Low ground cut off from the sea may lie under it, but only under a lake.
            REQUIRE((elevation >= 0 || terrain.lake(cell)));
            REQUIRE(elevation <=
                    metres(Elevation::lowland_height + Elevation::mountain_height) + volcanoes);
        } else {
            // Lakes build their rims up out of low ground, the sea's too.
            if (!terrain.landform().near_lake(cell)) {
                const auto shields = terrain.hotspots().surface(cell);
                REQUIRE(elevation <= std::max(terrain.volcanoes().lift(cell), shields.value_or(0)));
            }
            REQUIRE(elevation >= metres(-Elevation::deepest));
            REQUIRE(terrain.mountain_strength(cell) == 0);
        }
    }
}

TEST_CASE("mountains rise on part of the land and the ocean is deep", "[planet][elevation]") {
    const std::uint64_t seed = GENERATE(take(hog::testing::cases(4), values({1U, 2U, 3U, 4U})));
    const Elevation terrain{planet(seed)};
    std::size_t land = 0;
    std::size_t high = 0;
    std::int64_t highest = 0;
    std::vector<std::int64_t> depths;
    for (const CellId cell : cells_at(6)) {
        const std::int64_t elevation = terrain.elevation(cell);
        if (terrain.is_land(cell)) {
            ++land;
            if (elevation > metres(1'000)) {
                ++high;
            }
            highest = std::max(highest, elevation);
        } else {
            depths.push_back(-elevation);
        }
    }
    CHECK(high * 100 > land);
    CHECK(high * 4 < land);
    CHECK(highest > metres(2'000));
    std::ranges::nth_element(depths,
                             depths.begin() + static_cast<std::ptrdiff_t>(depths.size() / 2));
    CHECK(depths[depths.size() / 2] > metres(2'500));
}

TEST_CASE("coasts stay low", "[planet][elevation]") {
    const Elevation terrain{planet(4)};
    std::int64_t highest_coast = 0;
    for (const CellId cell :
         hog::planet::sample_tile(CellId::from_face(4), 8, [](CellId cell) { return cell; })) {
        if (!terrain.is_land(cell)) {
            continue;
        }
        const auto neighbours = hog::planet::edge_neighbors(cell);
        if (std::ranges::any_of(neighbours, [&](CellId next) { return !terrain.is_land(next); })) {
            highest_coast = std::max(highest_coast, terrain.elevation(cell));
        }
    }
    CHECK(highest_coast > 0);
    CHECK(highest_coast < metres(500));
}

TEST_CASE("elevation changes smoothly between neighbours", "[planet][elevation]") {
    // Steps shrink with the cells; a discontinuity would not.
    const Elevation terrain{planet(2)};
    constexpr int level = 16;
    constexpr std::uint64_t side = std::uint64_t{1} << level;
    std::int64_t largest_step = 0;
    for (std::uint64_t sample = 0; sample < 10'000; ++sample) {
        const CellId cell = CellId::from_face_ij(
            static_cast<int>(hog::hash_values(sample, 0) % hog::planet::face_count),
            static_cast<std::uint32_t>(hog::hash_values(sample, 1) % side),
            static_cast<std::uint32_t>(hog::hash_values(sample, 2) % side), level);
        for (const CellId next : hog::planet::edge_neighbors(cell)) {
            largest_step =
                std::max(largest_step, std::abs(terrain.elevation(cell) - terrain.elevation(next)));
        }
    }
    // Cells here are about 3 m across.
    CHECK(largest_step < metres(10));
}

TEST_CASE("land is where the ground stands above the sea, to the finest cell",
          "[planet][elevation]") {
    const Elevation terrain{planet(4)};
    for (const auto& [land, sea] : coasts(terrain, 8)) {
        const hog::DVec3 at = shore(terrain, land, sea);
        for (int centimetre = -300; centimetre <= 300; ++centimetre) {
            const CellId cell = hog::planet::cell_at(along(at, sea, centimetre / 100.0));
            REQUIRE(terrain.is_land(cell) == (terrain.elevation(cell) > 0));
        }
    }
}

TEST_CASE("a sandy shore's floor follows Dean's profile out to a shelf", "[planet][elevation]") {
    // Read as metres off the shore, a metre to each hundred-thousandth of continent height.
    const auto floor = [](double out) {
        const std::int64_t below = std::llround(
            out * 1e-5 * static_cast<double>(hog::planet::fixed_unit<fine_fraction_bits>));
        return fixed_to_double<fine_fraction_bits>(Elevation::sea_floor<fine_fraction_bits>(below));
    };
    for (const double out : {1.0, 10.0, 100.0, 500.0, 1'000.0, 1'500.0}) {
        CAPTURE(out);
        CHECK_THAT(floor(out), Catch::Matchers::WithinAbs(-0.12 * std::cbrt(out * out), 1e-3));
    }
    for (const double out : {2'000.0, 3'000.0, 4'990.0}) {
        CAPTURE(out);
        CHECK_THAT(floor(out), Catch::Matchers::WithinAbs(-out / 100.0, 1e-3));
    }
    CHECK(floor(30'000.0) < -3'000.0);
    CHECK(floor(1e6) == -static_cast<double>(Elevation::ocean_depth));

    // Down without a rise or a step anywhere from the shore to the ocean floor.
    std::int64_t previous = 0;
    for (std::int64_t below = 0; below < hog::planet::fixed_unit<fine_fraction_bits> / 4; ++below) {
        const std::int64_t next = Elevation::sea_floor<fine_fraction_bits>(below);
        REQUIRE(next <= previous);
        REQUIRE(previous - next < hog::planet::fixed_unit<fine_fraction_bits> / 100);
        previous = next;
    }
}

TEST_CASE("however much is built up under it, the ocean floor stays as deep as the shelf's edge",
          "[planet][elevation]") {
    constexpr std::int64_t one = hog::planet::fixed_unit<fine_fraction_bits>;
    const std::int64_t below = one / 2;
    CHECK(Elevation::sea_floor<fine_fraction_bits>(below, 2'000 * one) == -2'000 * one);
    CHECK(Elevation::sea_floor<fine_fraction_bits>(below, 50 * one) == -50 * one);
    CHECK(Elevation::sea_floor<fine_fraction_bits>(below, -1'000 * one) == -50 * one);
}

TEST_CASE("the sea stays shallow off a coast", "[planet][elevation]") {
    const std::uint64_t seed = GENERATE(1U, 4U);
    const Elevation terrain{planet(seed)};
    int open = 0;
    for (const auto& [land, sea] : coasts(terrain, 12)) {
        const hog::DVec3 at = shore(terrain, land, sea);
        const auto depth = [&](double out) {
            return -fixed_to_double(terrain.elevation(hog::planet::cell_at(along(at, sea, out))));
        };
        // Measured off coasts facing open water, not across an inlet to land again.
        if (depth(100.0) <= 0.0 || depth(1'000.0) <= 0.0) {
            continue;
        }
        ++open;
        CHECK(depth(100.0) > 0.5);
        CHECK(depth(100.0) < 8.0);
        CHECK(depth(1'000.0) > 3.0);
        CHECK(depth(1'000.0) < 40.0);
    }
    CHECK(open >= 6);
}

TEST_CASE("the sea floor deepens off a coast without steps", "[planet][elevation]") {
    // Generated with too few bits, the floor held for over a metre and then dropped by 0.4 m. A
    // steep shore falls fast right at the water, but never that fast.
    const Elevation terrain{planet(4)};
    for (const auto& [land, sea] : coasts(terrain, 8)) {
        const hog::DVec3 at = shore(terrain, land, sea);
        std::int64_t previous = terrain.elevation(hog::planet::cell_at(at));
        for (int step = 1; step <= 400; ++step) {
            const std::int64_t floor =
                terrain.elevation(hog::planet::cell_at(along(at, sea, step / 20.0)));
            REQUIRE(std::abs(floor - previous) < fixed_one / 5);
            previous = floor;
        }
    }
}

TEST_CASE("elevation depends on the seed", "[planet][elevation]") {
    const Elevation first{planet(1)};
    const Elevation again{planet(1)};
    const Elevation other{planet(2)};
    int differ = 0;
    for (const CellId cell : cells_at(4)) {
        REQUIRE(again.elevation(cell) == first.elevation(cell));
        differ += other.elevation(cell) != first.elevation(cell) ? 1 : 0;
    }
    CHECK(differ > static_cast<int>(cells_at(4).size()) / 2);
}

TEST_CASE("ground stands above the water it drains to", "[planet][elevation]") {
    const Elevation terrain{planet(1)};
    int land = 0;
    for (const CellId cell : cells_at(7)) {
        const std::int64_t above = terrain.above_water(cell);
        REQUIRE(above >= 0);
        if (terrain.elevation(cell) <= 0) {
            REQUIRE(above == 0);
        } else {
            // Rivers and the sea lie at or above sea level.
            REQUIRE(above <= terrain.elevation(cell));
            ++land;
        }
    }
    CHECK(land > 0);
}

TEST_CASE("the nearest land is land, with none nearer", "[planet][elevation]") {
    const Elevation terrain{planet(1)};
    const auto& landform = terrain.landform();
    const auto& grid = landform.grid();
    const std::int64_t above = metres(50);
    for (const hog::DVec3 from : {hog::planet::start_direction, hog::planet::north_pole,
                                  hog::planet::direction_at(-0.6, 2.1)}) {
        const hog::DVec3 found = terrain.nearest_land(from, above);
        const std::uint32_t there = grid.index(hog::planet::cell_at(found, grid.level()));
        REQUIRE(landform.heights()[there] > above);
        CHECK(terrain.is_land(hog::planet::cell_at(found)));
        // Nothing on the way there stands so high.
        const double angle = std::acos(std::clamp(glm::dot(found, from), -1.0, 1.0));
        for (int step = 1; step < 200; ++step) {
            const std::uint32_t index = grid.index(hog::planet::cell_at(
                along(from, found, angle * radius * step / 200.0), grid.level()));
            CHECK((index == there || landform.heights()[index] <= above));
        }
    }
    // From land, the land itself.
    const hog::DVec3 land = terrain.nearest_land(hog::planet::start_direction, above);
    CHECK(terrain.nearest_land(land, above) == land);
}

TEST_CASE("only ground joined to the sea lies under it", "[planet][elevation]") {
    // This planet has a stretch below sea level cut off from the sea, thousands of km² across.
    const Elevation terrain{planet(9)};
    const auto& landform = terrain.landform();
    const auto& drainage = landform.drainage();
    // Some 200,000 cells, however fine the landform.
    const std::uint32_t stride = std::max(landform.grid().size() / 200'000, 1U);
    double cut_off = 0.0; // km²
    for (std::uint32_t index = 0; index < landform.grid().size(); index += stride) {
        const CellId cell = landform.grid().cell(index);
        if (landform.heights()[index] > 0 || terrain.elevation(cell) >= 0) {
            continue;
        }
        const bool sea = drainage.receiver[index] == index;
        REQUIRE(terrain.is_land(cell) != sea);
        cut_off += sea ? 0.0 : static_cast<double>(landform.grid().area(index) * stride) / 1e6;
    }
    CHECK(cut_off > 1'000.0);
}

TEST_CASE("the deep ocean floor rises at ridges, sinks into trenches and rolls between",
          "[planet][elevation]") {
    const Elevation terrain{planet(1)};
    constexpr auto depth = static_cast<double>(Elevation::ocean_depth);
    constexpr double hills = Elevation::abyssal_hill_height / 2.0;
    int ridges = 0;
    int trenches = 0;
    double shallowest_plain = depth * 2.0;
    double deepest_plain = 0.0;
    for (const CellId cell : cells_at(6)) {
        if (terrain.height(cell) > -fixed_one / 5 || terrain.volcanoes().lift(cell) > 0 ||
            terrain.hotspots().surface(cell) || terrain.hotspots().swell(cell) > 0) {
            continue;
        }
        const double below = -fixed_to_double(terrain.elevation(cell));
        const double floor = fixed_to_double(terrain.continents().crust(cell).floor);
        REQUIRE(std::abs(below - (depth - floor)) <= hills + 0.01);
        if (floor > 500.0) {
            ++ridges;
        } else if (floor < -500.0) {
            ++trenches;
        } else if (std::abs(floor) < 1.0) {
            shallowest_plain = std::min(shallowest_plain, below);
            deepest_plain = std::max(deepest_plain, below);
        }
    }
    CHECK(ridges > 10);
    CHECK(trenches > 10);
    CHECK(shallowest_plain >= depth - hills);
    CHECK(deepest_plain <= depth + hills);
    CHECK(deepest_plain - shallowest_plain > 100.0);
}

TEST_CASE("the youngest shields over plumes rise as islands", "[planet][elevation]") {
    const Elevation terrain{planet(1)};
    REQUIRE_FALSE(terrain.hotspots().hotspots().empty());
    for (std::size_t plume = 0; plume < terrain.hotspots().hotspots().size(); ++plume) {
        const auto& young =
            terrain.hotspots().shields()[plume * hog::planet::Hotspots::chain_length];
        const CellId summit = hog::planet::cell_at(young.center);
        CHECK(terrain.is_land(summit));
        CHECK(terrain.elevation(summit) > metres(500));
        const auto& old =
            terrain.hotspots().shields()[((plume + 1) * hog::planet::Hotspots::chain_length) - 1];
        // Unless carried onto land.
        if (terrain.height(hog::planet::cell_at(old.center)) < 0) {
            CHECK_FALSE(terrain.is_land(hog::planet::cell_at(old.center)));
        }
    }
}

namespace {

// How far a lake's water at `level` over `cell` stands above whatever is beside it at `next`, the
// ground or another lake, where no rim between holds it in; 0 where it's held or it runs out.
std::int64_t wall(const Elevation& terrain, CellId cell, CellId next, std::int64_t level) {
    // A river beside a lake is its water running out, falling from the lake's level.
    if (terrain.river(next)) {
        return 0;
    }
    const auto beside = terrain.lake(next);
    const std::int64_t drop =
        level - (beside ? beside->surface : std::max<std::int64_t>(terrain.elevation(next), 0));
    if (drop <= fixed_one / 100) {
        return 0;
    }
    // As are the banks of the notch it cuts through the rim, as far out as they fall below the
    // lake from the river's water.
    if (const auto* reach = terrain.rivers().near(next, 200.0)) {
        const std::int64_t river = std::min(reach->from_surface, reach->to_surface);
        if (terrain.rivers().near(next, fixed_to_double(level - river) /
                                            hog::planet::Rivers::bank_slope) != nullptr) {
            return 0;
        }
    }
    // Lower ground beyond a rim is fine, as long as the water reaches no further.
    const hog::DVec3 from = hog::planet::cell_center(cell);
    const hog::DVec3 to = hog::planet::cell_center(next);
    for (int step = 1; step < 32; ++step) {
        const double t = step / 32.0;
        if (terrain.elevation(hog::planet::cell_at(
                glm::normalize((from * (1.0 - t)) + (to * t)))) >= level - (fixed_one / 100)) {
            return 0;
        }
    }
    return drop;
}

} // namespace

TEST_CASE("the ground holds lakes in, the water never standing over lower ground beside it",
          "[planet][elevation]") {
    // Worn as finely as a real world, where the shores' finest detail lies.
    const Elevation terrain{{.seed = 1, .ocean_fraction = 0.7, .radius = 115'000.0}};
    const auto& landform = terrain.landform();
    const auto& grid = landform.grid();
    const auto lakes = landform.lakes();
    // Landform cells by a shore: a lake's beside dry ground, or dry ground beside a lake.
    std::vector<std::uint32_t> shore;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const bool wet = lakes[index] != hog::planet::Landform::dry;
        if (std::ranges::any_of(grid.neighbors(index), [&](std::uint32_t around) {
                return (lakes[around] != hog::planet::Landform::dry) != wet;
            })) {
            shore.push_back(index);
        }
    }
    REQUIRE(shore.size() > 100);
    int shores = 0;
    int walls = 0;
    std::int64_t tallest = 0;
    // About 2,000 of them, looked over finely.
    const std::size_t stride = std::max<std::size_t>(shore.size() / 2'000, 1);
    for (std::size_t at = 0; at < shore.size(); at += stride) {
        for (const CellId cell :
             hog::planet::sample_tile(grid.cell(shore[at]), 3, [](CellId fine) { return fine; })) {
            const auto lake = terrain.lake(cell);
            for (const CellId next : hog::planet::edge_neighbors(cell)) {
                const auto beside = terrain.lake(next);
                if (!lake || (beside && beside->surface == lake->surface)) {
                    continue;
                }
                ++shores;
                const std::int64_t drop = wall(terrain, cell, next, lake->surface);
                walls += drop > 0 ? 1 : 0;
                tallest = std::max(tallest, drop);
            }
        }
    }
    INFO("walls " << walls << " of " << shores << ", tallest " << fixed_to_double(tallest));
    CHECK(shores > 1'000);
    CHECK(walls == 0);
}

TEST_CASE("the ground never stands in sheer walls between neighbouring points",
          "[planet][elevation]") {
    // Worn as finely as a real world, where valleys cut deepest beside their walls.
    const Elevation terrain{{.seed = 1, .ocean_fraction = 0.7, .radius = 115'000.0}};
    const auto& landform = terrain.landform();
    const auto& grid = landform.grid();
    double steepest = 0.0;
    for (std::uint32_t index = 0; index < grid.size(); index += 97) {
        if (landform.heights()[index] <= 0) {
            continue;
        }
        for (const CellId cell :
             hog::planet::sample_tile(grid.cell(index), 3, [](CellId fine) { return fine; })) {
            const std::int64_t here = terrain.elevation(cell);
            for (const CellId next : hog::planet::edge_neighbors(cell)) {
                const double apart =
                    glm::length(hog::planet::cell_center(cell) - hog::planet::cell_center(next)) *
                    115'000.0;
                steepest = std::max(
                    steepest, std::abs(fixed_to_double(terrain.elevation(next) - here)) / apart);
            }
        }
    }
    // Crags stand steeply, but nothing like a cliff a cell tall.
    CHECK(steepest < 2.5);
}

TEST_CASE("bare beds on steep ground step into ledges", "[planet][elevation]") {
    const Elevation terrain{{.seed = 1, .ocean_fraction = 0.7, .radius = 115'000.0}};
    const auto& landform = terrain.landform();
    const auto& grid = landform.grid();
    // Where in their beds points on steep bare sandstone and limestone lie: ledges gather them
    // on the benches at the beds' ends, away from the risers between.
    int points = 0;
    int midway = 0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        if (landform.heights()[index] <= 100 * fixed_one ||
            landform.steepness()[index] < fixed_one * 7 / 10 ||
            landform.soil()[index] > fixed_one / 5) {
            continue;
        }
        for (const CellId cell :
             hog::planet::sample_tile(grid.cell(index), 3, [](CellId fine) { return fine; })) {
            const std::int64_t crust = terrain.continents().height(cell);
            if (crust < fixed_one / 20) {
                continue;
            }
            const auto column = terrain.beds().column(
                cell, {.continental = true, .mountains = terrain.mountain_strength(cell)});
            const std::int64_t frame =
                terrain.elevation(cell) - grid.smooth(landform.risen(), cell);
            const auto bed = terrain.beds().bed_at(column, frame);
            if (!bed || (bed->rock != Rock::Sandstone && bed->rock != Rock::Limestone)) {
                continue;
            }
            const double along =
                static_cast<double>(frame - bed->base) / static_cast<double>(bed->top - bed->base);
            ++points;
            midway += along > 0.25 && along < 0.75 ? 1 : 0;
        }
    }
    INFO(midway << " of " << points << " midway up their beds");
    REQUIRE(points > 200);
    // Evenly spread, half would be.
    CHECK(midway < points * 2 / 5);
}

TEST_CASE("steep or thin-soiled ground shows its rock, deep soil hides it", "[planet][elevation]") {
    const Elevation terrain{{.seed = 1, .ocean_fraction = 0.7, .radius = 115'000.0}};
    const auto& landform = terrain.landform();
    const auto& grid = landform.grid();
    std::int64_t steep_bare = 0;
    int steep = 0;
    int covered = 0;
    int land_points = 0;
    int mostly_bare = 0;
    int exposed = 0;
    int agreeing = 0;
    int layered = 0;
    for (std::uint32_t index = 0; index < grid.size(); index += 7) {
        const CellId cell = grid.cell(index);
        const std::int64_t ground = terrain.elevation(cell);
        const hog::planet::Exposure exposure = terrain.exposure(cell, ground);
        CHECK(exposure.bare >= 0);
        CHECK(exposure.bare <= fixed_one);
        if (ground <= 0) {
            CHECK(exposure.bare == 0);
            continue;
        }
        if (landform.heights()[index] > 100 * fixed_one &&
            landform.steepness()[index] > fixed_one * 7 / 10 &&
            landform.soil()[index] < fixed_one / 5) {
            ++steep;
            steep_bare += exposure.bare;
        }
        if (landform.steepness()[index] < fixed_one / 10 && landform.soil()[index] > fixed_one &&
            exposure.bare == 0) {
            ++covered;
        }
        ++land_points;
        mostly_bare += exposure.bare > fixed_one / 2 ? 1 : 0;
        // Uplift is read where the meanders shift the cell to, so nearly always the same bed.
        const auto column =
            terrain.beds().column(cell, {.continental = terrain.continents().height(cell) > 0,
                                         .mountains = terrain.mountain_strength(cell)});
        const std::int64_t frame = ground - grid.smooth(landform.risen(), cell);
        ++exposed;
        const auto bed = terrain.beds().bed_at(column, frame);
        if (bed && exposure.layer >> hog::planet::fixed_fraction_bits == bed->index) {
            ++layered;
        }
        agreeing += exposure.rock == terrain.beds().rock_at(column, frame) &&
                            (bed.has_value() || exposure.layer == 0)
                        ? 1
                        : 0;
    }
    INFO(steep << " steep, " << covered << " covered, " << agreeing << " of " << exposed
               << " agreeing, " << layered << " layered, " << mostly_bare << " of " << land_points
               << " land points mostly bare");
    REQUIRE(steep > 20);
    // Read smoothly between cells, so a little gentler than the steep cells alone.
    CHECK(steep_bare / steep > fixed_one * 2 / 3);
    // Bare rock is the exception: most land keeps its soil.
    CHECK(mostly_bare > land_points / 50);
    CHECK(mostly_bare < land_points / 5);
    CHECK(covered > 100);
    CHECK(layered > 100);
    CHECK(agreeing > exposed * 95 / 100);
}

TEST_CASE("ground dug into shows bare rock", "[planet][elevation]") {
    const Elevation terrain{{.seed = 1, .ocean_fraction = 0.7, .radius = 115'000.0}};
    const auto& landform = terrain.landform();
    const auto& grid = landform.grid();
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const CellId cell = grid.cell(index);
        const std::int64_t ground = terrain.elevation(cell);
        if (ground > 100 * fixed_one && terrain.exposure(cell, ground).bare == 0) {
            CHECK(terrain.exposure(cell, ground, fixed_one).bare == fixed_one);
            return;
        }
    }
    FAIL("no covered ground");
}

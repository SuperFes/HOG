#include <hog/planet/continents.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

using hog::planet::CellId;
using hog::planet::Continents;
using hog::planet::Plates;

namespace {

constexpr double radius = 115'000.0;

Continents on_plates(std::uint64_t seed, double ocean_fraction) {
    return Continents{seed, ocean_fraction,
                      std::make_shared<const Plates>(
                          seed, hog::planet::LevelGrid{Plates::level, radius}, radius, 10)};
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

double ocean_area_fraction(const Continents& continents, int level) {
    double ocean = 0.0;
    double total = 0.0;
    for (const CellId cell : cells_at(level)) {
        const double area = hog::planet::cell_area(cell);
        total += area;
        if (!continents.is_land(cell)) {
            ocean += area;
        }
    }
    return ocean / total;
}

// Sizes of the connected regions of land and of ocean, joined across cell edges.
struct Regions {
    std::vector<std::size_t> land;
    std::vector<std::size_t> ocean;
};

Regions regions(const Continents& continents, int level) {
    const std::size_t side = std::size_t{1} << static_cast<unsigned>(level);
    const auto index = [side](CellId cell) {
        const auto ij = cell.to_face_ij();
        return (((static_cast<std::size_t>(ij.face) * side) + ij.j) * side) + ij.i;
    };
    const auto cells = cells_at(level);
    std::vector<bool> land(cells.size());
    for (const CellId cell : cells) {
        land[index(cell)] = continents.is_land(cell);
    }

    Regions result;
    std::vector<bool> seen(cells.size());
    for (const CellId start : cells) {
        if (seen[index(start)]) {
            continue;
        }
        const bool is_land = land[index(start)];
        std::size_t size = 0;
        std::vector<CellId> stack{start};
        seen[index(start)] = true;
        while (!stack.empty()) {
            const CellId cell = stack.back();
            stack.pop_back();
            ++size;
            for (const CellId next : hog::planet::edge_neighbors(cell)) {
                if (land[index(next)] == is_land && !seen[index(next)]) {
                    seen[index(next)] = true;
                    stack.push_back(next);
                }
            }
        }
        (is_land ? result.land : result.ocean).push_back(size);
    }
    std::ranges::sort(result.land, std::greater{});
    std::ranges::sort(result.ocean, std::greater{});
    return result;
}

std::size_t total(const std::vector<std::size_t>& sizes) {
    return std::accumulate(sizes.begin(), sizes.end(), std::size_t{0});
}

} // namespace

TEST_CASE("the ocean covers the target fraction of the surface", "[planet][continents]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const double ocean_fraction = GENERATE(0.3, 0.7);
    const Continents continents = on_plates(seed, ocean_fraction);
    CHECK(std::abs(ocean_area_fraction(continents, Continents::survey_level) - ocean_fraction) <
          0.02);
}

TEST_CASE("a planet without ocean is all land", "[planet][continents]") {
    const Continents continents = on_plates(1, 0.0);
    CHECK(std::ranges::all_of(cells_at(Continents::survey_level),
                              [&](CellId cell) { return continents.is_land(cell); }));
}

TEST_CASE("land forms several continents around one world ocean", "[planet][continents]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U, 4U);
    const Regions found = regions(on_plates(seed, 0.7), Continents::survey_level);
    const std::size_t land = total(found.land);
    REQUIRE(found.land.size() >= 2);
    CHECK(found.land[0] < land * 9 / 10);
    CHECK(found.land[1] > land / 20);
    CHECK(found.ocean[0] > total(found.ocean) * 9 / 10);
}

TEST_CASE("continent heights change smoothly between neighbours", "[planet][continents]") {
    const Continents continents = on_plates(1, 0.7);
    std::mt19937_64 rng{7};
    std::uniform_int_distribution<std::uint32_t> coordinate{0, (1U << 12U) - 1};
    std::uniform_int_distribution<int> face{0, hog::planet::face_count - 1};
    std::int64_t largest_step = 0;
    for (int sample = 0; sample < 2'000; ++sample) {
        const CellId cell = CellId::from_face_ij(face(rng), coordinate(rng), coordinate(rng), 12);
        for (const CellId next : hog::planet::edge_neighbors(cell)) {
            largest_step =
                std::max(largest_step, std::abs(continents.height(cell) - continents.height(next)));
        }
    }
    CHECK(largest_step < hog::planet::fixed_one / 64);
}

TEST_CASE("continents depend on the seed", "[planet][continents]") {
    const CellId cell = CellId::from_face_ij(2, 100, 200, 10);
    const Continents first = on_plates(1, 0.7);
    CHECK(on_plates(1, 0.7).sea_level() == first.sea_level());
    CHECK(on_plates(1, 0.7).height(cell) == first.height(cell));
    CHECK(on_plates(2, 0.7).height(cell) != first.height(cell));
    // Salted, so not the planet seed's plain noise.
    CHECK(first.field(cell) !=
          hog::planet::fbm(1, hog::planet::noise_point(hog::planet::cell_center(cell), 1.5), 6));
}

TEST_CASE("continents ride on the plates whose crust rises", "[planet][continents]") {
    const std::uint64_t seed = GENERATE(1U, 2U, 3U);
    const Continents found = on_plates(seed, 0.7);
    const Plates& plates = found.plates();
    const auto by_crust = [&](std::uint32_t plate) { return plates.plates()[plate].continental; };
    std::vector<std::uint32_t> order;
    for (std::uint32_t plate = 0; plate < plates.plates().size(); ++plate) {
        order.push_back(plate);
    }
    std::ranges::sort(order, {}, by_crust);
    // Land over each plate's ground well inside its boundaries.
    const auto land_on = [&](std::uint32_t plate) {
        std::size_t land = 0;
        std::size_t inside = 0;
        for (std::uint32_t index = 0; index < plates.grid().size(); ++index) {
            if (plates.plate()[index] == plate &&
                plates.boundaries()[index].distance > 30'000 * hog::planet::fixed_one) {
                ++inside;
                land += found.is_land(plates.grid().cell(index)) ? 1 : 0;
            }
        }
        return static_cast<double>(land) / static_cast<double>(std::max<std::size_t>(inside, 1));
    };
    CHECK(land_on(order.back()) > 0.6);
    CHECK(land_on(order.front()) < 0.1);
}

TEST_CASE("continent preconditions", "[planet][continents]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS(on_plates(1, -0.1), AssertionFailed);
    CHECK_THROWS_AS(on_plates(1, 1.0), AssertionFailed);
    CHECK_THROWS_AS(on_plates(1, std::numeric_limits<double>::quiet_NaN()), AssertionFailed);
}

namespace {

constexpr double spin = 50'000.0 / radius;

// Two plates grown from opposite sides of the planet, turning about the pole so they close over
// +y and part over -y, carrying the given crust.
Continents pair(double east_crust, double west_crust, double turning = spin) {
    return Continents{
        1, 0.3,
        std::make_shared<const Plates>(
            1, hog::planet::LevelGrid{Plates::level, radius}, radius,
            std::vector<Plates::Plate>{
                {.origin = {1.0, 0.0, 0.0}, .spin = {0.0, 0.0, turning}, .continental = east_crust},
                {.origin = {-1.0, 0.0, 0.0},
                 .spin = {0.0, 0.0, -turning},
                 .continental = west_crust}})};
}

// `metres` east of the boundary over +y or -y, about where the plates meet.
CellId east_of(double metres, double y) {
    return hog::planet::cell_at(glm::normalize(hog::DVec3{metres / radius, y, 0.0}));
}

double mountains(const Continents& continents, CellId cell) {
    return hog::planet::fixed_to_double(continents.crust(cell).mountains);
}

} // namespace

TEST_CASE("continents colliding raise a range along where they meet", "[planet][continents]") {
    const Continents found = pair(0.8, 0.8);
    CHECK(mountains(found, east_of(0.0, 1.0)) > 0.5);
    CHECK(mountains(found, hog::planet::cell_at(hog::DVec3{1.0, 0.0, 0.0})) == 0.0);
}

TEST_CASE("a continent overriding ocean floor raises its range inland", "[planet][continents]") {
    const Continents found = pair(0.8, -0.8);
    // Averaged along the boundary, which wanders.
    const auto across = [&](double metres) {
        double sum = 0.0;
        for (int along = -10; along <= 10; ++along) {
            const double z = along * 2'000.0 / radius;
            sum += mountains(
                found, hog::planet::cell_at(glm::normalize(hog::DVec3{metres / radius, 1.0, z})));
        }
        return sum / 21.0;
    };
    double peak = 0.0;
    double highest = 0.0;
    for (int step = -20; step <= 30; ++step) {
        const double metres = step * 2'000.0;
        if (across(metres) > highest) {
            highest = across(metres);
            peak = metres;
        }
    }
    CHECK(highest > 0.3);
    CHECK(peak > 5'000.0);
    CHECK(across(-30'000.0) < 0.01);
}

TEST_CASE("continents parting sink into a rift", "[planet][continents]") {
    const Continents parting = pair(0.8, 0.8);
    const Continents still = pair(0.8, 0.8, 0.0);
    const CellId rift = east_of(0.0, -1.0);
    CHECK(parting.field(rift) < still.field(rift) - (hog::planet::fixed_one / 20));
    const CellId far = hog::planet::cell_at(hog::DVec3{1.0, 0.0, 0.0});
    CHECK(parting.field(far) == still.field(far));
}

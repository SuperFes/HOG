#include <hog/jobs/job_system.hpp>
#include <hog/planet/lithosphere.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>
#include <hog/planet/volume.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::planet::Cover;
using hog::planet::Crust;
using hog::planet::CrustType;
using hog::planet::Forces;
using hog::planet::LevelGrid;
using hog::planet::Lithosphere;
using hog::planet::loose_sediment;
using hog::planet::Mantle;
using hog::planet::Material;
using hog::planet::sedimentary_rock;
using hog::planet::Strength;
using hog::planet::Tectonics;
using hog::planet::volcanic_rock;
using hog::planet::Volume;

namespace {

constexpr double radius = 115'000.0;
constexpr double step_years = 10.0;
constexpr Tectonics::Settings settings{.level = 5, .plates = 10, .continental = 0.3, .travel = 1.5};

const Tectonics& start() {
    static const Tectonics tectonics{1, radius, settings};
    return tectonics;
}

// `at` turned by `angle` radians about the unit `axis`.
DVec3 turned(DVec3 at, DVec3 axis, double angle) {
    return glm::angleAxis(angle, axis) * at;
}

// Square metres of the planet each plate carries crust over, sampled on a finer grid.
std::vector<double> covered(const Lithosphere& lithosphere) {
    std::vector<double> areas(lithosphere.plates().size(), 0.0);
    const LevelGrid samples{settings.level + 2, radius};
    for (std::uint32_t sample = 0; sample < samples.size(); ++sample) {
        const DVec3 at = hog::planet::cell_center(samples.cell(sample));
        for (std::uint32_t plate = 0; plate < areas.size(); ++plate) {
            if (lithosphere.crust_of(plate, at)) {
                areas[plate] += static_cast<double>(samples.area(sample));
            }
        }
    }
    return areas;
}

// One plate over the whole grid, all of it the same crust.
Lithosphere::Plate everywhere(const LevelGrid& grid, Crust crust, DVec3 pole, double rate) {
    Lithosphere::Plate plate{.pole = pole, .rate = rate, .cells = {}, .crust = {}};
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        plate.cells.push_back(cell);
        plate.crust.push_back(crust);
    }
    return plate;
}

// Ground a kilometre over a sea at nought, or four under it where `drowned`.
std::vector<std::int64_t> ground(const std::vector<bool>& drowned) {
    std::vector<std::int64_t> heights(drowned.size());
    for (std::size_t place = 0; place < drowned.size(); ++place) {
        heights[place] = (drowned[place] ? -4'000 : 1'000) * hog::planet::fixed_one;
    }
    return heights;
}

// The plate's cells of the northern half of its own frame only.
Lithosphere::Plate northern(const LevelGrid& grid, Lithosphere::Plate plate) {
    Lithosphere::Plate half = plate;
    half.cells.clear();
    half.crust.clear();
    for (std::size_t index = 0; index < plate.cells.size(); ++index) {
        if (hog::planet::cell_center(grid.cell(plate.cells[index])).z > 0.0) {
            half.cells.push_back(plate.cells[index]);
            half.crust.push_back(plate.crust[index]);
        }
    }
    return half;
}

// Two halves of the planet meeting along the equator, turning about the x axis in opposite ways,
// so they part around +y and meet around -y.
Lithosphere halves() {
    const LevelGrid grid{4, radius};
    const DVec3 pole{1.0, 0.0, 0.0};
    constexpr double rate = 0.01;
    const Lithosphere::Plate north = northern(grid, everywhere(grid, Crust{}, pole, rate));
    Lithosphere::Plate south = northern(grid, everywhere(grid, Crust{}, pole, -rate));
    south.frame = glm::angleAxis(std::numbers::pi, pole);
    return Lithosphere{grid, {north, south}};
}

// On the equator where the halves part, and either side of it.
constexpr DVec3 on_equator{0.0, 1.0, 0.0};
DVec3 near_north() {
    return glm::normalize(DVec3{0.0, 1.0, 0.1});
}
DVec3 near_south() {
    return glm::normalize(DVec3{0.0, 1.0, -0.1});
}

constexpr auto no_plate = std::numeric_limits<std::uint32_t>::max();

std::uint32_t top_plate(const Lithosphere& lithosphere, DVec3 at) {
    const auto found = lithosphere.top(at);
    return found ? found->plate : no_plate;
}

} // namespace

TEST_CASE("a lithosphere starts with each cell's crust on the plate it lay on",
          "[planet][lithosphere]") {
    const Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    const auto& grid = lithosphere.grid();
    REQUIRE(lithosphere.plates().size() == start().plates().size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const Tectonics::Column& column = start().columns()[cell];
        const auto found = lithosphere.top(hog::planet::cell_center(grid.cell(cell)));
        REQUIRE(found.has_value());
        const Lithosphere::Found top = found.value_or(Lithosphere::Found{});
        CHECK(top.plate == column.plate);
        CHECK(top.crust.thickness == column.thickness);
        CHECK((top.crust.type == CrustType::Continental) ==
              (column.crust == Tectonics::Crust::Continental));
        CHECK(top.crust.age == column.age * static_cast<float>(step_years));
    }
}

TEST_CASE("a plate carries its crust round its pole and ages it", "[planet][lithosphere]") {
    Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    const Lithosphere before = lithosphere;
    constexpr double years = 25.0;
    lithosphere.advance(years);
    CHECK(lithosphere.time() == years);

    const auto& grid = lithosphere.grid();
    for (std::uint32_t plate = 0; plate < lithosphere.plates().size(); ++plate) {
        const Lithosphere::Plate& carrier = lithosphere.plates()[plate];
        CHECK(carrier.cells == before.plates()[plate].cells);
        for (std::size_t index = 0; index < carrier.cells.size(); index += 7) {
            const DVec3 was = hog::planet::cell_center(grid.cell(carrier.cells[index]));
            const DVec3 now = turned(was, carrier.pole, carrier.rate * years);
            CHECK_THAT(glm::length(lithosphere.to_planet(plate, was) - now), WithinAbs(0.0, 1e-9));
            const std::optional<Crust> crust = lithosphere.crust_of(plate, now);
            REQUIRE(crust.has_value());
            CHECK_THAT(
                crust.value_or(Crust{}).age,
                WithinAbs(static_cast<double>(before.plates()[plate].crust[index].age) + years,
                          1e-3));
        }
    }
}

TEST_CASE("a plate keeps the area of its crust however far it turns", "[planet][lithosphere]") {
    Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    const std::vector<double> before = covered(lithosphere);
    lithosphere.advance(40.0 * step_years);
    const std::vector<double> after = covered(lithosphere);
    for (std::size_t plate = 0; plate < before.size(); ++plate) {
        CHECK_THAT(after[plate], WithinRel(before[plate], 0.03));
    }
}

TEST_CASE("a plate turned all the way round is back where it began", "[planet][lithosphere]") {
    const LevelGrid grid{3, radius};
    const DVec3 pole = glm::normalize(DVec3{1.0, 2.0, 3.0});
    Lithosphere lithosphere{grid, {everywhere(grid, Crust{}, pole, std::numbers::pi / 50.0)}};
    for (int step = 0; step < 100; ++step) {
        lithosphere.advance(1.0);
    }
    const DVec3 at = glm::normalize(DVec3{0.3, -0.5, 0.8});
    CHECK_THAT(glm::length(lithosphere.to_plate(0, at) - at), WithinAbs(0.0, 1e-9));
}

TEST_CASE("where plates overlap the lighter crust stays on top", "[planet][lithosphere]") {
    const LevelGrid grid{3, radius};
    const DVec3 pole{0.0, 0.0, 1.0};
    const Crust old_floor{.type = CrustType::Oceanic, .thickness = 7.0F, .age = 80.0F};
    const Crust young_floor{.type = CrustType::Oceanic, .thickness = 7.0F, .age = 5.0F};
    const Crust continent{.type = CrustType::Continental, .thickness = 35.0F, .age = 900.0F};
    const DVec3 at = glm::normalize(DVec3{0.2, 0.4, 0.9});

    const Lithosphere floors{
        grid, {everywhere(grid, old_floor, pole, 0.0), everywhere(grid, young_floor, pole, 0.0)}};
    CHECK(top_plate(floors, at) == 1);

    const Lithosphere landed{
        grid, {everywhere(grid, continent, pole, 0.0), everywhere(grid, young_floor, pole, 0.0)}};
    CHECK(top_plate(landed, at) == 0);

    const Lithosphere same{
        grid, {everywhere(grid, young_floor, pole, 0.0), everywhere(grid, young_floor, pole, 0.0)}};
    CHECK(top_plate(same, at) == 0);
}

TEST_CASE("plates turning apart leave places with no crust", "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    CHECK(top_plate(lithosphere, near_north()) == 0);
    CHECK(top_plate(lithosphere, near_south()) == 1);

    lithosphere.advance(20.0);
    CHECK_FALSE(lithosphere.top(on_equator));
    CHECK(top_plate(lithosphere, DVec3{0.0, 0.0, 1.0}) == 0);
    CHECK(top_plate(lithosphere, DVec3{0.0, 0.0, -1.0}) == 1);
}

TEST_CASE("new ocean floor fills where plates part, split down the middle between them",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    constexpr double years = 20.0;
    lithosphere.advance(years);
    const Lithosphere moved = lithosphere;
    lithosphere.spread();
    const auto& grid = lithosphere.grid();
    // The place of the grid over a direction, as the lithosphere is read.
    const auto place = [&](DVec3 at) {
        return hog::planet::cell_center(hog::planet::cell_at(at, grid.level()));
    };

    for (const DVec3 at : {on_equator, near_north(), near_south()}) {
        const auto found = lithosphere.top(place(at));
        REQUIRE(found.has_value());
        const Crust crust = found.value_or(Lithosphere::Found{}).crust;
        CHECK(crust.type == CrustType::Oceanic);
        CHECK(crust.age == 0.0F);
        CHECK(crust.thickness == Crust{}.thickness);
    }
    CHECK(top_plate(lithosphere, place(near_north())) == 0);
    CHECK(top_plate(lithosphere, place(near_south())) == 1);

    // Nothing changes where they meet, nor where crust already lay but right at the gap's edge,
    // where a cell of new floor in a plate's own grid can reach over the place next to it.
    const DVec3 meeting{0.0, -1.0, 0.0};
    CHECK(lithosphere.crust_of(0, meeting) == moved.crust_of(0, meeting));
    CHECK(lithosphere.crust_of(1, meeting) == moved.crust_of(1, meeting));
    const auto bare = [&](std::uint32_t cell) {
        return !moved.top(hog::planet::cell_center(grid.cell(cell)));
    };
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const DVec3 at = hog::planet::cell_center(grid.cell(cell));
        if (bare(cell)) {
            CHECK(lithosphere.top(at).has_value());
        } else if (!std::ranges::any_of(grid.neighbors(cell), bare)) {
            CHECK(lithosphere.top(at) == moved.top(at));
        }
    }
}

TEST_CASE("spreading leaves no place of the grid without crust", "[planet][lithosphere]") {
    Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    const auto& grid = lithosphere.grid();
    // Share of the planet bare, sampled at the grid's places or finer, between them.
    const auto bare = [&](int level) {
        const LevelGrid samples{level, radius};
        double area = 0.0;
        for (std::uint32_t sample = 0; sample < samples.size(); ++sample) {
            if (!lithosphere.top(hog::planet::cell_center(samples.cell(sample)))) {
                area += static_cast<double>(samples.area(sample));
            }
        }
        return area / (4.0 * std::numbers::pi * radius * radius);
    };
    for (int step = 0; step < 10; ++step) {
        lithosphere.advance(step_years);
        const double parted = bare(grid.level() + 2);
        CHECK(bare(grid.level()) > 0.0);
        lithosphere.spread();
        CHECK(bare(grid.level()) == 0.0);
        // Slivers narrower than a place along every edge between plates aren't gaps.
        CHECK(bare(grid.level() + 2) < parted / 3.0);
    }
}

namespace {

// Places of the grid where more than one plate carries crust.
int overlapping(const Lithosphere& lithosphere) {
    const auto& grid = lithosphere.grid();
    int places = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const DVec3 at = hog::planet::cell_center(grid.cell(cell));
        int carrying = 0;
        for (std::uint32_t plate = 0; plate < lithosphere.plates().size(); ++plate) {
            carrying += lithosphere.crust_of(plate, at) ? 1 : 0;
        }
        places += carrying > 1 ? 1 : 0;
    }
    return places;
}

} // namespace

TEST_CASE("ocean floor under another plate dives and the plate over it rises into an arc",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    constexpr double years = 20.0;
    lithosphere.advance(years);
    const Lithosphere moved = lithosphere;
    const int met = overlapping(moved);
    REQUIRE(met > 0);
    lithosphere.subduct(years);

    // Floor of the same age on both: the lower plate stays on top, and the other's dives.
    const auto& grid = lithosphere.grid();
    const DVec3 meeting =
        hog::planet::cell_center(hog::planet::cell_at(DVec3{0.0, -1.0, 0.0}, grid.level()));
    CHECK(lithosphere.crust_of(0, meeting).has_value());
    CHECK_FALSE(lithosphere.crust_of(1, meeting).has_value());
    CHECK(overlapping(lithosphere) < met / 5);
    CHECK(lithosphere.plates()[1].cells.size() < moved.plates()[1].cells.size());
    CHECK(lithosphere.plates()[0].cells.size() == moved.plates()[0].cells.size());

    // Behind the trench, and only there, the plate on top is thicker by the arc's growth.
    const hog::planet::Arcs arcs{};
    std::vector<DVec3> dove;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const DVec3 at = hog::planet::cell_center(grid.cell(cell));
        if (moved.crust_of(1, at) && !lithosphere.crust_of(1, at)) {
            dove.push_back(at);
        }
    }
    const double place_angle = std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid.size()));
    // Rings of places taken by their corners reach up to a diagonal's length out.
    const auto near_trench = [&](DVec3 at) {
        return std::ranges::any_of(dove, [&](DVec3 trench) {
            return std::acos(std::min(glm::dot(at, trench), 1.0)) <
                   (arcs.reach + 1) * place_angle * std::numbers::sqrt2;
        });
    };
    int raised = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const DVec3 at = hog::planet::cell_center(grid.cell(cell));
        const auto found = lithosphere.top(at);
        if (!found) {
            continue;
        }
        const Crust crust = found.value_or(Lithosphere::Found{}).crust;
        if (crust.thickness == Crust{}.thickness) {
            continue;
        }
        ++raised;
        CHECK(found.value_or(Lithosphere::Found{}).plate == 0);
        CHECK(near_trench(at));
        CHECK_THAT(crust.thickness, WithinAbs(static_cast<double>(Crust{}.thickness) +
                                                  (years * static_cast<double>(arcs.growth)),
                                              1e-4));
        CHECK(crust.raised == 0.0F);
        CHECK(crust.type == CrustType::Oceanic);
        // Some of it lava on top.
        CHECK_THAT(static_cast<double>(crust.cover[volcanic_rock]),
                   WithinAbs(years * static_cast<double>(arcs.growth * arcs.erupted), 1e-5));
    }
    CHECK(raised > 0);
}

TEST_CASE("the older plate's floor dives all along where they meet, however its age varies",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    std::vector<Lithosphere::Plate> plates{lithosphere.plates().begin(),
                                           lithosphere.plates().end()};
    for (Crust& crust : plates[0].crust) {
        crust.age = 50.0F;
    }
    // Older on the whole, but younger than the other at one place in four.
    for (std::size_t index = 0; index < plates[1].crust.size(); ++index) {
        plates[1].crust[index].age = index % 4 == 0 ? 40.0F : 70.0F;
    }
    lithosphere = Lithosphere{lithosphere.grid(), plates};
    constexpr double years = 20.0;
    lithosphere.advance(years);
    const Lithosphere moved = lithosphere;
    lithosphere.subduct(years);
    CHECK(lithosphere.plates()[0].cells.size() == moved.plates()[0].cells.size());
    CHECK(lithosphere.plates()[1].cells.size() < moved.plates()[1].cells.size());
}

TEST_CASE("an arc's lava buried deep is baked into crystalline rock", "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    constexpr double years = 20.0;
    lithosphere.advance(years);
    lithosphere.subduct(years, {.reach = 1, .growth = 4.0F});
    const auto& grid = lithosphere.grid();
    int raised = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const Crust crust = lithosphere.top(hog::planet::cell_center(grid.cell(cell)))
                                .value_or(Lithosphere::Found{})
                                .crust;
        if (crust.thickness > Crust{}.thickness) {
            ++raised;
            CHECK(crust.cover == Cover{0.0F, 0.0F, hog::planet::hardened});
        }
    }
    CHECK(raised > 0);
}

TEST_CASE("plumes rise anywhere, each as buoyant as its own, bringing more under a bigger planet",
          "[planet][lithosphere]") {
    using hog::planet::plumes;
    constexpr double earth = 6.371e6;
    const hog::planet::Plumes plumes_of;
    const auto rising = plumes(7, earth);
    CHECK(rising.size() == static_cast<std::size_t>(plumes_of.count));
    const auto bigger = plumes(7, 2.0 * earth);
    REQUIRE(bigger.size() == rising.size());
    for (std::size_t index = 0; index < rising.size(); ++index) {
        CHECK(bigger[index].at == rising[index].at);
        CHECK_THAT(bigger[index].magma, WithinRel(4.0 * rising[index].magma, 1e-12));
    }
    CHECK(plumes(7, earth) == rising);
    CHECK(plumes(8, earth) != rising);
    double north = 0.0;
    for (const hog::planet::Plume& plume : rising) {
        CHECK_THAT(glm::length(plume.at), WithinAbs(1.0, 1e-12));
        CHECK(plume.magma >= plumes_of.weakest * plumes_of.magma);
        CHECK(plume.magma <= plumes_of.strongest * plumes_of.magma);
        north += plume.at.z;
    }
    // Not all to one side.
    CHECK(std::abs(north) < static_cast<double>(rising.size()) / 2.0);
}

TEST_CASE("a plume builds the crust over it up along the track its plate carries over it",
          "[planet][lithosphere]") {
    const LevelGrid grid{5, radius};
    constexpr double years = 10.0;
    const hog::planet::Plumes plumes_of;
    // Enough to raise a place by half a kilometre over the years, well short of baking its lava.
    const double place_km2 = static_cast<double>(grid.area(0)) * 1e-6;
    const std::vector<hog::planet::Plume> rising{
        {.at = DVec3{1.0, 0.0, 0.0}, .magma = 0.05 * place_km2}};
    const double across = std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid.size()));
    const auto fed = [&](double rate) {
        Lithosphere lithosphere{grid, {everywhere(grid, Crust{}, DVec3{0.0, 0.0, 1.0}, rate)}};
        lithosphere.feed(rising, years, plumes_of);
        double added = 0.0;
        int raised = 0;
        const Lithosphere::Plate& plate = lithosphere.plates()[0];
        for (std::size_t index = 0; index < plate.cells.size(); ++index) {
            const Crust& crust = plate.crust[index];
            const double thicker = static_cast<double>(crust.thickness - Crust{}.thickness);
            if (thicker <= 0.0) {
                continue;
            }
            ++raised;
            added += thicker * static_cast<double>(grid.area(plate.cells[index])) * 1e-6;
            CHECK_THAT(static_cast<double>(crust.cover[volcanic_rock]),
                       WithinRel(thicker * static_cast<double>(plumes_of.erupted), 1e-3));
            // Behind the plume, along the way the plate turns.
            const DVec3 at = hog::planet::cell_center(grid.cell(plate.cells[index]));
            const double longitude = std::atan2(at.y, at.x);
            CHECK(std::abs(at.z) < across);
            CHECK(longitude > -across);
            CHECK(longitude < (rate * years) + across);
        }
        CHECK_THAT(added, WithinRel(rising[0].magma * years, 1e-4));
        return raised;
    };
    // Standing still, all in one place; moving on, strung out behind.
    CHECK(fed(0.0) == 1);
    CHECK(fed(0.02) > 2);
}

TEST_CASE("bodies strike anywhere, smaller ones more often, and far more often long ago",
          "[planet][lithosphere]") {
    using hog::planet::impacts;
    constexpr double earth_km = 6'371.0;
    const hog::planet::Impacts rates;
    const double twenty = 20.0 / earth_km;
    const auto struck = impacts(7, 0.0, 100.0, twenty);
    const double expected = rates.rate * 4.0 * std::numbers::pi * earth_km * earth_km * 100.0;
    CHECK(std::abs(static_cast<double>(struck.size()) - expected) < 5.0 * std::sqrt(expected));
    DVec3 middle{0.0};
    for (const hog::planet::Impact& impact : struck) {
        CHECK(impact.diameter >= twenty);
        CHECK_THAT(glm::length(impact.at), WithinAbs(1.0, 1e-12));
        middle += impact.at / static_cast<double>(struck.size());
    }
    CHECK(glm::length(middle) < 0.2);
    CHECK(impacts(7, 0.0, 100.0, twenty) == struck);
    CHECK(impacts(7, 10.0, 100.0, twenty) != struck);
    // Twice as big, a quarter as many.
    const double quarter = expected / 4.0;
    CHECK(std::abs(static_cast<double>(impacts(7, 0.0, 100.0, 2.0 * twenty).size()) - quarter) <
          5.0 * std::sqrt(quarter));
    // Four billion years ago, ten million years brought what five billion would now.
    const double big = 10.0 * twenty;
    CHECK(impacts(7, 4'000.0, 10.0, big).size() > 50);
    CHECK(impacts(7, 0.0, 10.0, big).size() < 5);
}

TEST_CASE("a body digs a crater, its cover first, and throws it all out around it",
          "[planet][lithosphere]") {
    constexpr double earth_km = 6'371.0;
    const LevelGrid grid{5, radius};
    const Crust land{.type = CrustType::Continental, .thickness = 70.0F, .cover = {1.0F}};
    Lithosphere lithosphere{grid, {everywhere(grid, land, DVec3{0.0, 0.0, 1.0}, 0.0)}};
    const double across = std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid.size()));
    const hog::planet::Impact impact{.at = hog::planet::cell_center(grid.cell(0)),
                                     .diameter = 5.0 * across};
    const hog::planet::Craters craters;
    lithosphere.strike(std::span{&impact, 1}, craters);

    const double transient =
        std::pow(impact.diameter * earth_km * std::pow(craters.simple, 0.13) / 1.17, 1.0 / 1.13);
    const double rim = transient / 2.0 / earth_km;
    const Lithosphere::Plate& plate = lithosphere.plates()[0];
    double before = 0.0;
    double after = 0.0;
    std::optional<double> spread;
    int dug = 0;
    for (std::size_t index = 0; index < plate.cells.size(); ++index) {
        const Crust& crust = plate.crust[index];
        const DVec3 at = hog::planet::cell_center(grid.cell(plate.cells[index]));
        const double apart =
            std::atan2(glm::length(glm::cross(at, impact.at)), glm::dot(at, impact.at));
        const auto area = static_cast<double>(grid.area(plate.cells[index]));
        before += static_cast<double>(land.thickness) * area;
        after += static_cast<double>(crust.thickness) * area;
        const auto gained = static_cast<double>(crust.thickness - land.thickness);
        if (apart < rim) {
            ++dug;
            CHECK(gained < 0.0);
            continue;
        }
        // Laid loose, pressed into rock and baked hard as deep as ever.
        CHECK_THAT(
            static_cast<double>(crust.cover[loose_sediment] + crust.cover[sedimentary_rock]),
            WithinAbs(std::min(1.0 + gained, static_cast<double>(hog::planet::hardened)), 1e-4));
        // Thinner as the cube of the distance, where thick enough to tell.
        if (apart < 3.0 * rim) {
            const double scaled = gained * apart * apart * apart;
            if (!spread) {
                spread = scaled;
            }
            CHECK_THAT(scaled, WithinRel(spread.value_or(0.0), 1e-3));
        }
    }
    CHECK(dug > 1);
    CHECK_THAT(after, WithinRel(before, 1e-5));
    const Crust& middle = plate.crust[0];
    CHECK_THAT(
        static_cast<double>(middle.thickness),
        WithinAbs(static_cast<double>(land.thickness) - (craters.excavated * transient), 1e-3));
    CHECK(middle.cover == Cover{});
}

TEST_CASE("ocean floor an arc thickens enough floats as continent", "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    constexpr double years = 20.0;
    lithosphere.advance(years);
    lithosphere.subduct(years, {.reach = 2, .growth = 1.0F});
    const auto& grid = lithosphere.grid();
    int continental = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const Crust crust = lithosphere.top(hog::planet::cell_center(grid.cell(cell)))
                                .value_or(Lithosphere::Found{})
                                .crust;
        if (crust.thickness >= Lithosphere::arc_continent) {
            ++continental;
            CHECK(crust.type == CrustType::Continental);
        }
    }
    CHECK(continental > 0);
}

TEST_CASE("plates that part and meet keep ocean floor only on top", "[planet][lithosphere]") {
    Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    const auto& grid = lithosphere.grid();
    // Places of the grid with no crust, and with ocean floor under another plate's crust.
    struct Count {
        int bare = 0;
        int under = 0;
    };
    const auto count = [&] {
        Count counted;
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            const DVec3 at = hog::planet::cell_center(grid.cell(cell));
            const auto found = lithosphere.top(at);
            counted.bare += found ? 0 : 1;
            for (std::uint32_t plate = 0; plate < lithosphere.plates().size(); ++plate) {
                const auto crust = lithosphere.crust_of(plate, at);
                if (found && plate != found->plate && crust && crust->type == CrustType::Oceanic) {
                    ++counted.under;
                    break;
                }
            }
        }
        return counted;
    };
    for (int step = 0; step < 10; ++step) {
        lithosphere.advance(step_years);
        const Count moved = count();
        lithosphere.subduct(step_years);
        lithosphere.spread();
        const Count settled = count();
        CAPTURE(step, moved.under, settled.under);
        CHECK(settled.bare == 0);
        // Left only along edges, by cells whose middles aren't under yet, and by new floor
        // reaching over a gap's edge; never piling up.
        CHECK(settled.under < moved.under / 2);
        CHECK(settled.under < static_cast<int>(grid.size() / 25));
    }
}

namespace {

constexpr Crust continent{
    .type = CrustType::Continental, .thickness = 35.0F, .age = 900.0F, .raised = 900.0F};

// The halves, both of them continent.
Lithosphere continental_halves() {
    const Lithosphere ocean = halves();
    std::vector<Lithosphere::Plate> plates{ocean.plates().begin(), ocean.plates().end()};
    for (auto& plate : plates) {
        std::ranges::fill(plate.crust, continent);
    }
    return Lithosphere{ocean.grid(), plates};
}

// Cubic kilometres of continental crust the plates carry.
double continental_volume(const Lithosphere& lithosphere) {
    const auto& grid = lithosphere.grid();
    double volume = 0.0;
    for (const auto& plate : lithosphere.plates()) {
        for (std::size_t index = 0; index < plate.cells.size(); ++index) {
            if (plate.crust[index].type == CrustType::Continental) {
                volume += static_cast<double>(grid.area(plate.cells[index])) * 1e-6 *
                          static_cast<double>(plate.crust[index].thickness);
            }
        }
    }
    return volume;
}

float thickest(const Lithosphere& lithosphere) {
    float most = 0.0F;
    for (const auto& plate : lithosphere.plates()) {
        for (const Crust& crust : plate.crust) {
            most = std::max(most, crust.thickness);
        }
    }
    return most;
}

} // namespace

TEST_CASE("continents meeting pile up into a range on the one on top", "[planet][lithosphere]") {
    Lithosphere lithosphere = continental_halves();
    constexpr double years = 20.0;
    lithosphere.advance(years);
    const Lithosphere moved = lithosphere;
    const double volume = continental_volume(moved);
    // Strong enough to hold all that piles up, so it stays where it piled.
    lithosphere.collide({.strongest = 1'000.0F});

    const auto& grid = lithosphere.grid();
    const DVec3 meeting =
        hog::planet::cell_center(hog::planet::cell_at(DVec3{0.0, -1.0, 0.0}, grid.level()));
    CHECK(lithosphere.crust_of(0, meeting).has_value());
    CHECK_FALSE(lithosphere.crust_of(1, meeting).has_value());
    CHECK(lithosphere.plates()[0].cells.size() == moved.plates()[0].cells.size());
    CHECK(lithosphere.plates()[1].cells.size() < moved.plates()[1].cells.size());
    CHECK_THAT(continental_volume(lithosphere), WithinRel(volume, 1e-5));

    int piled = 0;
    for (const Crust& crust : lithosphere.plates()[0].crust) {
        if (crust.thickness > continent.thickness) {
            ++piled;
            CHECK(crust.raised == 0.0F);
        } else {
            CHECK(crust.thickness == continent.thickness);
        }
    }
    CHECK(piled > 0);
    const auto brittle = static_cast<float>(hog::planet::LowerCrust{}.brittle);
    CHECK(thickest(lithosphere) >= (continent.thickness + brittle) * 0.9F);
}

TEST_CASE("continent going under has its warm crust thrust on beneath the plate on top",
          "[planet][lithosphere]") {
    Lithosphere moved = continental_halves();
    moved.advance(20.0);
    const double volume = continental_volume(moved);
    const auto& grid = moved.grid();
    // How far into the plate on top its crust thickens, as an angle from where they meet.
    const auto inland = [&](float dip) {
        Lithosphere lithosphere = moved;
        lithosphere.collide({.strongest = 1'000.0F, .dip = dip});
        CHECK_THAT(continental_volume(lithosphere), WithinRel(volume, 1e-5));
        const Lithosphere::Plate& top = lithosphere.plates()[0];
        double farthest = -1.0;
        for (std::size_t index = 0; index < top.cells.size(); ++index) {
            const DVec3 at =
                lithosphere.to_planet(0, hog::planet::cell_center(grid.cell(top.cells[index])));
            if (at.y < 0.0 && top.crust[index].thickness > continent.thickness) {
                farthest = std::max(farthest, std::asin(at.z));
            }
        }
        return farthest;
    };
    // A fault dipping a degree reaches the base of 35 km of crust some 2,000 km in.
    constexpr double earth_radius_km = 6'371.0;
    const double reach = static_cast<double>(continent.thickness) /
                         std::tan(std::numbers::pi / 180.0) / earth_radius_km;
    const double across = std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid.size()));
    const double steep = inland(hog::planet::Ranges{}.dip);
    const double shallow = inland(1.0F);
    CHECK(shallow - steep > reach - (2.0 * across));
    CHECK(shallow - steep < reach + across);
}

TEST_CASE("crust piled too thick flows out into the thinner continent around it",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = continental_halves();
    lithosphere.advance(20.0);
    const double volume = continental_volume(lithosphere);
    const hog::planet::Ranges ranges;
    lithosphere.collide(ranges);
    CHECK_THAT(continental_volume(lithosphere), WithinRel(volume, 1e-4));
    CHECK(thickest(lithosphere) <= ranges.strongest + 1e-3F);
    CHECK(thickest(lithosphere) > continent.thickness);
}

TEST_CASE("crust too thick with only ocean floor around spreads out over it",
          "[planet][lithosphere]") {
    // A thick arc of continent around the pole, with only ocean floor around it.
    const LevelGrid grid{4, radius};
    Lithosphere::Plate plate = everywhere(grid, Crust{}, DVec3{0.0, 0.0, 1.0}, 0.0);
    std::size_t arc = 0;
    for (std::size_t index = 0; index < plate.cells.size(); ++index) {
        if (hog::planet::cell_center(grid.cell(plate.cells[index])).z > 0.95) {
            plate.crust[index] = continent;
            plate.crust[index].thickness = 90.0F;
            ++arc;
        }
    }
    Lithosphere lithosphere{grid, {plate}};
    const double volume = continental_volume(lithosphere);
    const hog::planet::Ranges ranges;
    lithosphere.collide(ranges);
    CHECK(thickest(lithosphere) == ranges.strongest);
    CHECK(lithosphere.foundered() == 0.0);
    const auto& crust = lithosphere.plates()[0].crust;
    CHECK(std::ranges::count(crust, CrustType::Continental, &Crust::type) >
          static_cast<std::ptrdiff_t>(arc));
    // The floor buried under it as well.
    CHECK(continental_volume(lithosphere) > volume);
}

TEST_CASE("crust too thick with no room on its whole plate founders into the mantle",
          "[planet][lithosphere]") {
    const LevelGrid grid{4, radius};
    Crust thick = continent;
    thick.thickness = 90.0F;
    Lithosphere lithosphere{grid, {everywhere(grid, thick, DVec3{0.0, 0.0, 1.0}, 0.0)}};
    const double volume = continental_volume(lithosphere);
    const hog::planet::Ranges ranges;
    lithosphere.collide(ranges);
    CHECK(thickest(lithosphere) == ranges.strongest);
    CHECK_THAT(lithosphere.foundered(),
               WithinRel(volume * (90.0 - static_cast<double>(ranges.strongest)) / 90.0, 1e-6));
}

TEST_CASE("what wears off a continent is laid down on the nearest crust under the sea",
          "[planet][lithosphere]") {
    // A continent around the pole, with only ocean floor around it, and its edge from `shelf` out
    // under the sea.
    const LevelGrid grid{4, radius};
    const auto worn_off = [&](double shelf) {
        Lithosphere::Plate plate = everywhere(grid, Crust{}, DVec3{0.0, 0.0, 1.0}, 0.0);
        std::vector<bool> drowned(grid.size(), true);
        for (std::size_t index = 0; index < plate.cells.size(); ++index) {
            const double z = hog::planet::cell_center(grid.cell(plate.cells[index])).z;
            if (z > 0.9) {
                plate.crust[index] = continent;
                drowned[plate.cells[index]] = z < shelf;
            }
        }
        Lithosphere lithosphere{grid, {plate}};
        if (shelf > 0.0) {
            lithosphere.erode(std::vector<float>(grid.size(), 1.0F), ground(drowned), 0);
        }
        return lithosphere;
    };
    const auto volume = [&](const Lithosphere& lithosphere, CrustType type) {
        double total = 0.0;
        const Lithosphere::Plate& carrier = lithosphere.plates()[0];
        for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
            if (carrier.crust[index].type == type) {
                total += static_cast<double>(grid.area(carrier.cells[index])) *
                         static_cast<double>(carrier.crust[index].thickness);
            }
        }
        return total;
    };
    const Lithosphere start = worn_off(0.0);
    const double land = volume(start, CrustType::Continental);
    const double floor = volume(start, CrustType::Oceanic);

    // With no shelf, it all goes out onto the ocean floor next to the coast.
    const Lithosphere bare = worn_off(0.9);
    CHECK_THAT(volume(bare, CrustType::Continental) + volume(bare, CrustType::Oceanic),
               WithinRel(land + floor, 1e-6));
    CHECK(volume(bare, CrustType::Continental) < land);
    const Lithosphere::Plate& carrier = bare.plates()[0];
    for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
        const Crust& crust = carrier.crust[index];
        const double z = hog::planet::cell_center(grid.cell(carrier.cells[index])).z;
        if (crust.type == CrustType::Continental) {
            CHECK(crust.thickness == continent.thickness - 1.0F);
        } else {
            // All laid down as sediment, pressed into rock where it's deep.
            CHECK_THAT(
                static_cast<double>(crust.cover[loose_sediment] + crust.cover[sedimentary_rock]),
                WithinAbs(static_cast<double>(crust.thickness - Crust{}.thickness), 1e-5));
            CHECK(crust.cover[loose_sediment] <= hog::planet::lithified);
        }
        if (crust.type == CrustType::Oceanic && z < 0.8) {
            // Too far from the coast for any to reach.
            CHECK(crust.thickness == Crust{}.thickness);
        }
    }

    // With one, it stays on the continent, building the shelf, and none reaches the floor.
    const Lithosphere shelved = worn_off(0.95);
    CHECK_THAT(volume(shelved, CrustType::Continental), WithinRel(land, 1e-6));
    CHECK(volume(shelved, CrustType::Oceanic) == floor);
}

TEST_CASE("sediment fills the sea up to its level by the coast, and runs on down the slope",
          "[planet][lithosphere]") {
    // A continent around the pole, the ocean floor round it deepening away from it.
    const LevelGrid grid{4, radius};
    Lithosphere::Plate plate = everywhere(grid, Crust{}, DVec3{0.0, 0.0, 1.0}, 0.0);
    std::vector<std::int64_t> heights(grid.size());
    std::vector<bool> land(grid.size());
    for (std::size_t index = 0; index < plate.cells.size(); ++index) {
        const double z = hog::planet::cell_center(grid.cell(plate.cells[index])).z;
        land[plate.cells[index]] = z > 0.9;
        if (z > 0.9) {
            plate.crust[index] = continent;
        }
        heights[plate.cells[index]] = std::llround((z > 0.9 ? 1'000.0 : -20'000.0 * (0.9 - z)) *
                                                   static_cast<double>(hog::planet::fixed_one));
    }
    const auto volume = [&](const Lithosphere& lithosphere) {
        double total = 0.0;
        const Lithosphere::Plate& carrier = lithosphere.plates()[0];
        for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
            total += static_cast<double>(grid.area(carrier.cells[index])) *
                     static_cast<double>(carrier.crust[index].thickness);
        }
        return total;
    };
    Lithosphere lithosphere{grid, {plate}};
    const double before = volume(lithosphere);
    lithosphere.erode(std::vector<float>(grid.size(), 2.0F), heights, 0);
    CHECK_THAT(volume(lithosphere), WithinRel(before, 1e-6));
    int filled = 0;
    int beyond = 0;
    const Lithosphere::Plate& carrier = lithosphere.plates()[0];
    for (std::size_t index = 0; index < carrier.cells.size(); ++index) {
        const std::uint32_t place = carrier.cells[index];
        const auto gained = static_cast<double>(carrier.crust[index].thickness - Crust{}.thickness);
        if (land[place] || gained <= 0.0) {
            continue;
        }
        const double room = -hog::planet::fixed_to_double(heights[place]) / 1'000.0;
        const bool lower = std::ranges::any_of(grid.neighbors(place), [&](std::uint32_t next) {
            return !land[next] && heights[next] < heights[place];
        });
        if (lower) {
            CHECK(gained <= room + 1e-4);
            filled += gained > room - 1e-4 ? 1 : 0;
        }
        const bool coast = std::ranges::any_of(grid.neighbors(place),
                                               [&](std::uint32_t next) { return land[next]; });
        beyond += coast ? 0 : 1;
    }
    // Up to the sea's level, the shelf built out past the coast.
    CHECK(filled > 0);
    CHECK(beyond > 0);
}

TEST_CASE("sediment builds out from the coast ring by ring, filling hollows on its way",
          "[planet][lithosphere]") {
    // A continent around the pole, the ocean floor round it deepening away from it, with a hollow
    // in it near the coast.
    const LevelGrid grid{4, radius};
    Lithosphere::Plate plate = everywhere(grid, Crust{}, DVec3{0.0, 0.0, 1.0}, 0.0);
    std::vector<std::int64_t> heights(grid.size());
    std::vector<bool> land(grid.size());
    std::optional<std::uint32_t> hollow;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const double z = hog::planet::cell_center(grid.cell(cell)).z;
        land[cell] = z > 0.9;
        if (land[cell]) {
            plate.crust[cell] = continent;
        }
        heights[cell] = std::llround((land[cell] ? 1'000.0 : -20'000.0 * (0.9 - z)) *
                                     static_cast<double>(hog::planet::fixed_one));
    }
    // Off the coast, but not on it.
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const auto ashore = [&](std::uint32_t next) { return land[next]; };
        if (!land[cell] && std::ranges::none_of(grid.neighbors(cell), ashore) &&
            (!hollow || heights[cell] > heights[hollow.value_or(0)])) {
            hollow = cell;
        }
    }
    REQUIRE(hollow.has_value());
    const std::uint32_t pit = hollow.value_or(0);
    std::int64_t rim = std::numeric_limits<std::int64_t>::max();
    for (const std::uint32_t next : grid.neighbors(pit)) {
        rim = std::min(rim, heights[next]);
    }
    heights[pit] = rim - (500 * hog::planet::fixed_one);
    Lithosphere lithosphere{grid, {plate}};
    lithosphere.erode(std::vector<float>(grid.size(), 3.0F), heights, 0);
    const auto stands = [&](std::uint32_t cell) {
        const auto gained =
            static_cast<double>(lithosphere.plates()[0].crust[cell].thickness - Crust{}.thickness);
        return hog::planet::fixed_to_double(heights[cell]) + (gained * 1'000.0);
    };
    int beyond = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (!land[cell]) {
            // No higher than the sea.
            CHECK(stands(cell) <= 0.1);
            beyond +=
                cell != pit && lithosphere.plates()[0].crust[cell].thickness > Crust{}.thickness
                    ? 1
                    : 0;
        }
    }
    CHECK(beyond > 0);
    CHECK(lithosphere.plates()[0].crust[pit].thickness > Crust{}.thickness);
}

TEST_CASE("ground under the sea's level but cut off from the sea isn't sea",
          "[planet][lithosphere]") {
    // Land everywhere but the ocean round the south pole, with a hollow under the sea's level at
    // the north pole.
    const LevelGrid grid{3, radius};
    Lithosphere::Plate plate = everywhere(grid, continent, DVec3{0.0, 0.0, 1.0}, 0.0);
    std::vector<std::int64_t> heights(grid.size());
    std::uint32_t pit = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const double z = hog::planet::cell_center(grid.cell(cell)).z;
        if (z < -0.5) {
            plate.crust[cell] = Crust{};
        }
        heights[cell] = (z < -0.5 ? -4'000 : 1'000) * hog::planet::fixed_one;
        if (z > hog::planet::cell_center(grid.cell(pit)).z) {
            pit = cell;
        }
    }
    heights[pit] = -500 * hog::planet::fixed_one;
    std::vector<float> worn(grid.size(), 0.0F);
    for (const std::uint32_t next : grid.neighbors(pit)) {
        worn[next] = 1.0F;
    }
    Lithosphere lithosphere{grid, {plate}};
    lithosphere.erode(worn, heights, 0);
    // Worn round it, but none of it laid in it.
    CHECK(lithosphere.plates()[0].crust[pit].thickness == continent.thickness);
    double laid = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const Crust& crust = lithosphere.plates()[0].crust[cell];
        if (crust.type == CrustType::Oceanic) {
            laid += static_cast<double>(grid.area(cell)) *
                    static_cast<double>(crust.thickness - Crust{}.thickness);
        }
    }
    double off = 0.0;
    for (const std::uint32_t next : grid.neighbors(pit)) {
        off += static_cast<double>(grid.area(next));
    }
    CHECK_THAT(laid, WithinRel(off, 1e-5));
}

TEST_CASE("a continent's soft rock wears off before the rock under it, the softest first",
          "[planet][lithosphere]") {
    const LevelGrid grid{3, radius};
    Lithosphere::Plate plate = everywhere(grid, Crust{}, DVec3{0.0, 0.0, 1.0}, 0.0);
    std::vector<bool> drowned(grid.size(), true);
    Crust covered = continent;
    covered.cover = {0.5F, 0.5F};
    plate.crust[0] = covered;
    drowned[0] = false;
    Lithosphere lithosphere{grid, {plate}};
    REQUIRE(lithosphere.cover()[0] == Cover{0.5F, 0.5F});
    lithosphere.erode(std::vector<float>(grid.size(), 0.25F), ground(drowned), 0);
    CHECK(lithosphere.plates()[0].crust[0].cover == Cover{0.25F, 0.5F});
    lithosphere.erode(std::vector<float>(grid.size(), 0.5F), ground(drowned), 0);
    CHECK(lithosphere.plates()[0].crust[0].cover == Cover{0.0F, 0.25F});
    lithosphere.erode(std::vector<float>(grid.size(), 1.0F), ground(drowned), 0);
    CHECK(lithosphere.plates()[0].crust[0].cover == Cover{});
    CHECK(lithosphere.plates()[0].crust[0].thickness == continent.thickness - 1.75F);
}

TEST_CASE("sediment buried deep is pressed into rock, and deeper still baked as hard as the rest",
          "[planet][lithosphere]") {
    using hog::planet::hardened;
    using hog::planet::lithified;
    // A continent everywhere, all of it wearing down into one basin under the sea.
    const LevelGrid grid{3, radius};
    std::vector<bool> drowned(grid.size(), false);
    drowned[0] = true;
    Lithosphere lithosphere{grid, {everywhere(grid, continent, DVec3{0.0, 0.0, 1.0}, 0.0)}};
    lithosphere.erode(std::vector<float>(grid.size(), 0.5F), ground(drowned), 0);
    const Crust& basin = lithosphere.plates()[0].crust[0];
    CHECK(basin.thickness > continent.thickness + hardened);
    CHECK(basin.cover == Cover{lithified, hardened - lithified});
}

TEST_CASE("sediment buried under a range is baked as hard as the rock around it",
          "[planet][lithosphere]") {
    const Lithosphere bare = continental_halves();
    std::vector<Lithosphere::Plate> plates{bare.plates().begin(), bare.plates().end()};
    for (auto& plate : plates) {
        for (Crust& crust : plate.crust) {
            crust.cover = {1.0F, 3.0F};
        }
    }
    Lithosphere lithosphere{bare.grid(), plates};
    lithosphere.advance(20.0);
    lithosphere.collide();
    int piled = 0;
    for (const auto& plate : lithosphere.plates()) {
        for (const Crust& crust : plate.crust) {
            if (crust.thickness > continent.thickness) {
                ++piled;
                // Only what was thrust on top is left soft.
                CHECK(crust.cover[loose_sediment] + crust.cover[sedimentary_rock] < 6.0F);
            }
        }
    }
    CHECK(piled > 0);
}

TEST_CASE("ocean floor never piles up", "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    lithosphere.advance(20.0);
    const Lithosphere moved = lithosphere;
    lithosphere.collide();
    for (std::uint32_t plate = 0; plate < 2; ++plate) {
        CHECK(lithosphere.plates()[plate].cells == moved.plates()[plate].cells);
        CHECK(lithosphere.plates()[plate].crust == moved.plates()[plate].crust);
    }
}

TEST_CASE("no continent is lost as plates part and meet but what founders",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    const double volume = continental_volume(lithosphere);
    for (int step = 0; step < 10; ++step) {
        lithosphere.advance(step_years);
        // Arcs make new continent; without them none is made either.
        lithosphere.subduct(step_years, {.reach = 3, .growth = 0.0F});
        lithosphere.collide();
        lithosphere.spread();
    }
    CHECK_THAT(continental_volume(lithosphere) + lithosphere.foundered(), WithinRel(volume, 0.01));
}

TEST_CASE("a plate carries each place round its pole", "[planet][lithosphere]") {
    const Lithosphere lithosphere = halves();
    // The northern half turns about +x at 0.01 a million years, so the +y axis heads north.
    const DVec3 moving = lithosphere.velocity(0, DVec3{0.0, 1.0, 0.0});
    CHECK_THAT(glm::length(moving - DVec3{0.0, 0.0, 0.01}), WithinAbs(0.0, 1e-12));
    CHECK_THAT(glm::length(lithosphere.velocity(0, DVec3{1.0, 0.0, 0.0})), WithinAbs(0.0, 1e-12));
}

namespace {

using Kind = hog::planet::Boundary::Kind;

// Plates moved on and settled: the overlaps between them gone and the gaps filled.
Lithosphere settled(Lithosphere lithosphere, double years) {
    lithosphere.advance(years);
    lithosphere.subduct(years);
    lithosphere.collide();
    lithosphere.spread();
    return lithosphere;
}

} // namespace

TEST_CASE("plates part at ridges and close at trenches, either side of their edge",
          "[planet][lithosphere]") {
    const Lithosphere lithosphere = settled(halves(), 20.0);
    const auto& grid = lithosphere.grid();
    const std::vector<hog::planet::Boundary> found = lithosphere.boundaries();
    REQUIRE_FALSE(found.empty());
    // Both halves turn at 0.01 a million years, so they part and close at up to twice that.
    constexpr double most = 0.02;
    for (const auto& boundary : found) {
        const DVec3 at = hog::planet::cell_center(grid.cell(boundary.place));
        CHECK(boundary.other == 1 - boundary.plate);
        CHECK_THAT(glm::dot(boundary.normal, at), WithinAbs(0.0, 1e-9));
        CHECK_THAT(glm::length(boundary.normal), WithinAbs(1.0, 1e-9));
        // From the north toward the south, and back, as far as the edge's steps let it.
        CHECK(glm::dot(boundary.normal, DVec3{0.0, 0.0, boundary.plate == 0 ? -1.0 : 1.0}) > 0.0);
        CHECK(boundary.length > 0.0);
        CHECK(std::abs(boundary.closing) <= most * 1.01);
        // Away from the poles they turn about, they part around +y and close around -y.
        if (std::abs(at.x) > 0.5) {
            continue;
        }
        if (at.y > 0.0) {
            CHECK(boundary.kind == Kind::Ridge);
            CHECK(boundary.closing < -most * 0.5);
        } else {
            CHECK(boundary.closing > most * 0.5);
            // Floor as old either side: the lower plate's stays on top, and the other's dives.
            CHECK(boundary.kind == (boundary.plate == 0 ? Kind::Arc : Kind::Trench));
        }
    }
}

TEST_CASE("plates fault the crust along their edge as they move against each other there",
          "[planet][lithosphere]") {
    using hog::planet::Slip;
    Lithosphere lithosphere = settled(halves(), 20.0);
    const auto& grid = lithosphere.grid();
    const std::vector<hog::planet::Boundary> edges = lithosphere.boundaries();
    lithosphere.drive();
    int marked = 0;
    for (const auto& edge : edges) {
        const DVec3 at = hog::planet::cell_center(grid.cell(edge.place));
        const std::optional<Crust> crust = lithosphere.crust_of(edge.plate, at);
        REQUIRE(crust.has_value());
        CHECK(crust.value_or(Crust{}).faulted == 0.0F);
        // Away from the poles they turn about, pulled apart around +y and pushed together around
        // -y.
        if (std::abs(at.x) < 0.5) {
            CHECK(crust.value_or(Crust{}).fault == (at.y > 0.0 ? Slip::Normal : Slip::Reverse));
            ++marked;
        }
    }
    CHECK(marked > 0);
    // Nowhere near an edge, no fault.
    CHECK(lithosphere.top(DVec3{0.0, 0.0, 1.0}).value_or(Lithosphere::Found{}).crust.fault ==
          Slip::None);

    // Turning about the axis through their middles, they slide past each other all along it.
    const LevelGrid sliding_grid{4, radius};
    const DVec3 up{0.0, 0.0, 1.0};
    const Lithosphere::Plate north =
        northern(sliding_grid, everywhere(sliding_grid, Crust{}, up, 0.01));
    Lithosphere::Plate south = northern(sliding_grid, everywhere(sliding_grid, Crust{}, up, -0.01));
    south.frame = glm::angleAxis(std::numbers::pi, DVec3{1.0, 0.0, 0.0});
    Lithosphere sliding{sliding_grid, {north, south}};
    const std::vector<hog::planet::Boundary> slid = sliding.boundaries();
    REQUIRE_FALSE(slid.empty());
    sliding.drive();
    for (const auto& edge : slid) {
        const DVec3 at = hog::planet::cell_center(sliding_grid.cell(edge.place));
        CHECK(sliding.crust_of(edge.plate, at).value_or(Crust{}).fault == Slip::Strike);
    }
}

TEST_CASE("faults keep how they slipped once the plates move on, each somewhere in its cycle",
          "[planet][lithosphere]") {
    using hog::planet::Fault;
    using hog::planet::Slip;
    Lithosphere lithosphere = settled(halves(), 20.0);
    lithosphere.drive();
    constexpr double years = 20.0;
    lithosphere.advance(years);
    const std::vector<Fault> found = lithosphere.faults(3);
    REQUIRE_FALSE(found.empty());
    CHECK(lithosphere.faults(3) == found);
    CHECK(lithosphere.faults(4) != found);
    int moving = 0;
    int left = 0;
    for (const Fault& fault : found) {
        CHECK(fault.slip != Slip::None);
        CHECK(fault.stress >= 0.0F);
        CHECK(fault.stress < 1.0F);
        if (fault.rate > 0.0) {
            ++moving;
            CHECK(fault.idle == 0.0F);
        } else {
            ++left;
            CHECK_THAT(static_cast<double>(fault.idle), WithinAbs(years, 1e-4));
        }
    }
    // Still moving along the edge, and left behind as the floor pulled apart moved off the ridge.
    CHECK(moving > 0);
    CHECK(left > 0);
}

TEST_CASE("continents closing on each other collide", "[planet][lithosphere]") {
    const Lithosphere lithosphere = settled(continental_halves(), 20.0);
    int collisions = 0;
    for (const auto& boundary : lithosphere.boundaries()) {
        // Away from the poles they turn about, where new floor parts them.
        const DVec3 at = hog::planet::cell_center(lithosphere.grid().cell(boundary.place));
        if (boundary.closing > 0.0 && std::abs(at.x) < 0.5) {
            CHECK(boundary.kind == Kind::Collision);
            ++collisions;
        }
    }
    CHECK(collisions > 0);
}

namespace {

DVec3 turn_of(const Lithosphere& lithosphere, std::uint32_t plate) {
    const auto& carrier = lithosphere.plates()[plate];
    return carrier.rate * carrier.pole;
}

// How fast the crust moves on the whole: the root of its mean square speed over the sphere.
double pace(const Lithosphere& lithosphere) {
    const auto& grid = lithosphere.grid();
    double sum = 0.0;
    double sphere = 0.0;
    for (std::uint32_t plate = 0; plate < lithosphere.plates().size(); ++plate) {
        const auto& carrier = lithosphere.plates()[plate];
        for (const std::uint32_t cell : carrier.cells) {
            const DVec3 at =
                lithosphere.to_planet(plate, hog::planet::cell_center(grid.cell(cell)));
            const double speed = glm::length(lithosphere.velocity(plate, at));
            sum += static_cast<double>(grid.area(cell)) * speed * speed;
        }
    }
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        sphere += static_cast<double>(grid.area(cell));
    }
    return std::sqrt(sum / sphere);
}

// The halves, settled, with the southern one's floor aged to `age` and, if `keel`, continent
// around its pole, far from either edge.
Lithosphere aged_halves(float age, bool keel) {
    const Lithosphere moved = settled(halves(), 20.0);
    std::vector<Lithosphere::Plate> plates{moved.plates().begin(), moved.plates().end()};
    const auto& grid = moved.grid();
    for (std::size_t index = 0; index < plates[1].cells.size(); ++index) {
        Crust& crust = plates[1].crust[index];
        crust.age = age;
        const DVec3 at =
            moved.to_planet(1, hog::planet::cell_center(grid.cell(plates[1].cells[index])));
        if (keel && at.z < -0.6) {
            crust = continent;
        }
    }
    return Lithosphere{grid, plates};
}

} // namespace

TEST_CASE("slabs pull plates toward their trenches and ridges push them apart",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = aged_halves(100.0F, false);
    const double before = pace(lithosphere);
    lithosphere.drive();
    // Both still turn about +x as the halves part and close, the south pulled by its slab too.
    const DVec3 north = turn_of(lithosphere, 0);
    const DVec3 south = turn_of(lithosphere, 1);
    CHECK(glm::dot(glm::normalize(north), DVec3{1.0, 0.0, 0.0}) > 0.9);
    CHECK(glm::dot(glm::normalize(south), DVec3{-1.0, 0.0, 0.0}) > 0.9);
    CHECK(glm::length(south) > glm::length(north));
    // At the pace they had.
    CHECK_THAT(pace(lithosphere), WithinRel(before, 1e-6));
}

TEST_CASE("older slabs pull harder", "[planet][lithosphere]") {
    const auto lead = [](float age) {
        Lithosphere lithosphere = aged_halves(age, false);
        lithosphere.drive();
        return glm::length(turn_of(lithosphere, 1)) / glm::length(turn_of(lithosphere, 0));
    };
    CHECK(lead(10.0F) < lead(50.0F));
    // No harder once the floor is done cooling.
    CHECK_THAT(lead(100.0F), WithinRel(lead(200.0F), 1e-9));
}

TEST_CASE("a slab pulls no harder than it holds before breaking off", "[planet][lithosphere]") {
    const auto lead = [](Strength strength, double hotter) {
        Lithosphere lithosphere = aged_halves(100.0F, false);
        lithosphere.warm(10.0, {.hotter = hotter});
        lithosphere.drive({}, strength);
        return glm::length(turn_of(lithosphere, 1)) / glm::length(turn_of(lithosphere, 0));
    };
    // Today's old slab holds more than it pulls.
    CHECK_THAT(lead({.ocean = 1.0}, 0.0), WithinRel(lead({}, 0.0), 1e-9));
    CHECK(lead({.ocean = 0.5}, 0.0) < lead({}, 0.0));
    // Over the young mantle it's softer, so it breaks off sooner.
    CHECK(lead({}, 200.0) < lead({}, 0.0));
}

TEST_CASE("a slab pulls as long as floor goes down it, not by all that has",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = aged_halves(100.0F, false);
    lithosphere.drive();
    const double first = glm::length(turn_of(lithosphere, 1));
    for (int step = 0; step < 3; ++step) {
        lithosphere = settled(lithosphere, 10.0);
        lithosphere.drive();
    }
    CHECK_THAT(glm::length(turn_of(lithosphere, 1)), WithinRel(first, 0.2));
}

TEST_CASE("a slab hangs on, pulling, after its plates weld, until it breaks off",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = aged_halves(100.0F, false);
    // So short a while that the halves move as one.
    lithosphere.weld(1e-9);
    REQUIRE(lithosphere.plates().size() == 1);
    lithosphere.drive();
    CHECK(lithosphere.plates()[0].rate > 0.0);
    lithosphere.advance(Lithosphere::breakoff * 0.5);
    lithosphere.drive();
    CHECK(lithosphere.plates()[0].rate > 0.0);
    lithosphere.advance(Lithosphere::breakoff);
    lithosphere.drive();
    CHECK(lithosphere.plates()[0].rate == 0.0);
}

TEST_CASE("plates closing on each other drag on each other, hardest where continents collide",
          "[planet][lithosphere]") {
    // How far the plates turn against each other, of how far they turn on the whole.
    const auto apart = [](const Lithosphere& lithosphere) {
        const DVec3 north = turn_of(lithosphere, 0);
        const DVec3 south = turn_of(lithosphere, 1);
        return glm::length(north - south) / (glm::length(north) + glm::length(south));
    };
    const auto trench = [&](Forces forces) {
        Lithosphere lithosphere = aged_halves(100.0F, false);
        lithosphere.drive(forces);
        return apart(lithosphere);
    };
    CHECK(trench({}) < trench({.megathrust = 0.0}));
    // A strip of thick continent along where the halves meet, closing on itself.
    const auto collision = [&](Forces forces) {
        const Lithosphere ocean = halves();
        std::vector<Lithosphere::Plate> plates{ocean.plates().begin(), ocean.plates().end()};
        const auto& grid = ocean.grid();
        for (std::uint32_t plate = 0; plate < 2; ++plate) {
            for (std::size_t index = 0; index < plates[plate].cells.size(); ++index) {
                const DVec3 at = ocean.to_planet(
                    plate, hog::planet::cell_center(grid.cell(plates[plate].cells[index])));
                if (std::abs(at.z) < 0.15) {
                    plates[plate].crust[index] = continent;
                }
            }
        }
        Lithosphere lithosphere = settled(Lithosphere{grid, plates}, 20.0);
        lithosphere.drive(forces);
        return apart(lithosphere);
    };
    const double unbound = collision({.megathrust = 0.0, .suture = 0.0});
    CHECK(collision({.megathrust = 0.0}) < unbound);
    CHECK(collision({.megathrust = 0.0, .suture = 0.3}) > collision({.megathrust = 0.0}));
}

TEST_CASE("the mantle drags harder on plates carrying continent", "[planet][lithosphere]") {
    const auto lead = [](bool keel) {
        Lithosphere lithosphere = aged_halves(100.0F, keel);
        lithosphere.drive();
        return glm::length(turn_of(lithosphere, 1)) / glm::length(turn_of(lithosphere, 0));
    };
    CHECK(lead(true) < lead(false));
    const auto without = [] {
        Lithosphere lithosphere = aged_halves(100.0F, true);
        lithosphere.drive({.keel = 1.0});
        return glm::length(turn_of(lithosphere, 1)) / glm::length(turn_of(lithosphere, 0));
    };
    CHECK_THAT(without(), WithinRel(lead(false), 1e-6));
}

TEST_CASE("the mantle drags on a place as it's carried and as it spins", "[planet][lithosphere]") {
    const DVec3 at = glm::normalize(DVec3{1.0, 2.0, 3.0});
    const DVec3 along = glm::normalize(glm::cross(at, DVec3{0.0, 0.0, 1.0}));
    constexpr double share = 1e-3;
    const glm::dmat3 drag = hog::planet::place_drag(at, share);
    // Carried along at unit speed, as its share of the sphere.
    CHECK_THAT(glm::dot(along, drag * along), WithinRel(share, 1e-12));
    // Spun about its middle, as a disc of 4 share square radians, by its radius squared over two.
    CHECK_THAT(glm::dot(at, drag * at), WithinRel(share * 2.0 * share, 1e-12));
    CHECK(glm::determinant(drag) > 0.0);
}

TEST_CASE("plates slide off the dome a warm mantle lifts", "[planet][lithosphere]") {
    // The halves slide past each other about z, so their edges neither pull nor push, with a
    // continent across their edge around -y warming the mantle under it.
    const Lithosphere ocean = halves();
    std::vector<Lithosphere::Plate> plates{ocean.plates().begin(), ocean.plates().end()};
    const auto& grid = ocean.grid();
    for (std::uint32_t plate = 0; plate < 2; ++plate) {
        plates[plate].pole = DVec3{0.0, 0.0, 1.0};
        plates[plate].rate = plate == 0 ? 0.01 : -0.01;
        for (std::size_t index = 0; index < plates[plate].cells.size(); ++index) {
            const DVec3 at = ocean.to_planet(
                plate, hog::planet::cell_center(grid.cell(plates[plate].cells[index])));
            if (at.y < -0.8) {
                plates[plate].crust[index] = continent;
            }
        }
    }
    Lithosphere lithosphere{grid, plates};
    lithosphere.warm(1000.0);
    lithosphere.drive();
    // The north slides off it northward and the south southward, so they part over it.
    CHECK(glm::dot(glm::normalize(turn_of(lithosphere, 0)), DVec3{-1.0, 0.0, 0.0}) > 0.9);
    CHECK(glm::dot(glm::normalize(turn_of(lithosphere, 1)), DVec3{1.0, 0.0, 0.0}) > 0.9);
}

TEST_CASE("plates keep the pace they were first driven at", "[planet][lithosphere]") {
    Lithosphere lithosphere =
        settled(hog::planet::lithosphere_from(start(), step_years), step_years);
    const double before = pace(lithosphere);
    lithosphere.drive();
    CHECK_THAT(pace(lithosphere), WithinRel(before, 1e-6));
    // Twice the pace stays twice once driven, whatever the forces then do.
    std::vector<Lithosphere::Plate> plates{lithosphere.plates().begin(),
                                           lithosphere.plates().end()};
    Lithosphere twice{lithosphere.grid(), plates};
    for (auto& plate : plates) {
        plate.rate *= 2.0;
    }
    Lithosphere doubled{lithosphere.grid(), plates};
    twice.drive();
    doubled.drive();
    for (int step = 0; step < 3; ++step) {
        twice = settled(twice, step_years);
        twice.drive();
        doubled = settled(doubled, step_years);
        doubled.drive();
    }
    CHECK(pace(doubled) > pace(twice) * 1.5);
}

TEST_CASE("a range thick enough pushes the continents building it apart", "[planet][lithosphere]") {
    const Lithosphere moved = settled(continental_halves(), 20.0);
    // Where the halves close, away from the poles they turn about.
    const auto closing = [](const Lithosphere& lithosphere) {
        double sum = 0.0;
        for (const auto& boundary : lithosphere.boundaries()) {
            const DVec3 at = hog::planet::cell_center(lithosphere.grid().cell(boundary.place));
            if (at.y < -0.5 && std::abs(at.x) < 0.5) {
                sum += boundary.closing;
            }
        }
        return sum;
    };
    REQUIRE(closing(moved) > 0.0);
    const auto driven = [&](float thickness) {
        std::vector<Lithosphere::Plate> plates{moved.plates().begin(), moved.plates().end()};
        for (auto& plate : plates) {
            for (Crust& crust : plate.crust) {
                if (crust.type == CrustType::Continental) {
                    crust.thickness = thickness;
                }
            }
        }
        Lithosphere lithosphere{moved.grid(), plates};
        lithosphere.drive();
        return closing(lithosphere);
    };
    // Settled continent pushes nothing back, so the ridges keep closing them.
    CHECK(driven(continent.thickness) > 0.0);
    CHECK(driven(100.0F) < 0.0);
}

namespace {

// The halves turning together, as one.
Lithosphere together(const Lithosphere& lithosphere) {
    std::vector<Lithosphere::Plate> plates{lithosphere.plates().begin(),
                                           lithosphere.plates().end()};
    plates[1].pole = plates[0].pole;
    plates[1].rate = plates[0].rate;
    return Lithosphere{lithosphere.grid(), plates};
}

} // namespace

TEST_CASE("continents that have collided and move as one weld into one plate",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = together(settled(continental_halves(), 20.0));
    lithosphere.advance(20.0);
    const double volume = continental_volume(lithosphere);
    lithosphere.weld(10.0);
    CHECK(lithosphere.plates().size() == 1);
    CHECK_THAT(continental_volume(lithosphere), WithinRel(volume, 0.03));
    // What the frames left between them fills as any gap does.
    lithosphere.spread();
    const auto& grid = lithosphere.grid();
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const auto found = lithosphere.top(hog::planet::cell_center(grid.cell(cell)));
        REQUIRE(found.has_value());
        CHECK(found.value_or(Lithosphere::Found{}).plate == 0);
    }
}

TEST_CASE("a small continent run into a bigger one is sutured onto it as a terrane",
          "[planet][lithosphere]") {
    // A cap of crust around +z, of `reach` radians, turning toward -y into a plate still around it.
    const auto cap = [](double reach, Crust crust) {
        const LevelGrid grid{4, radius};
        Lithosphere::Plate around = everywhere(grid, continent, DVec3{1.0, 0.0, 0.0}, 0.0);
        Lithosphere::Plate moving = everywhere(grid, crust, DVec3{1.0, 0.0, 0.0}, 0.01);
        moving.cells.clear();
        moving.crust.clear();
        around.cells.clear();
        around.crust.clear();
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            const bool inside = hog::planet::cell_center(grid.cell(cell)).z > std::cos(reach);
            (inside ? moving : around).cells.push_back(cell);
            (inside ? moving.crust : around.crust).push_back(inside ? crust : continent);
        }
        Lithosphere lithosphere{grid, {around, moving}};
        lithosphere.weld(step_years);
        return lithosphere.plates().size();
    };
    CHECK(cap(0.3, continent) == 1);
    // Big enough to hold its own course, or ocean floor diving under the continent around it.
    CHECK(cap(1.2, continent) == 2);
    CHECK(cap(0.3, Crust{}) == 2);
}

TEST_CASE("plates moving as one weld into one plate whatever crust they meet with",
          "[planet][lithosphere]") {
    Lithosphere ocean = together(halves());
    ocean.advance(20.0);
    ocean.weld(10.0);
    CHECK(ocean.plates().size() == 1);
    Lithosphere touching = together(continental_halves());
    touching.advance(20.0);
    touching.weld(10.0);
    CHECK(touching.plates().size() == 1);
}

TEST_CASE("continents piling up more than they can take spread out, and their plates stay apart",
          "[planet][lithosphere]") {
    // A strip of thick continent along where the halves meet, with only ocean floor beyond.
    const Lithosphere ocean = halves();
    std::vector<Lithosphere::Plate> plates{ocean.plates().begin(), ocean.plates().end()};
    const auto& grid = ocean.grid();
    for (std::uint32_t plate = 0; plate < 2; ++plate) {
        for (std::size_t index = 0; index < plates[plate].cells.size(); ++index) {
            const DVec3 at = ocean.to_planet(
                plate, hog::planet::cell_center(grid.cell(plates[plate].cells[index])));
            if (std::abs(at.z) < 0.15) {
                plates[plate].crust[index] = continent;
                plates[plate].crust[index].thickness = 60.0F;
            }
        }
    }
    Lithosphere jammed = settled(Lithosphere{grid, plates}, 20.0);
    CHECK(jammed.foundered() == 0.0);
    jammed.weld(10.0);
    CHECK(jammed.plates().size() == 2);
}

TEST_CASE("plates still moving, at sea or where their continents never collided, stay apart",
          "[planet][lithosphere]") {
    Lithosphere ocean = settled(halves(), 20.0);
    ocean.weld(10.0);
    CHECK(ocean.plates().size() == 2);
    Lithosphere closing = settled(continental_halves(), 20.0);
    closing.weld(10.0);
    CHECK(closing.plates().size() == 2);

    // Continents touching about the pole the halves turn on, where they barely move against each
    // other, while the floor further along their edge parts and closes.
    const Lithosphere slow = halves();
    std::vector<Lithosphere::Plate> plates{slow.plates().begin(), slow.plates().end()};
    const auto& grid = slow.grid();
    for (std::uint32_t plate = 0; plate < 2; ++plate) {
        for (std::size_t index = 0; index < plates[plate].cells.size(); ++index) {
            const DVec3 at = slow.to_planet(
                plate, hog::planet::cell_center(grid.cell(plates[plate].cells[index])));
            if (at.x > 0.98) {
                plates[plate].crust[index] = continent;
            }
        }
    }
    Lithosphere touching{grid, plates};
    touching.weld(10.0);
    CHECK(touching.plates().size() == 2);
}

namespace {

// Shells a kilometre thick.
constexpr int shell_level = 8;

Volume filled(const Lithosphere& lithosphere) {
    Volume volume{hog::planet::Contents{}, 2};
    hog::planet::fill_lithosphere(volume, lithosphere, radius, lithosphere.grid().level(),
                                  shell_level);
    volume.settle();
    return volume;
}

Material material_at(const Volume& volume, DVec3 direction, double height) {
    const hog::WorldPosition position{direction * (radius + height)};
    const auto leaf = hog::planet::leaf_at(volume, radius, position);
    REQUIRE(leaf.has_value());
    return hog::planet::material_at(leaf.value_or(Volume::Leaf{}), radius, position);
}

} // namespace

TEST_CASE("the mantle warms under continents and cools under ocean floor",
          "[planet][lithosphere]") {
    // The north is continent and the south ocean, both turning together half round in 100 Myr.
    const Lithosphere ocean = halves();
    std::vector<Lithosphere::Plate> plates{ocean.plates().begin(), ocean.plates().end()};
    std::ranges::fill(plates[0].crust, continent);
    for (auto& plate : plates) {
        plate.rate = std::numbers::pi / 100.0;
    }
    Lithosphere lithosphere{ocean.grid(), plates};
    const auto& grid = lithosphere.grid();
    const auto heat_at = [&](DVec3 at) {
        return lithosphere.heat()[grid.index(hog::planet::cell_at(at, grid.level()))];
    };
    const DVec3 north{0.0, 0.0, 1.0};
    const DVec3 south{0.0, 0.0, -1.0};
    CHECK(heat_at(north) == 0.0F);

    constexpr Mantle mantle{.settling = 100.0, .hotter = 0.0};
    lithosphere.warm(50.0, mantle);
    lithosphere.warm(50.0, mantle);
    CHECK_THAT(heat_at(north), WithinAbs(1.0 - std::exp(-1.0), 1e-5));
    CHECK(heat_at(south) == 0.0F);
    lithosphere.warm(1000.0, mantle);
    CHECK_THAT(heat_at(north), WithinAbs(1.0, 1e-4));

    // Once the continent has moved on, the mantle it warmed cools as fast.
    lithosphere.advance(100.0);
    lithosphere.warm(100.0, mantle);
    CHECK_THAT(heat_at(north), WithinAbs(std::exp(-1.0), 1e-5));
    CHECK_THAT(heat_at(south), WithinAbs(1.0 - std::exp(-1.0), 1e-5));
}

TEST_CASE("the young mantle softens the lithosphere less as it cools", "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    CHECK(lithosphere.softer() == 1.0);
    constexpr Mantle mantle{.hotter = 200.0, .cooling = 0.1};
    lithosphere.warm(10.0, mantle);
    // Creep with 300 kJ/mol, 200 K over 1623 K.
    const double young = lithosphere.softer();
    CHECK_THAT(young, WithinRel(std::exp(300e3 / 8.314 * ((1.0 / 1623.0) - (1.0 / 1823.0))), 1e-9));
    lithosphere.advance(1000.0);
    lithosphere.warm(10.0, mantle);
    CHECK(lithosphere.softer() > 1.0);
    CHECK(lithosphere.softer() < young);
    lithosphere.advance(1000.0);
    lithosphere.warm(10.0, mantle);
    CHECK(lithosphere.softer() == 1.0);
}

namespace {

// The northern half alone, ocean floor `age` million years old but for a continent around its
// pole, warmed under it for good over a mantle `hotter` than today's.
Lithosphere warm_cap(double hotter = 0.0, float age = 100.0F) {
    const Lithosphere ocean = halves();
    std::vector<Lithosphere::Plate> plates{ocean.plates().front()};
    const auto& grid = ocean.grid();
    for (std::size_t index = 0; index < plates[0].cells.size(); ++index) {
        plates[0].crust[index].age = age;
        if (hog::planet::cell_center(grid.cell(plates[0].cells[index])).z > 0.7) {
            plates[0].crust[index] = continent;
        }
    }
    Lithosphere lithosphere{grid, plates};
    lithosphere.warm(1000.0, {.hotter = hotter});
    return lithosphere;
}

constexpr Strength weak{.cool = 0.02, .warm = 0.01, .ocean = 0.02};

// Million years, long enough for any rift pulled apart to break.
constexpr double long_enough = 1e4;

// The plates driven, then rifted for as long as any rift pulled apart takes to break.
void rifted(Lithosphere& lithosphere, Strength strength = {}) {
    lithosphere.drive({}, strength);
    lithosphere.rift(long_enough, {}, strength);
}

} // namespace

TEST_CASE("a plate splits over a warm mantle only where pulled harder than it holds",
          "[planet][lithosphere]") {
    Lithosphere strong = warm_cap();
    rifted(strong);
    CHECK(strong.plates().size() == 1);

    // Cool, the mantle pushes nothing, however weak the plate.
    const Lithosphere warmed = warm_cap();
    Lithosphere cool{warmed.grid(), {warmed.plates().begin(), warmed.plates().end()}};
    rifted(cool, weak);
    CHECK(cool.plates().size() == 1);

    Lithosphere split = warm_cap();
    split.drive({}, weak);
    const Lithosphere::Plate whole = split.plates().front();
    split.rift(long_enough, {}, weak);
    REQUIRE(split.plates().size() == 2);
    // Each part keeps its crust where it was, turning as the whole did, and some continent.
    std::vector<std::uint32_t> cells;
    for (const auto& plate : split.plates()) {
        CHECK(plate.frame == whole.frame);
        CHECK(plate.pole == whole.pole);
        CHECK(plate.rate == whole.rate);
        CHECK(std::ranges::count(plate.crust, continent) > 0);
        cells.insert(cells.end(), plate.cells.begin(), plate.cells.end());
    }
    std::ranges::sort(cells);
    CHECK(cells == whole.cells);
    // Off the dome, the parts part.
    split.drive();
    int parting = 0;
    int closing = 0;
    for (const auto& boundary : split.boundaries()) {
        (boundary.closing < 0.0 ? parting : closing) += 1;
    }
    CHECK(parting > 0);
    CHECK(closing == 0);
}

TEST_CASE("a plate covering the planet splits only where pulled harder than it holds",
          "[planet][lithosphere]") {
    // Any line across it runs all the way round.
    const auto covering = [] {
        Lithosphere lithosphere = aged_halves(100.0F, true);
        lithosphere.weld(1e-9);
        REQUIRE(lithosphere.plates().size() == 1);
        lithosphere.warm(1000.0);
        lithosphere.drive();
        return lithosphere;
    };
    Lithosphere strong = covering();
    strong.rift(long_enough);
    CHECK(strong.plates().size() == 1);
    Lithosphere split = covering();
    split.rift(long_enough, {}, weak);
    CHECK(split.plates().size() == 2);
}

TEST_CASE("young ocean floor, as thin as it is, holds less than old", "[planet][lithosphere]") {
    constexpr Strength middling{.cool = 0.2, .warm = 0.1, .ocean = 0.2};
    Lithosphere old_floor = warm_cap();
    rifted(old_floor, middling);
    CHECK(old_floor.plates().size() == 1);
    Lithosphere young_floor = warm_cap(0.0, 1.0F);
    rifted(young_floor, middling);
    CHECK(young_floor.plates().size() == 2);
}

TEST_CASE("a rift breaks once it has opened far enough, and fails once no longer pulled apart",
          "[planet][lithosphere]") {
    constexpr int never = 1'000;
    // Steps of `years` a rift is pulled apart until it breaks, but for one holding fast after
    // `held` of them.
    const auto steps = [](double years, int held) {
        Lithosphere lithosphere = warm_cap();
        lithosphere.drive({}, weak);
        for (int step = 1; step < never; ++step) {
            lithosphere.rift(years, {}, step == held + 1 ? Strength{} : weak);
            if (lithosphere.plates().size() > 1) {
                return step;
            }
        }
        return never;
    };
    // Steps short enough that it takes a few.
    double years = 1.0;
    int straight = 1;
    while (straight < 4 && years > 1e-30) {
        years /= 10.0;
        straight = steps(years, never);
    }
    REQUIRE(straight >= 4);
    REQUIRE(straight < never);
    // A failed rift starts again from nothing.
    CHECK(steps(years, straight / 2) == straight + (straight / 2) + 1);
}

TEST_CASE("a plate that holds over today's mantle splits over a young hotter one",
          "[planet][lithosphere]") {
    constexpr Strength middling{.cool = 0.2, .warm = 0.1, .ocean = 0.2};
    Lithosphere today = warm_cap();
    rifted(today, middling);
    CHECK(today.plates().size() == 1);
    Lithosphere young = warm_cap(200.0);
    rifted(young, middling);
    CHECK(young.plates().size() == 2);
}

TEST_CASE("each plate's crust is found wherever it lies, through splits and welds",
          "[planet][lithosphere]") {
    Lithosphere lithosphere =
        settled(hog::planet::lithosphere_from(start(), step_years), step_years);
    lithosphere.drive();
    bool split = false;
    for (int step = 0; step < 4; ++step) {
        lithosphere = settled(lithosphere, step_years);
        lithosphere.weld(step_years);
        lithosphere.warm(step_years);
        const std::size_t before = lithosphere.plates().size();
        lithosphere.rift(long_enough, {}, weak);
        split = split || lithosphere.plates().size() > before;
        lithosphere.drive();
    }
    REQUIRE(split);
    const auto& grid = lithosphere.grid();
    for (std::uint32_t plate = 0; plate < lithosphere.plates().size(); ++plate) {
        const auto& cells = lithosphere.plates()[plate].cells;
        for (std::uint32_t place = 0; place < grid.size(); ++place) {
            const DVec3 at = hog::planet::cell_center(grid.cell(place));
            const std::uint32_t own =
                grid.index(hog::planet::cell_at(lithosphere.to_plate(plate, at), grid.level()));
            const bool held = std::ranges::binary_search(cells, own);
            REQUIRE(lithosphere.crust_of(plate, at).has_value() == held);
        }
    }
}

TEST_CASE("plates come out the same however many threads share the work", "[planet][lithosphere]") {
    hog::jobs::JobSystem jobs{4};
    const auto run = [](hog::jobs::JobSystem* shared) {
        Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
        lithosphere.share_work(shared);
        for (int step = 0; step < 4; ++step) {
            lithosphere.advance(step_years);
            lithosphere.subduct(step_years);
            lithosphere.collide();
            lithosphere.spread();
            lithosphere.weld(step_years);
            lithosphere.warm(step_years);
            lithosphere.rift(long_enough, {}, weak);
            lithosphere.drive();
        }
        lithosphere.share_work(nullptr);
        return lithosphere;
    };
    const Lithosphere alone = run(nullptr);
    const Lithosphere shared = run(&jobs);
    REQUIRE(alone.plates().size() == shared.plates().size());
    for (std::size_t plate = 0; plate < alone.plates().size(); ++plate) {
        const auto& one = alone.plates()[plate];
        const auto& other = shared.plates()[plate];
        CHECK(one.cells == other.cells);
        CHECK(one.crust == other.crust);
        CHECK(one.frame == other.frame);
        CHECK(one.pole == other.pole);
        CHECK(one.rate == other.rate);
    }
    CHECK(std::ranges::equal(alone.heat(), shared.heat()));
}

TEST_CASE("the volume's rock is the crust on top over the mantle", "[planet][lithosphere]") {
    Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    lithosphere.advance(3.0 * step_years);
    const Volume volume = filled(lithosphere);
    const auto& grid = lithosphere.grid();
    int parted = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); cell += 5) {
        const DVec3 at = hog::planet::cell_center(grid.cell(cell));
        CHECK(material_at(volume, at, 300.0) == Material::Air);
        const auto found = lithosphere.top(at);
        if (!found) {
            ++parted;
            CHECK(material_at(volume, at, -300.0) == Material::Mantle);
            continue;
        }
        const Crust crust = found.value_or(Lithosphere::Found{}).crust;
        CHECK(material_at(volume, at, -300.0) ==
              (crust.type == CrustType::Continental ? Material::Granite : Material::Basalt));
        const double base = static_cast<double>(crust.thickness) * 1'000.0;
        CHECK(material_at(volume, at, -base - 2'000.0) == Material::Mantle);
    }
    // Plates turning apart without new floor between them leave gaps.
    CHECK(parted > 0);
}

TEST_CASE("the volume holds as much crust as the plates carry", "[planet][lithosphere]") {
    const Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    const auto counted = hog::planet::budget(filled(lithosphere), radius);
    const auto& grid = lithosphere.grid();
    double granite = 0.0;
    double basalt = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const Crust crust = lithosphere.top(hog::planet::cell_center(grid.cell(cell)))
                                .value_or(Lithosphere::Found{})
                                .crust;
        // The column's share of the sphere, times its shell from the base of the crust up.
        const double share = static_cast<double>(grid.area(cell)) / (radius * radius);
        const double base = radius - (static_cast<double>(crust.thickness) * 1'000.0);
        const double rock = share * (std::pow(radius, 3.0) - std::pow(base, 3.0)) / 3.0;
        (crust.type == CrustType::Continental ? granite : basalt) += rock;
    }
    CHECK_THAT(counted.of(Material::Granite), WithinRel(granite, 0.03));
    CHECK_THAT(counted.of(Material::Basalt), WithinRel(basalt, 0.03));
    const double ball = 4.0 / 3.0 * std::numbers::pi * std::pow(radius, 3.0);
    CHECK_THAT(counted.of(Material::Granite) + counted.of(Material::Basalt) +
                   counted.of(Material::Mantle),
               WithinRel(ball, 0.02));
}

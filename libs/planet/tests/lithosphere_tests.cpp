#include <hog/planet/lithosphere.hpp>
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
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::planet::Crust;
using hog::planet::CrustType;
using hog::planet::LevelGrid;
using hog::planet::Lithosphere;
using hog::planet::Material;
using hog::planet::Ranges;
using hog::planet::Tectonics;
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
    // Strong enough to hold all that piles up, and stiff, so it stays where it piled.
    lithosphere.collide(years, {.strongest = 1'000.0F, .flow = 0.0F});

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
    CHECK(thickest(lithosphere) >= 2.0F * continent.thickness * 0.9F);
}

TEST_CASE("crust piled too thick flows out into the thinner continent around it",
          "[planet][lithosphere]") {
    Lithosphere lithosphere = continental_halves();
    lithosphere.advance(20.0);
    const double volume = continental_volume(lithosphere);
    const hog::planet::Ranges stiff{.flow = 0.0F};
    lithosphere.collide(20.0, stiff);
    CHECK_THAT(continental_volume(lithosphere), WithinRel(volume, 1e-4));
    CHECK(thickest(lithosphere) <= stiff.strongest + 1e-3F);
    CHECK(thickest(lithosphere) > continent.thickness);
}

TEST_CASE("a range spreads into the continent around it", "[planet][lithosphere]") {
    const LevelGrid grid{4, radius};
    Lithosphere::Plate plate = everywhere(grid, continent, DVec3{0.0, 0.0, 1.0}, 0.0);
    constexpr std::size_t peak = 1'000;
    plate.crust[peak].thickness = 55.0F;
    const auto settle = [&](float flow) {
        Lithosphere lithosphere{grid, {plate}};
        const double volume = continental_volume(lithosphere);
        lithosphere.collide(10.0, {.flow = flow});
        CHECK_THAT(continental_volume(lithosphere), WithinRel(volume, 1e-5));
        return lithosphere;
    };
    const Lithosphere stiff = settle(0.0F);
    CHECK(stiff.plates()[0].crust == plate.crust);

    const Lithosphere settled = settle(Ranges{}.flow);
    const auto& crust = settled.plates()[0].crust;
    CHECK(crust[peak].thickness < 55.0F);
    CHECK(crust[peak].thickness > continent.thickness);
    for (const std::uint32_t next : grid.neighbors(peak)) {
        CHECK(crust[next].thickness > continent.thickness);
        CHECK(crust[next].thickness < crust[peak].thickness);
    }
}

TEST_CASE("ocean floor never piles up", "[planet][lithosphere]") {
    Lithosphere lithosphere = halves();
    lithosphere.advance(20.0);
    const Lithosphere moved = lithosphere;
    lithosphere.collide(20.0);
    for (std::uint32_t plate = 0; plate < 2; ++plate) {
        CHECK(lithosphere.plates()[plate].cells == moved.plates()[plate].cells);
        CHECK(lithosphere.plates()[plate].crust == moved.plates()[plate].crust);
    }
}

TEST_CASE("no continent is lost as plates part and meet", "[planet][lithosphere]") {
    Lithosphere lithosphere = hog::planet::lithosphere_from(start(), step_years);
    const double volume = continental_volume(lithosphere);
    for (int step = 0; step < 10; ++step) {
        lithosphere.advance(step_years);
        // Arcs make new continent; without them none is made either.
        lithosphere.subduct(step_years, {.reach = 3, .growth = 0.0F});
        lithosphere.collide(step_years);
        lithosphere.spread();
    }
    CHECK_THAT(continental_volume(lithosphere), WithinRel(volume, 0.01));
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
    lithosphere.collide(years);
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

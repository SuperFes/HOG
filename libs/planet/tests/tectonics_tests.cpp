#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

using hog::DVec3;
using hog::planet::Tectonics;

namespace {

constexpr double radius = 115'000.0;
constexpr Tectonics::Settings settings{.level = 5, .plates = 10, .continental = 0.3, .travel = 1.5};

const Tectonics& start() {
    static const Tectonics tectonics{1, radius, settings};
    return tectonics;
}

double continental_share(const Tectonics& tectonics) {
    double area = 0.0;
    double continental = 0.0;
    const auto& grid = tectonics.grid();
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const auto cell_area = static_cast<double>(grid.area(cell));
        area += cell_area;
        continental +=
            tectonics.columns()[cell].crust == Tectonics::Crust::Continental ? cell_area : 0.0;
    }
    return continental / area;
}

// The unit direction the continental crust of a plate centres on.
DVec3 continental_middle(const Tectonics& tectonics, std::uint32_t plate) {
    DVec3 sum{0.0};
    for (std::uint32_t cell = 0; cell < tectonics.grid().size(); ++cell) {
        const Tectonics::Column& column = tectonics.columns()[cell];
        if (column.plate == plate && column.crust == Tectonics::Crust::Continental) {
            sum += hog::planet::cell_center(tectonics.grid().cell(cell));
        }
    }
    return glm::normalize(sum);
}

} // namespace

TEST_CASE("the crust starts as one supercontinent amid ocean floor", "[planet][tectonics]") {
    const Tectonics& tectonics = start();
    CHECK(std::abs(continental_share(tectonics) - settings.continental) < 0.05);
    // Nearly all of the continental crust is one landmass.
    const auto& grid = tectonics.grid();
    const auto continental = [&](std::uint32_t cell) {
        return tectonics.columns()[cell].crust == Tectonics::Crust::Continental;
    };
    std::vector<bool> seen(grid.size(), false);
    std::size_t total = 0;
    std::size_t largest = 0;
    for (std::uint32_t first = 0; first < grid.size(); ++first) {
        if (!continental(first) || seen[first]) {
            continue;
        }
        std::size_t size = 0;
        std::queue<std::uint32_t> frontier;
        frontier.push(first);
        seen[first] = true;
        while (!frontier.empty()) {
            const std::uint32_t cell = frontier.front();
            frontier.pop();
            ++size;
            for (const std::uint32_t next : grid.neighbors(cell)) {
                if (continental(next) && !seen[next]) {
                    seen[next] = true;
                    frontier.push(next);
                }
            }
        }
        total += size;
        largest = std::max(largest, size);
    }
    CHECK(static_cast<double>(largest) > 0.9 * static_cast<double>(total));
    for (const Tectonics::Column& column : tectonics.columns()) {
        REQUIRE(column.plate < tectonics.plates().size());
    }
}

TEST_CASE("the supercontinent thins toward its coast", "[planet][tectonics]") {
    const Tectonics& tectonics = start();
    const auto& grid = tectonics.grid();
    float coast = 0.0F;
    float inland = 0.0F;
    int coasts = 0;
    int inlands = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const Tectonics::Column& column = tectonics.columns()[cell];
        if (column.crust != Tectonics::Crust::Continental) {
            continue;
        }
        const bool shore = std::ranges::any_of(grid.neighbors(cell), [&](std::uint32_t next) {
            return tectonics.columns()[next].crust == Tectonics::Crust::Oceanic;
        });
        (shore ? coast : inland) += column.thickness;
        ++(shore ? coasts : inlands);
    }
    REQUIRE(coasts > 0);
    REQUIRE(inlands > 0);
    CHECK(coast / static_cast<float>(coasts) < 28.0F);
    CHECK(inland / static_cast<float>(inlands) > 30.0F);
}

TEST_CASE("plates carry their crust round their poles", "[planet][tectonics]") {
    Tectonics tectonics = start();
    // The plate carrying the most continent.
    std::vector<int> counts(tectonics.plates().size(), 0);
    for (const Tectonics::Column& column : tectonics.columns()) {
        counts[column.plate] += column.crust == Tectonics::Crust::Continental ? 1 : 0;
    }
    const auto plate =
        static_cast<std::uint32_t>(std::ranges::max_element(counts) - counts.begin());
    const DVec3 before = continental_middle(tectonics, plate);
    constexpr int steps = 4;
    for (int step = 0; step < steps; ++step) {
        tectonics.step();
    }
    const Tectonics::Plate& moving = tectonics.plates()[plate];
    const DVec3 expected = glm::normalize(
        (before * std::cos(steps * moving.turn)) +
        (glm::cross(moving.pole, before) * std::sin(steps * moving.turn)) +
        (moving.pole * (glm::dot(moving.pole, before) * (1.0 - std::cos(steps * moving.turn)))));
    const DVec3 after = continental_middle(tectonics, plate);
    // Nearer where it should have gone than where it was, within a cell or so.
    CHECK(glm::length(after - expected) < glm::length(after - before));
    CHECK(tectonics.steps() == steps);
}

TEST_CASE("where plates part, new ocean floor wells up", "[planet][tectonics]") {
    Tectonics tectonics = start();
    float oldest_floor = 0.0F;
    for (const Tectonics::Column& column : tectonics.columns()) {
        if (column.crust == Tectonics::Crust::Oceanic) {
            oldest_floor = std::max(oldest_floor, column.age);
        }
    }
    constexpr int steps = 5;
    for (int step = 0; step < steps; ++step) {
        tectonics.step();
    }
    int newest = 0;
    for (const Tectonics::Column& column : tectonics.columns()) {
        REQUIRE(column.plate < tectonics.plates().size());
        if (column.crust == Tectonics::Crust::Oceanic) {
            REQUIRE(column.age <= oldest_floor + static_cast<float>(steps));
            REQUIRE(column.thickness >= Tectonics::oceanic_thickness);
            newest += column.age < static_cast<float>(steps) ? 1 : 0;
        }
    }
    CHECK(newest > 0);
}

TEST_CASE("continental crust is neither made nor lost as the plates move", "[planet][tectonics]") {
    // Without arcs adding to it or wear taking from it.
    auto still = settings;
    still.arc_growth = 0.0F;
    still.wearing = 0.0F;
    Tectonics tectonics{1, radius, still};
    const double before = tectonics.continental_volume();
    for (int step = 0; step < 10; ++step) {
        tectonics.step();
    }
    CHECK(std::abs(tectonics.continental_volume() - before) < 0.05 * before);
}

TEST_CASE("continents meeting pile up into thicker crust", "[planet][tectonics]") {
    Tectonics tectonics = start();
    for (int step = 0; step < 10; ++step) {
        tectonics.step();
    }
    // Raised lately, and thicker for it: the thickest crust of all was piled up, not spread.
    int piled = 0;
    float thickest = 0.0F;
    float thickest_raised = 0.0F;
    for (const Tectonics::Column& column : tectonics.columns()) {
        if (column.crust != Tectonics::Crust::Continental) {
            continue;
        }
        piled +=
            column.raised < 10.0F && column.thickness > Tectonics::continental_thickness ? 1 : 0;
        if (column.thickness > thickest) {
            thickest = column.thickness;
            thickest_raised = column.raised;
        }
    }
    CHECK(piled > 0);
    CHECK(thickest_raised < 10.0F);
}

TEST_CASE("plates slide across the planet rather than spin on the spot", "[planet][tectonics]") {
    const Tectonics& tectonics = start();
    std::vector<DVec3> middles(tectonics.plates().size(), DVec3{0.0});
    for (std::uint32_t cell = 0; cell < tectonics.grid().size(); ++cell) {
        middles[tectonics.columns()[cell].plate] +=
            hog::planet::cell_center(tectonics.grid().cell(cell));
    }
    for (std::size_t plate = 0; plate < middles.size(); ++plate) {
        if (glm::length(middles[plate]) == 0.0) {
            continue;
        }
        // Its pole at least 60 degrees from its middle.
        CHECK(std::abs(glm::dot(glm::normalize(middles[plate]), tectonics.plates()[plate].pole)) <
              0.5);
    }
}

TEST_CASE("plates stay whole as they move", "[planet][tectonics]") {
    Tectonics tectonics = start();
    for (int step = 0; step < 10; ++step) {
        tectonics.step();
    }
    const auto& grid = tectonics.grid();
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const auto plate = tectonics.columns()[cell].plate;
        const auto around = grid.neighbors(cell);
        REQUIRE(std::ranges::any_of(
            around, [&](std::uint32_t next) { return tectonics.columns()[next].plate == plate; }));
    }
    for (int step = 0; step < 50; ++step) {
        tectonics.step();
    }
    // Each plate one piece, not trailing slivers.
    std::vector<bool> seen(grid.size(), false);
    std::vector<int> pieces(tectonics.plates().size(), 0);
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (seen[cell]) {
            continue;
        }
        const auto plate = tectonics.columns()[cell].plate;
        ++pieces[plate];
        std::queue<std::uint32_t> open;
        open.push(cell);
        seen[cell] = true;
        while (!open.empty()) {
            const std::uint32_t at = open.front();
            open.pop();
            for (const std::uint32_t next : grid.neighbors(at)) {
                if (!seen[next] && tectonics.columns()[next].plate == plate) {
                    seen[next] = true;
                    open.push(next);
                }
            }
        }
    }
    for (const int count : pieces) {
        CHECK(count <= 1);
    }
}

TEST_CASE("ocean floor diving under a continent raises an arc behind its coast",
          "[planet][tectonics]") {
    // Ocean floor north of the equator turning south under a still continent south of it.
    const hog::planet::LevelGrid grid{5, radius};
    std::vector<Tectonics::Column> columns(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const bool north = hog::planet::cell_center(grid.cell(cell)).z > 0.0;
        columns[cell] = {.plate = north ? 0U : 1U,
                         .crust = north ? Tectonics::Crust::Oceanic : Tectonics::Crust::Continental,
                         .thickness = north ? Tectonics::oceanic_thickness
                                            : Tectonics::continental_thickness,
                         .age = 10.0F};
    }
    auto still = settings;
    still.wearing = 0.0F;
    Tectonics tectonics{
        grid, {{.pole = {1.0, 0.0, 0.0}, .turn = 0.07}, {.pole = {1.0, 0.0, 0.0}}}, columns, still};
    for (int step = 0; step < 5; ++step) {
        tectonics.step();
    }
    float near_coast = 0.0F;
    float far_inland = 0.0F;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const Tectonics::Column& column = tectonics.columns()[cell];
        const double z = hog::planet::cell_center(grid.cell(cell)).z;
        if (column.crust != Tectonics::Crust::Continental) {
            continue;
        }
        if (z < 0.0 && z > -0.15) {
            near_coast = std::max(near_coast, column.thickness);
        } else if (z < -0.5) {
            far_inland = std::max(far_inland, column.thickness);
        }
    }
    CHECK(near_coast > Tectonics::continental_thickness + 1.0F);
    CHECK(far_inland < Tectonics::continental_thickness + 0.5F);
}

TEST_CASE("island arcs grow into new continents", "[planet][tectonics]") {
    // Old ocean floor diving under young.
    const hog::planet::LevelGrid grid{5, radius};
    std::vector<Tectonics::Column> columns(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const bool north = hog::planet::cell_center(grid.cell(cell)).z > 0.0;
        columns[cell] = {.plate = north ? 0U : 1U,
                         .crust = Tectonics::Crust::Oceanic,
                         .thickness = Tectonics::oceanic_thickness,
                         .age = north ? 50.0F : 5.0F};
    }
    auto quick = settings;
    quick.arc_growth = 4.0F;
    Tectonics tectonics{
        grid, {{.pole = {1.0, 0.0, 0.0}, .turn = 0.07}, {.pole = {1.0, 0.0, 0.0}}}, columns, quick};
    for (int step = 0; step < 6; ++step) {
        tectonics.step();
    }
    CHECK(continental_share(tectonics) > 0.0);
}

TEST_CASE("piled crust spreads into a belt and wears down", "[planet][tectonics]") {
    const hog::planet::LevelGrid grid{5, radius};
    std::vector<Tectonics::Column> columns(
        grid.size(),
        {.crust = Tectonics::Crust::Continental, .thickness = Tectonics::continental_thickness});
    constexpr std::uint32_t peak = 1'000;
    columns[peak].thickness = 90.0F;
    const auto make = [&](float wearing) {
        auto quiet = settings;
        quiet.wearing = wearing;
        return Tectonics{grid, {{.pole = {0.0, 0.0, 1.0}}}, columns, quiet};
    };
    Tectonics spreading = make(0.0F);
    const double volume = spreading.continental_volume();
    for (int step = 0; step < 5; ++step) {
        spreading.step();
    }
    CHECK(spreading.columns()[peak].thickness < 90.0F);
    for (const std::uint32_t next : grid.neighbors(peak)) {
        CHECK(spreading.columns()[next].thickness > Tectonics::continental_thickness);
    }
    CHECK(std::abs(spreading.continental_volume() - volume) < 1e-4 * volume);

    Tectonics wearing = make(0.1F);
    for (int step = 0; step < 5; ++step) {
        wearing.step();
    }
    CHECK(wearing.continental_volume() < volume);
    CHECK(wearing.columns()[peak].thickness < spreading.columns()[peak].thickness);
}

TEST_CASE("crust piled past its strength flows out into a plateau", "[planet][tectonics]") {
    const hog::planet::LevelGrid grid{5, radius};
    std::vector<Tectonics::Column> columns(
        grid.size(),
        {.crust = Tectonics::Crust::Continental, .thickness = Tectonics::continental_thickness});
    constexpr std::uint32_t peak = 1'000;
    columns[peak].thickness = 400.0F;
    auto still = settings;
    still.spreading = 0.0F;
    still.wearing = 0.0F;
    Tectonics tectonics{grid, {{.pole = {0.0, 0.0, 1.0}}}, columns, still};
    const double volume = tectonics.continental_volume();
    tectonics.step();
    int plateau = 0;
    for (const Tectonics::Column& column : tectonics.columns()) {
        CHECK(column.thickness <= still.strongest + 0.01F);
        plateau += column.thickness > still.strongest - 1.0F ? 1 : 0;
    }
    CHECK(plateau > 3);
    CHECK(std::abs(tectonics.continental_volume() - volume) < 1e-4 * volume);
    // Crust that holds itself up stays where it is.
    columns[peak].thickness = still.strongest;
    Tectonics strong{grid, {{.pole = {0.0, 0.0, 1.0}}}, columns, still};
    strong.step();
    CHECK(strong.columns()[peak].thickness == still.strongest);
}

TEST_CASE("the same seed moves the same plates", "[planet][tectonics]") {
    Tectonics one{3, radius, settings};
    Tectonics other{3, radius, settings};
    for (int step = 0; step < 3; ++step) {
        one.step();
        other.step();
    }
    for (std::size_t cell = 0; cell < one.columns().size(); ++cell) {
        REQUIRE(one.columns()[cell].plate == other.columns()[cell].plate);
        REQUIRE(one.columns()[cell].thickness == other.columns()[cell].thickness);
    }
}

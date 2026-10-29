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
            REQUIRE(column.thickness == Tectonics::oceanic_thickness);
            newest += column.age < static_cast<float>(steps) ? 1 : 0;
        }
    }
    CHECK(newest > 0);
}

TEST_CASE("continental crust is neither made nor lost as the plates move", "[planet][tectonics]") {
    Tectonics tectonics = start();
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
    int piled = 0;
    for (const Tectonics::Column& column : tectonics.columns()) {
        if (column.crust == Tectonics::Crust::Continental &&
            column.thickness > Tectonics::continental_thickness) {
            ++piled;
            CHECK(column.raised < 10.0F);
        }
    }
    CHECK(piled > 0);
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

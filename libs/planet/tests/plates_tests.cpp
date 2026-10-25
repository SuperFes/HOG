#include <hog/planet/level_grid.hpp>
#include <hog/planet/plates.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::planet::LevelGrid;
using hog::planet::Plates;

namespace {

constexpr double radius = 115'000.0;
constexpr double speed = 50'000.0; // metres per million years
constexpr double spin = speed / radius;

LevelGrid grid() {
    return LevelGrid{5, radius};
}

const Plates& random_plates() {
    static const Plates plates{1, grid(), radius, 10};
    return plates;
}

// Two plates grown from opposite sides of the planet, turning as given.
Plates pair(DVec3 east_spin, DVec3 west_spin) {
    return Plates{1,
                  grid(),
                  radius,
                  {{.origin = {1.0, 0.0, 0.0}, .spin = east_spin},
                   {.origin = {-1.0, 0.0, 0.0}, .spin = west_spin}}};
}

// The boundary cells within a short way of a unit direction.
std::vector<Plates::Boundary> boundaries_near(const Plates& plates, DVec3 at) {
    std::vector<Plates::Boundary> found;
    for (std::uint32_t index = 0; index < plates.grid().size(); ++index) {
        const auto& boundary = plates.boundaries()[index];
        if (boundary.distance == 0 &&
            glm::dot(hog::planet::cell_center(plates.grid().cell(index)), at) > 0.95) {
            found.push_back(boundary);
        }
    }
    return found;
}

// The mean closing and sliding rates.
std::pair<double, double> mean(const std::vector<Plates::Boundary>& boundaries) {
    double closing = 0.0;
    double sliding = 0.0;
    for (const auto& boundary : boundaries) {
        closing += boundary.closing;
        sliding += boundary.sliding;
    }
    const auto count = static_cast<double>(boundaries.size());
    return {closing / count, sliding / count};
}

} // namespace

TEST_CASE("plates cover the planet, each in one piece", "[plates]") {
    const Plates& plates = random_plates();
    const auto& cells = plates.grid();
    REQUIRE(plates.plates().size() == 10);
    for (std::uint32_t plate = 0; plate < 10; ++plate) {
        std::vector<std::uint32_t> stack;
        std::vector<bool> seen(cells.size(), false);
        std::size_t size = 0;
        for (std::uint32_t index = 0; index < cells.size(); ++index) {
            REQUIRE(plates.plate()[index] < 10);
            if (plates.plate()[index] == plate) {
                ++size;
                if (stack.empty() && !seen[index]) {
                    stack.push_back(index);
                    seen[index] = true;
                }
            }
        }
        REQUIRE(size > 0);
        std::size_t reached = 0;
        while (!stack.empty()) {
            const std::uint32_t index = stack.back();
            stack.pop_back();
            ++reached;
            for (const std::uint32_t neighbor : cells.neighbors(index)) {
                if (!seen[neighbor] && plates.plate()[neighbor] == plate) {
                    seen[neighbor] = true;
                    stack.push_back(neighbor);
                }
            }
        }
        CHECK(reached == size);
    }
}

TEST_CASE("plates grow the same from the same seed and differently from another", "[plates]") {
    const Plates again{1, grid(), radius, 10};
    const Plates other{2, grid(), radius, 10};
    const auto first = random_plates().plate();
    CHECK(std::ranges::equal(first, again.plate()));
    CHECK_FALSE(std::ranges::equal(first, other.plate()));
}

TEST_CASE("boundaries lie where plates meet and are measured inwards from them", "[plates]") {
    const Plates& plates = random_plates();
    const auto& cells = plates.grid();
    for (std::uint32_t index = 0; index < cells.size(); ++index) {
        const auto own = plates.plate()[index];
        const auto& boundary = plates.boundaries()[index];
        const auto around = cells.neighbors(index);
        const bool meets = std::ranges::any_of(
            around, [&](std::uint32_t neighbor) { return plates.plate()[neighbor] != own; });
        REQUIRE((boundary.distance == 0) == meets);
        REQUIRE(boundary.other != own);
        if (!meets) {
            REQUIRE(std::ranges::any_of(around, [&](std::uint32_t neighbor) {
                return plates.plate()[neighbor] == own &&
                       plates.boundaries()[neighbor].distance < boundary.distance;
            }));
        }
    }
}

TEST_CASE("plates turning towards each other close on one side and part on the other", "[plates]") {
    const Plates plates = pair({0.0, 0.0, spin}, {0.0, 0.0, -spin});
    const auto closing = boundaries_near(plates, {0.0, 1.0, 0.0});
    const auto parting = boundaries_near(plates, {0.0, -1.0, 0.0});
    REQUIRE_FALSE(closing.empty());
    REQUIRE_FALSE(parting.empty());
    // The boundary wanders, so here and there the plates meet at a slant, but along it they
    // mostly close or part.
    const auto [closing_rate, closing_slide] = mean(closing);
    const auto [parting_rate, parting_slide] = mean(parting);
    CHECK(closing_rate > 1.5 * speed);
    CHECK(closing_slide < 0.5 * speed);
    CHECK(parting_rate < -1.5 * speed);
    CHECK(parting_slide < 0.5 * speed);
}

TEST_CASE("plates turning about the line between them slide past each other", "[plates]") {
    const Plates plates = pair({spin, 0.0, 0.0}, {-spin, 0.0, 0.0});
    const auto sliding = boundaries_near(plates, {0.0, 1.0, 0.0});
    REQUIRE_FALSE(sliding.empty());
    const auto [rate, slide] = mean(sliding);
    CHECK(std::abs(rate) < 0.5 * speed);
    CHECK(slide > 1.5 * speed);
}

TEST_CASE("continental plates touch one other at most", "[plates]") {
    const Plates& plates = random_plates();
    const auto continental = [&](std::uint32_t plate) {
        return plates.plates()[plate].continental > 0.0;
    };
    std::set<std::pair<std::uint32_t, std::uint32_t>> touching;
    for (std::uint32_t index = 0; index < plates.grid().size(); ++index) {
        const auto own = plates.plate()[index];
        for (const std::uint32_t neighbor : plates.grid().neighbors(index)) {
            const auto other = plates.plate()[neighbor];
            if (own != other && continental(own) && continental(other)) {
                touching.emplace(own, other);
            }
        }
    }
    std::size_t continents = 0;
    for (std::uint32_t plate = 0; plate < plates.plates().size(); ++plate) {
        if (!continental(plate)) {
            continue;
        }
        ++continents;
        const auto beside =
            std::ranges::count_if(touching, [&](const auto& pair) { return pair.first == plate; });
        CHECK(beside <= 1);
    }
    CHECK(continents >= 2);
    CHECK(continents < plates.plates().size());
}

TEST_CASE("a boundary's normal points across it to the other plate", "[plates]") {
    const Plates& plates = random_plates();
    const auto& cells = plates.grid();
    for (std::uint32_t index = 0; index < cells.size(); ++index) {
        const auto& boundary = plates.boundaries()[index];
        const DVec3 at = hog::planet::cell_center(cells.cell(index));
        REQUIRE(std::abs(glm::length(boundary.normal) - 1.0) < 1e-9);
        if (boundary.distance != 0) {
            continue;
        }
        REQUIRE(std::abs(glm::dot(boundary.normal, at)) < 1e-9);
        // Towards the other plate's neighbours of the cell more than away from them.
        double towards = 0.0;
        for (const std::uint32_t neighbor : cells.neighbors(index)) {
            if (plates.plate()[neighbor] == boundary.other) {
                towards +=
                    glm::dot(hog::planet::cell_center(cells.cell(neighbor)) - at, boundary.normal);
            }
        }
        REQUIRE(towards > 0.0);
    }
}

#include "fine_planet.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/detail.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/surface_mesh.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <vector>

using hog::DVec3;
using hog::globe::Cell;
using hog::globe::Eye;
using hog::globe::Generator;

namespace {

const Generator& earth() {
    static const Generator generator{fine(hog::globe::Planet{})};
    return generator;
}

constexpr double ratio = 3.0;

// The chunk among `chunks` over `cell`, if any; there's never more than one.
std::optional<Cell> over(const std::set<Cell>& chunks, Cell cell) {
    std::optional<Cell> found;
    for (Cell up = cell;; up = hog::globe::parent(up)) {
        if (chunks.contains(up)) {
            CHECK_FALSE(found.has_value());
            found = up;
        }
        if (up.level == 0) {
            return found;
        }
    }
}

double width_of(Cell chunk) {
    const std::array<DVec3, 4> corner = hog::globe::corners(chunk);
    return glm::length(corner[1] - corner[0]) * earth().planet().radius;
}

// None overlaps another, and those beside it are at most one detail apart; across the cube's
// edges, those facing it are at its detail.
void check_beside(const std::set<Cell>& chunks, Cell chunk) {
    const hog::globe::CellNeighbors near = hog::globe::around(chunk);
    for (const Cell beside : near.all()) {
        if (const std::optional<Cell> coarse = over(chunks, beside)) {
            CHECK(coarse->level + 1 >= chunk.level);
        }
    }
    const hog::globe::CellNeighbors facing = hog::globe::sides(chunk);
    for (const Cell beside : facing.all()) {
        const std::optional<Cell> coarse = over(chunks, beside);
        if (beside.face != chunk.face && coarse) {
            CHECK(coarse->level == chunk.level);
        }
    }
}

void check_around(Eye eye, double reach) {
    const hog::globe::Layers& layers = earth().layers();
    const std::vector<Cell> found = hog::globe::chunks_around(earth(), eye, reach, ratio);
    REQUIRE_FALSE(found.empty());
    const std::set<Cell> chunks{found.begin(), found.end()};
    REQUIRE(chunks.size() == found.size());
    // Nearest first, the eye's own at full detail when it's on the ground.
    for (std::size_t k = 1; k < found.size(); ++k) {
        CHECK(hog::globe::distance_to(earth(), eye, found[k - 1]) <=
              hog::globe::distance_to(earth(), eye, found[k]));
    }
    if (eye.height == 0.0) {
        CHECK(chunks.contains(hog::globe::chunk_under(eye.up, layers)));
    }
    for (const Cell chunk : found) {
        CAPTURE(chunk.face, chunk.level, chunk.i, chunk.j);
        const double distance = hog::globe::distance_to(earth(), eye, chunk);
        CHECK(distance <= reach);
        // None coarser than its distance allows.
        if (hog::globe::detail_of(chunk, layers) > 0) {
            CHECK(distance >= ratio * width_of(chunk));
        }
        CHECK(over(chunks, chunk) == chunk);
        check_beside(chunks, chunk);
    }
    // Full-detail chunks within reach, short of its edge, are each covered by one.
    const Cell under = hog::globe::chunk_under(eye.up, layers);
    std::set<Cell> seen{under};
    std::vector<Cell> open{under};
    std::size_t covered = 0;
    while (!open.empty()) {
        const Cell cell = open.back();
        open.pop_back();
        if (hog::globe::distance_to(earth(), eye, cell) > reach / 4.0) {
            continue;
        }
        CHECK(over(chunks, cell).has_value());
        ++covered;
        const hog::globe::CellNeighbors next = hog::globe::sides(cell);
        for (const Cell beside : next.all()) {
            if (seen.insert(beside).second) {
                open.push_back(beside);
            }
        }
    }
    CHECK(covered > 0);
}

} // namespace

TEST_CASE("chunks round the eye tile the ground, coarser further off", "[globe][detail]") {
    SECTION("on a face") {
        check_around({.up = glm::normalize(DVec3{0.3, -0.2, 1.0})}, 2'000.0);
    }
    SECTION("across the cube's edge") {
        check_around({.up = glm::normalize(DVec3{1.0, 1.0, 0.2})}, 2'000.0);
    }
    SECTION("at the cube's corner") {
        check_around({.up = glm::normalize(DVec3{1.0, 1.0, 1.0})}, 2'000.0);
    }
    SECTION("from the air, out to the horizon") {
        check_around({.up = glm::normalize(DVec3{0.3, -0.2, 1.0}), .height = 1'000.0}, 15'000.0);
    }
}

TEST_CASE("the higher the eye, the coarser the ground under it", "[globe][detail]") {
    const Eye ground{.up = glm::normalize(DVec3{0.3, -0.2, 1.0})};
    Eye high = ground;
    high.height = 2'000.0;
    const hog::globe::Layers& layers = earth().layers();
    const auto under = [&](Eye eye) {
        const std::vector<Cell> found = hog::globe::chunks_around(earth(), eye, 20'000.0, ratio);
        const std::set<Cell> chunks{found.begin(), found.end()};
        for (int detail = 0;; ++detail) {
            if (chunks.contains(hog::globe::chunk_under(eye.up, layers, detail))) {
                return detail;
            }
        }
    };
    CHECK(under(ground) == 0);
    CHECK(under(high) > 2);
    // And from far out, little but the coarsest.
    Eye orbit = ground;
    orbit.height = 400'000.0;
    const std::vector<Cell> far = hog::globe::chunks_around(earth(), orbit, 1e9, ratio);
    CHECK(far.size() < 200);
}

TEST_CASE("a chunk split stays split until the eye has drawn a little further off",
          "[globe][detail]") {
    const Eye ground{.up = glm::normalize(DVec3{0.3, -0.2, 1.0})};
    const hog::globe::Layers& layers = earth().layers();
    const double radius = earth().planet().radius;
    const std::vector<Cell> near = hog::globe::chunks_around(earth(), ground, 20'000.0, ratio);
    const hog::globe::Drawn shown{near};
    // The chunk a detail over the ground's own under the eye, split for it.
    const Cell parent = hog::globe::chunk_under(ground.up, layers, 1);
    REQUIRE(shown.split(parent));
    REQUIRE_FALSE(shown.contains(parent));
    const std::array<DVec3, 4> corners = hog::globe::corners(parent);
    const double width = glm::length(corners[1] - corners[0]) * radius;
    // Climbs until the parent would no longer be split afresh, though still kept split.
    Eye eye = ground;
    while (hog::globe::distance_to(earth(), eye, parent) < ratio * width) {
        eye.height += 1.0;
    }
    eye.height += 1.0;
    REQUIRE(hog::globe::distance_to(earth(), eye, parent) < hog::globe::split_kept * ratio * width);
    const std::vector<Cell> afresh = hog::globe::chunks_around(earth(), eye, 20'000.0, ratio);
    const std::vector<Cell> kept = hog::globe::chunks_around(earth(), eye, 20'000.0, ratio, shown);
    CHECK(std::ranges::find(afresh, parent) != afresh.end());
    CHECK(std::ranges::find(kept, parent) == kept.end());
    // Further off, it merges either way.
    while (hog::globe::distance_to(earth(), eye, parent) < hog::globe::split_kept * ratio * width) {
        eye.height += 1.0;
    }
    eye.height += 1.0;
    const std::vector<Cell> merged =
        hog::globe::chunks_around(earth(), eye, 20'000.0, ratio, shown);
    CHECK(std::ranges::find(merged, parent) != merged.end());
}

TEST_CASE("a steeper falloff gives fewer, coarser chunks far off, and the same ground near",
          "[globe][detail]") {
    const Eye eye{.up = glm::normalize(DVec3{0.3, -0.2, 1.0}), .height = 300.0};
    const hog::globe::Layers& layers = earth().layers();
    const std::vector<Cell> gentle = hog::globe::chunks_around(earth(), eye, 20'000.0, ratio);
    const std::vector<Cell> steep =
        hog::globe::chunks_around(earth(), eye, 20'000.0, ratio, {}, 2.0, 1'000.0);
    CHECK(steep.size() < gentle.size());
    // The coarsest is coarser.
    const auto coarsest = [&](const std::vector<Cell>& found) {
        int most = 0;
        for (const Cell chunk : found) {
            most = std::max(most, hog::globe::detail_of(chunk, layers));
        }
        return most;
    };
    CHECK(coarsest(steep) >= coarsest(gentle));
    // The ground under the eye, the finest, is as fine as it was.
    const auto under = [&](const std::vector<Cell>& found) {
        const std::set<Cell> chunks{found.begin(), found.end()};
        for (int detail = 0;; ++detail) {
            if (chunks.contains(hog::globe::chunk_under(eye.up, layers, detail))) {
                return detail;
            }
        }
    };
    CHECK(under(steep) == under(gentle));
    // And they still tile, with neighbours at most a detail apart.
    const std::set<Cell> chunks{steep.begin(), steep.end()};
    for (const Cell chunk : steep) {
        check_beside(chunks, chunk);
    }
}

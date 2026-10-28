#include "shared_world.hpp"

#include <hog/jobs/job_system.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/river_water.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::terrain::RiverVertex;
using hog::terrain::RiverWater;
using hog::terrain::testing::shared_world;

namespace {

const hog::planet::Elevation& terrain() {
    return shared_world().geology().terrain();
}

double radius() {
    return shared_world().geology().planet_radius();
}

// The river nearest the start, wide enough to see and inland.
const hog::planet::Reach& a_river() {
    static const hog::planet::Reach* found = [] {
        const hog::planet::Reach* nearest = nullptr;
        double closest = std::numeric_limits<double>::infinity();
        for (const auto& reach : terrain().rivers().reaches()) {
            const double away = glm::length(reach.from - hog::planet::start_direction);
            if (reach.width > 4.0 && reach.to_surface > 20 * hog::planet::fixed_one &&
                away < closest) {
                closest = away;
                nearest = &reach;
            }
        }
        return nearest;
    }();
    REQUIRE(found != nullptr);
    return *found;
}

const RiverWater& water() {
    static const RiverWater laid = hog::terrain::river_water(
        shared_world(), glm::normalize(a_river().from + a_river().to), radius());
    return laid;
}

// Where a vertex truly lies, from the planet's centre, in metres.
DVec3 placed(const RiverVertex& vertex) {
    return water().origin.metres() + DVec3{vertex.position};
}

// The vertices of the triangles drawn, each once.
std::vector<std::size_t> drawn() {
    std::vector<std::size_t> used(water().indices.begin(), water().indices.end());
    std::ranges::sort(used);
    const auto [last, end] = std::ranges::unique(used);
    used.erase(last, end);
    return used;
}

} // namespace

TEST_CASE("rivers' water lies along their channels at the level of their water",
          "[terrain][rivers]") {
    REQUIRE(!water().indices.empty());
    int middles = 0;
    for (const std::size_t at : drawn()) {
        const RiverVertex& vertex = water().vertices[at];
        if (vertex.across != 0.0F) {
            continue;
        }
        const DVec3 point = placed(vertex);
        const auto channel = terrain().river(hog::planet::cell_at(glm::normalize(point)));
        REQUIRE(channel);
        CHECK_THAT(glm::length(point) - radius(),
                   WithinAbs(hog::planet::fixed_to_double(channel->surface), 0.01));
        ++middles;
    }
    CHECK(middles > 20);
}

TEST_CASE("rivers' water reaches out to their banks and over them", "[terrain][rivers]") {
    int edges = 0;
    for (const std::size_t at : drawn()) {
        const RiverVertex& vertex = water().vertices[at];
        if (std::abs(vertex.across) != 1.0F) {
            continue;
        }
        // Just inside the edge, from the middle of the same piece, next to it.
        const std::size_t middle = vertex.across < 0.0F ? at + 1 : at - 1;
        REQUIRE(water().vertices[middle].across == 0.0F);
        const DVec3 inside =
            glm::normalize(glm::mix(placed(water().vertices[middle]), placed(vertex), 0.8));
        CHECK(terrain().river(hog::planet::cell_at(inside)));
        ++edges;
    }
    CHECK(edges > 20);
    // Over them by at least half the water's width.
    for (const RiverVertex& vertex : water().vertices) {
        if (std::abs(vertex.across) > 1.0F) {
            CHECK(std::abs(vertex.across) >= 1.999F);
        }
    }
}

TEST_CASE("rivers' water is laid out from a point of a lattice its pattern repeats over",
          "[terrain][rivers]") {
    const DVec3 origin = water().origin.metres() / hog::terrain::river_pattern;
    CHECK(origin == glm::round(origin));
    CHECK(glm::length(water().origin.metres() - (glm::normalize(a_river().from + a_river().to) *
                                                 radius())) < hog::terrain::river_pattern);
}

TEST_CASE("rivers' water flows downstream", "[terrain][rivers]") {
    const auto& vertices = water().vertices;
    const auto& indices = water().indices;
    int checked = 0;
    // Each pair of triangles joins a corner of one piece's end to the same of the next's.
    for (std::size_t at = 0; at + 1 < indices.size(); at += 6) {
        const RiverVertex& here = vertices[indices[at]];
        const RiverVertex& next = vertices[indices[at + 1]];
        if (here.across != 0.0F) {
            continue;
        }
        const DVec3 down = placed(next) - placed(here);
        CHECK(glm::dot(DVec3{here.flow}, down) > 0.0);
        CHECK(here.rough >= 0.0F);
        CHECK(here.rough <= 1.0F);
        ++checked;
    }
    CHECK(checked > 20);
    CHECK(hog::terrain::river_speed(a_river()) > 0.1);
    CHECK(hog::terrain::river_speed(a_river()) < 5.0);
}

TEST_CASE("rivers' water is laid out anew round the eye as it goes", "[terrain][rivers]") {
    hog::jobs::JobSystem jobs{2};
    hog::terrain::RiverStream stream{shared_world(), radius(), jobs};
    const DVec3 eye = glm::normalize(a_river().from + a_river().to);
    stream.follow(eye);
    const auto first = stream.finish();
    REQUIRE(first);
    CHECK(first->vertices == water().vertices);
    // A short way off, it stays as it is.
    const hog::planet::TangentFrame frame = hog::planet::tangent_frame(eye);
    stream.follow(glm::normalize(eye + (frame.east * (100.0 / radius()))));
    CHECK(!stream.in_flight());
    stream.follow(
        glm::normalize(eye + (frame.east * (2.0 * hog::terrain::river_relay / radius()))));
    CHECK(stream.in_flight());
    const auto next = stream.finish();
    REQUIRE(next);
    CHECK(next->origin.metres() != first->origin.metres());
}

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

// Which corner across its piece's end a vertex is, from the bank on the left over the water's
// edges to the bank on the right, the middle third.
std::size_t corner_of(std::size_t at) {
    return at % 5;
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
        if (corner_of(at) != 2) {
            continue;
        }
        CHECK(vertex.beyond < 0.0F);
        const DVec3 point = placed(vertex);
        const auto channel = terrain().river(hog::planet::cell_at(glm::normalize(point)));
        REQUIRE(channel.has_value());
        const auto surface = channel.value_or(hog::planet::Channel{}).surface;
        CHECK_THAT(glm::length(point) - radius(),
                   WithinAbs(hog::planet::fixed_to_double(surface), 0.01));
        ++middles;
    }
    CHECK(middles > 20);
}

TEST_CASE("rivers' water reaches out to their banks and over them", "[terrain][rivers]") {
    int edges = 0;
    for (const std::size_t at : drawn()) {
        const RiverVertex& vertex = water().vertices[at];
        if (corner_of(at) != 1 && corner_of(at) != 3) {
            continue;
        }
        CHECK(vertex.beyond == 0.0F);
        // Just inside the edge, from the middle of the same piece, next to it.
        const std::size_t middle = corner_of(at) == 1 ? at + 1 : at - 1;
        const DVec3 inside =
            glm::normalize(glm::mix(placed(water().vertices[middle]), placed(vertex), 0.8));
        CHECK(terrain().river(hog::planet::cell_at(inside)));
        ++edges;
    }
    CHECK(edges > 20);
    // Over them by at least as far as the water's laid over them.
    for (std::size_t at = 0; at < water().vertices.size(); ++at) {
        if (corner_of(at) == 0 || corner_of(at) == 4) {
            CHECK(static_cast<double>(water().vertices[at].beyond) >= hog::terrain::river_overlap);
        }
    }
}

TEST_CASE("rivers' water tells drawing which way across it widens", "[terrain][rivers]") {
    for (std::size_t at = 0; at < water().vertices.size(); ++at) {
        const RiverVertex& vertex = water().vertices[at];
        CHECK(vertex.half > 0.0F);
        const std::size_t middle = at - corner_of(at) + 2;
        CHECK(vertex.half == water().vertices[middle].half);
        if (corner_of(at) == 2) {
            CHECK(vertex.outward == hog::Vec3{0.0F});
            continue;
        }
        CHECK_THAT(glm::length(vertex.outward), WithinAbs(1.0, 1e-5));
        // Out from the middle, level over the ground.
        const DVec3 out = placed(vertex) - placed(water().vertices[middle]);
        CHECK(glm::dot(DVec3{vertex.outward}, glm::normalize(out)) > 0.99);
        CHECK(std::abs(glm::dot(DVec3{vertex.outward}, glm::normalize(placed(vertex)))) < 0.01);
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
        if (corner_of(indices[at]) != 2) {
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
    const auto finished = stream.finish();
    REQUIRE(finished.has_value());
    const hog::terrain::RiverWater first = finished.value_or(hog::terrain::RiverWater{});
    CHECK(first.vertices == water().vertices);
    // A short way off, it stays as it is.
    const hog::planet::TangentFrame frame = hog::planet::tangent_frame(eye);
    stream.follow(glm::normalize(eye + (frame.east * (100.0 / radius()))));
    CHECK(!stream.in_flight());
    stream.follow(
        glm::normalize(eye + (frame.east * (2.0 * hog::terrain::river_relay / radius()))));
    CHECK(stream.in_flight());
    const auto next = stream.finish();
    REQUIRE(next.has_value());
    CHECK(next.value_or(hog::terrain::RiverWater{}).origin.metres() != first.origin.metres());
}

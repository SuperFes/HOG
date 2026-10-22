#include <hog/core/math.hpp>
#include <hog/render/drifting_grid.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>

using Catch::Matchers::WithinAbs;
using hog::DVec2;
using hog::DVec3;
using hog::render::DriftingGrid;
using hog::render::GridStep;

namespace {

constexpr double radius = 115'000.0;
constexpr double height = 2.0;
constexpr std::uint32_t size = 256;
constexpr double spacing = 0.5;

// Where the eye starts, and east and north along the ground there.
struct Start {
    DVec3 up = glm::normalize(DVec3{0.2, -0.1, 1.0});
    DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
    DVec3 north = glm::cross(up, east);
};

// Over sea level `along` metres east and north of the start, along the ground.
DVec3 up_at(DVec2 along) {
    const Start at;
    return glm::normalize((at.up * radius) + (at.east * along.x) + (at.north * along.y));
}

// Of `point`, on sea level, the texel it lies on from an eye over `up`.
DVec2 texel_of(const DriftingGrid& grid, DVec3 up, DVec3 point) {
    return hog::render::grid_texel(grid.placing(up, radius, height),
                                   (point * radius) - (up * (radius + height)));
}

} // namespace

TEST_CASE("a drifting grid stays centred under the eye a whole texel at a time", "[render]") {
    const Start at;
    const DVec3 start = at.up;
    DriftingGrid grid{size, spacing};
    const GridStep laid = grid.follow(start, radius, DVec3{0.0}, 0.0);
    CHECK(!laid.kept);
    const DVec2 middle{size / 2.0};
    CHECK_THAT(glm::distance(texel_of(grid, start, start), middle), WithinAbs(0.0, 0.5));
    // A little way on, it doesn't step; further on, it steps by as many texels as it's gone.
    const DVec3 near = up_at({0.3 * spacing, 0.0});
    const GridStep stayed = grid.follow(near, radius, DVec3{0.0}, 0.1);
    CHECK(stayed.kept);
    CHECK(stayed.shift == glm::ivec2{0});
    const DVec3 on = up_at({10.4 * spacing, -3.2 * spacing});
    const GridStep stepped = grid.follow(on, radius, DVec3{0.0}, 0.1);
    CHECK(stepped.kept);
    CHECK(stepped.shift == glm::ivec2{10, -3});
    CHECK(glm::distance(texel_of(grid, on, on), middle) <= (0.5 * std::sqrt(2.0)) + 1e-3);
    // What's on the sea stays on the same texel as the grid steps under it, but for the step.
    const DVec3 mark = up_at({20.0, 12.5});
    const DVec2 before = texel_of(grid, on, mark);
    const DVec3 further = up_at({15.0, 10.0});
    const GridStep again = grid.follow(further, radius, DVec3{0.0}, 0.1);
    const DVec2 after = texel_of(grid, further, mark);
    CHECK_THAT(glm::distance(after + DVec2{again.shift}, before), WithinAbs(0.0, 1e-3));
}

TEST_CASE("a drifting grid drifts with the water at the surface", "[render]") {
    const Start at;
    const DVec3 start = at.up;
    DriftingGrid grid{size, spacing};
    (void)grid.follow(start, radius, DVec3{0.0}, 0.0);
    const DVec3 drift = (at.east * 0.6) - (at.north * 0.2);
    // A fleck on the water drifts with it, and stays on the same texel but for the grid's steps.
    const DVec2 laid = texel_of(grid, start, up_at({5.0, 5.0}));
    glm::ivec2 shifted{0};
    for (int second = 1; second <= 10; ++second) {
        const GridStep step = grid.follow(start, radius, drift, 1.0);
        REQUIRE(step.kept);
        shifted += step.shift;
        const DVec2 drifted = DVec2{5.0, 5.0} + (DVec2{0.6, -0.2} * static_cast<double>(second));
        const DVec2 now = texel_of(grid, start, up_at(drifted)) + DVec2{shifted};
        CHECK_THAT(glm::distance(now, laid), WithinAbs(0.0, 1e-3));
    }
    // The eye stayed put, so the grid stepped back against the drift, a texel per half metre.
    CHECK(shifted == glm::ivec2{-12, 4});
}

TEST_CASE("a drifting grid keeps nothing over a long pause, a step back in time, a long way or "
          "when it's to forget it",
          "[render]") {
    const Start at;
    const DVec3 start = at.up;
    DriftingGrid grid{size, spacing};
    (void)grid.follow(start, radius, DVec3{0.0}, 0.0);
    CHECK(grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
    CHECK(!grid.follow(start, radius, DVec3{0.0}, hog::render::grid_longest_step + 0.5).kept);
    CHECK(!grid.follow(start, radius, DVec3{0.0}, -0.5).kept);
    CHECK(grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
    grid.forget();
    CHECK(!grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
    CHECK(grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
    // Further than the grid is wide.
    CHECK(!grid.follow(up_at({size * spacing, 0.0}), radius, DVec3{0.0}, 0.5).kept);
    // Far from where the plane touched sea level, it's laid again, under the eye.
    const DVec3 far = up_at({hog::render::grid_plane_reach * 1.5, 0.0});
    (void)grid.follow(up_at({hog::render::grid_plane_reach * 0.8, 0.0}), radius, DVec3{0.0}, 0.5);
    CHECK(!grid.follow(far, radius, DVec3{0.0}, 0.5).kept);
    CHECK_THAT(glm::distance(texel_of(grid, far, far), DVec2{size / 2.0}), WithinAbs(0.0, 0.5));
    CHECK_THAT(glm::length(grid.placing(far, radius, height).anchor), WithinAbs(height, 1e-6));
}

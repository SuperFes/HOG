#include <hog/physics/space.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

using namespace hog;
using namespace hog::physics;

namespace {

constexpr double earth = 6'371'000.0;

// A square 20 m across, `metres` from the centre along +z and facing away from it.
Mesh floor_at(double metres) {
    return {.origin = WorldPosition{0.0, 0.0, metres},
            .vertices = {{-10.0F, -10.0F, 0.0F},
                         {10.0F, -10.0F, 0.0F},
                         {10.0F, 10.0F, 0.0F},
                         {-10.0F, 10.0F, 0.0F}},
            .triangles = {{0, 1, 2}, {0, 2, 3}}};
}

} // namespace

TEST_CASE("a ray meets a mesh where it is, however far from the centre", "[physics]") {
    Space space;
    const double height = earth + 0.123;
    const auto floor = space.add(floor_at(height));
    REQUIRE(floor);
    CHECK(space.size() == 1);

    const auto cast = space.cast_ray(WorldPosition{3.0, -4.0, height + 10.0}, {0.0, 0.0, -20.0});
    REQUIRE(cast);
    const Hit hit = cast.value_or(Hit{});
    CHECK(hit.body == *floor);
    CHECK(hit.at.metres().x == Catch::Approx(3.0).margin(1e-4));
    CHECK(hit.at.metres().y == Catch::Approx(-4.0).margin(1e-4));
    CHECK(hit.at.metres().z == Catch::Approx(height).margin(1e-4));
    CHECK(glm::length(hit.normal - DVec3{0.0, 0.0, 1.0}) < 1e-6);
}

TEST_CASE("a ray misses what's beyond its reach, behind it, or gone", "[physics]") {
    Space space;
    const auto floor = space.add(floor_at(earth));
    REQUIRE(floor);

    CHECK_FALSE(space.cast_ray(WorldPosition{0.0, 0.0, earth + 10.0}, {0.0, 0.0, -9.0}));
    CHECK_FALSE(space.cast_ray(WorldPosition{0.0, 0.0, earth - 1.0}, {0.0, 0.0, 2.0}));
    CHECK_FALSE(space.cast_ray(WorldPosition{20.0, 0.0, earth + 1.0}, {0.0, 0.0, -2.0}));

    space.remove(*floor);
    CHECK(space.size() == 0);
    CHECK_FALSE(space.cast_ray(WorldPosition{0.0, 0.0, earth + 1.0}, {0.0, 0.0, -2.0}));
}

TEST_CASE("meshes without triangles, or with corners that aren't there, are refused", "[physics]") {
    Space space;
    Mesh empty = floor_at(earth);
    empty.triangles.clear();
    CHECK_FALSE(space.add(empty));

    Mesh missing = floor_at(earth);
    missing.triangles.push_back({0, 1, 4});
    CHECK_FALSE(space.add(missing));
    CHECK(space.size() == 0);
}

TEST_CASE("a space moves along with what's in it", "[physics]") {
    Space space;
    REQUIRE(space.add(floor_at(earth)));
    const Space moved = std::move(space);
    CHECK(moved.size() == 1);
    CHECK(moved.cast_ray(WorldPosition{0.0, 0.0, earth + 1.0}, {0.0, 0.0, -2.0}));
}

#include <hog/physics/ground.hpp>
#include <hog/physics/space.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/stitching.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <array>
#include <set>
#include <utility>

using namespace hog;
using namespace hog::physics;

namespace {

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): nothing here throws
const planet::PlanetConfig config{.seed = 1,
                                  .ocean_fraction = 0.7,
                                  .radius = 115'000.0,
                                  .erosion_cell = hog::testing::coarse_erosion_cell};

// One World for each test run: the seed's generators are costly to set up.
const planet::World& world() {
    static const planet::World world{config};
    return world;
}

// 10 m over the ground at a unit direction.
WorldPosition over(DVec3 direction) {
    const double ground = static_cast<double>(world().state(planet::cell_at(direction)).elevation) /
                          static_cast<double>(planet::fixed_one);
    return WorldPosition{direction * (config.radius + ground + 10.0)};
}

WorldPosition over_land() {
    return over(
        world().geology().terrain().nearest_land(planet::start_direction, 50 * planet::fixed_one));
}

} // namespace

TEST_CASE("the ground is where it's drawn", "[physics][ground]") {
    Space space;
    Ground ground{space, world()};
    const WorldPosition standing = over_land();
    const std::array around{standing};
    REQUIRE(ground.keep(around));

    const terrain::Patch drawn =
        terrain::bake_patch(world(), planet::cell_at(standing, ground.level()));
    const auto indices = terrain::patch_indices(0);
    // Through the middles of triangles: Jolt's rays can slip between them at a vertex.
    for (const std::size_t triangle :
         {std::size_t{0}, indices.size() / 6, (indices.size() / 3) - 1}) {
        DVec3 middle{0.0};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            middle += DVec3{drawn.vertices[indices[(triangle * 3) + corner]].position} / 3.0;
        }
        const WorldPosition on = drawn.origin + middle;
        const DVec3 up = glm::normalize(on.metres());
        const auto cast = space.cast_ray(on + (up * 20.0), up * -40.0);
        REQUIRE(cast);
        const Hit hit = cast.value_or(Hit{});
        CHECK(glm::length(hit.at - on) < 1e-3);
        CHECK(glm::dot(hit.normal, up) > 0.5);
    }
}

TEST_CASE("the ground is kept near the points, and let go far from them", "[physics][ground]") {
    Space space;
    Ground ground{space, world(), {.tile = 50.0, .reach = 60.0}};
    const WorldPosition here = over_land();
    const std::array around{here};
    REQUIRE(ground.keep(around));
    const planet::CellId under = planet::cell_at(here, ground.level());
    CHECK(ground.holds(under));
    const std::size_t held = ground.size();
    CHECK(held >= 9);
    CHECK(space.size() == held);

    const std::array twice{here, here};
    REQUIRE(ground.keep(twice));
    CHECK(ground.size() == held);

    const planet::TangentFrame frame = planet::tangent_frame(here);
    const std::array away{over(glm::normalize((here + (frame.east * 1'000.0)).metres()))};
    REQUIRE(ground.keep(away));
    CHECK_FALSE(ground.holds(under));
    CHECK(ground.holds(planet::cell_at(away[0], ground.level())));
    CHECK(space.size() == ground.size());
}

TEST_CASE("the ground over a cube face's edge is kept on both faces", "[physics][ground]") {
    Space space;
    Ground ground{space, world()};
    const std::array around{over(glm::normalize(DVec3{1.0, 1.0, 0.3}))};
    REQUIRE(ground.keep(around));
    std::set<int> faces;
    for (int face = 0; face < 6; ++face) {
        for (const DVec3 side : {DVec3{1.0, 0.9999, 0.3}, DVec3{0.9999, 1.0, 0.3}}) {
            const planet::CellId tile = planet::cell_at(glm::normalize(side), ground.level());
            if (tile.face() == face && ground.holds(tile)) {
                faces.insert(face);
            }
        }
    }
    CHECK(faces.size() == 2);
}

TEST_CASE("ground let go of, or moved, takes its bodies along", "[physics][ground]") {
    Space space;
    {
        Ground ground{space, world()};
        const std::array around{over_land()};
        REQUIRE(ground.keep(around));
        const Ground moved = std::move(ground);
        CHECK(space.size() == moved.size());
        CHECK(ground.size() == 0); // NOLINT(bugprone-use-after-move): moved from is empty
    }
    CHECK(space.size() == 0);
}

TEST_CASE("the ground holds over the even floor of the deep ocean", "[physics][ground]") {
    // A patch of floor this even is one Jolt can only split at random as it builds its tree, and
    // warns of.
    const DVec3 deep{1.0, 0.0, 0.0};
    REQUIRE(world().state(planet::cell_at(deep)).elevation < -3'000 * planet::fixed_one);
    Space space;
    Ground ground{space, world()};
    const std::array around{over(deep)};
    REQUIRE(ground.keep(around));
    CHECK(ground.size() > 0);
}

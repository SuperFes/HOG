#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/surface_mesh.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/physics/ground.hpp>
#include <hog/physics/space.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace hog;
using namespace hog::physics;
using Catch::Approx;

namespace {

// Earth-like, once for every test: a generator costs a survey to set up.
const globe::Generator& earth() {
    static const globe::Generator generator{[] {
        globe::Planet planet;
        planet.seed = 1;
        return planet;
    }()};
    return generator;
}

double lod() {
    return globe::voxel_lod(earth());
}

// 10 m over the ground at a unit direction.
WorldPosition over(DVec3 direction) {
    return WorldPosition{direction *
                         (globe::ground_below(earth(), direction, lod()).value_or(0.0) + 10.0)};
}

DVec3 land() {
    return globe::land_near(earth(), DVec3{1.0, 0.0, 0.0}).value_or(DVec3{1.0, 0.0, 0.0});
}

globe::Cell chunk_under(WorldPosition at) {
    return globe::chunk_under(glm::normalize(at.metres()), earth().layers(), 0);
}

} // namespace

TEST_CASE("the ground is where it's drawn", "[physics][ground]") {
    globe::VoxelStore store{earth().layers()};
    const globe::World world{earth(), store};
    Space space;
    Ground ground{space, world};
    const WorldPosition standing = over(land());
    const std::array around{standing};
    REQUIRE(ground.keep(around));
    const globe::Cell chunk = chunk_under(standing);
    REQUIRE(ground.holds(chunk));
    globe::SurfaceMesh drawn;
    drawn.origin = globe::center(chunk) * earth().planet().radius;
    globe::add_chunk(drawn, world, chunk);
    // Through the middles of triangles: Jolt's rays can slip between them at a vertex.
    std::size_t checked = 0;
    for (std::size_t corner = 0; corner + 2 < drawn.indices.size() && checked < 3;
         corner += drawn.indices.size() / 7 * 3) {
        DVec3 middle{0.0};
        for (std::size_t k = 0; k < 3; ++k) {
            middle += DVec3{drawn.vertices[drawn.indices[corner + k]].position} / 3.0;
        }
        if (drawn.vertices[drawn.indices[corner]].material == globe::Material::Water) {
            continue;
        }
        const WorldPosition on = WorldPosition{drawn.origin} + middle;
        const DVec3 up = glm::normalize(on.metres());
        const auto cast = space.cast_ray(on + (up * 2.0), up * -4.0, Faces::Both);
        REQUIRE(cast);
        const Hit hit = cast.value_or(Hit{});
        CHECK(glm::length(hit.at - on) < 1e-3);
        ++checked;
    }
    CHECK(checked > 0);
}

TEST_CASE("the ground is kept near the points, and let go far from them", "[physics][ground]") {
    globe::VoxelStore store{earth().layers()};
    const globe::World world{earth(), store};
    Space space;
    Ground ground{space, world, {.reach = 30.0}};
    const WorldPosition here = over(land());
    const std::array around{here};
    REQUIRE(ground.keep(around));
    const globe::Cell under = chunk_under(here);
    CHECK(ground.holds(under));
    const std::size_t held = ground.size();
    CHECK(held >= 4);
    CHECK(space.size() == held);

    const std::array twice{here, here};
    REQUIRE(ground.keep(twice));
    CHECK(ground.size() == held);

    const DVec3 up = glm::normalize(here.metres());
    const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
    const std::array away{over(glm::normalize(up + (east * (1'000.0 / earth().planet().radius))))};
    REQUIRE(ground.keep(away));
    CHECK_FALSE(ground.holds(under));
    CHECK(ground.holds(chunk_under(away[0])));
    CHECK(space.size() == ground.size());
}

TEST_CASE("the ground over a cube face's edge is kept on both faces", "[physics][ground]") {
    globe::VoxelStore store{earth().layers()};
    const globe::World world{earth(), store};
    Space space;
    Ground ground{space, world, {.reach = 30.0}};
    const std::array around{over(glm::normalize(DVec3{1.0, 1.0, 0.3}))};
    REQUIRE(ground.keep(around));
    std::set<int> faces;
    for (const DVec3 side : {DVec3{1.0, 0.9999, 0.3}, DVec3{0.9999, 1.0, 0.3}}) {
        const globe::Cell chunk = chunk_under(WorldPosition{glm::normalize(side)});
        if (ground.holds(chunk)) {
            faces.insert(chunk.face);
        }
    }
    CHECK(faces.size() == 2);
}

TEST_CASE("ground let go of, or moved, takes its bodies along", "[physics][ground]") {
    globe::VoxelStore store{earth().layers()};
    const globe::World world{earth(), store};
    Space space;
    {
        Ground ground{space, world, {.reach = 30.0}};
        const std::array around{over(land())};
        REQUIRE(ground.keep(around));
        const Ground moved = std::move(ground);
        CHECK(space.size() == moved.size());
        CHECK(ground.size() == 0); // NOLINT(bugprone-use-after-move): moved from is empty
    }
    CHECK(space.size() == 0);
}

TEST_CASE("a brick dug lets go of the ground over it, which comes back with the hole",
          "[physics][ground]") {
    globe::VoxelStore store{earth().layers()};
    globe::World world{earth(), store};
    Space space;
    Ground ground{space, world, {.reach = 30.0}};
    const DVec3 up = land();
    const WorldPosition standing = over(up);
    const std::array around{standing};
    REQUIRE(ground.keep(around));
    const auto drop = [&] {
        const auto cast = space.cast_ray(standing, up * -40.0, Faces::Both);
        REQUIRE(cast);
        return glm::length(cast.value_or(Hit{}).at - standing);
    };
    const double before = drop();
    CHECK(before == Approx(10.0).margin(1.0));
    const std::uint64_t revision = store.revision();
    globe::dig(world, standing.metres() - (up * 10.0), 3.0);
    const std::vector<globe::BrickSite> changed = store.changed_since(revision);
    REQUIRE_FALSE(changed.empty());
    for (const globe::BrickSite brick : changed) {
        ground.changed(brick);
    }
    CHECK_FALSE(ground.holds(chunk_under(standing)));
    REQUIRE(ground.keep(around));
    CHECK(ground.holds(chunk_under(standing)));
    CHECK(drop() > before + 1.5);
}

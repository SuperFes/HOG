#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/held_tiles.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/globe/world.hpp>
#include <hog/physics/ground.hpp>
#include <hog/physics/space.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
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
        planet.body.seed = 1;
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

// The finest tile holding a place.
globe::TileKey tile_at(WorldPosition at) {
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    const globe::Layers& layers = earth().layers();
    const globe::Cell cell = globe::cell_under(glm::normalize(at.metres()), layers.level);
    return {.level = 0,
            .face = cell.face,
            .i = cell.i / side,
            .j = cell.j / side,
            .k = static_cast<std::uint32_t>(layers.layer_at(glm::length(at.metres()))) / side};
}

} // namespace

TEST_CASE("the ground is where its voxels are drawn", "[physics][ground]") {
    globe::MadeWorld made{earth()};
    const globe::World& world = made.world();
    made.cover(land());
    Space space;
    Ground ground{space, world};
    const WorldPosition standing = over(land());
    const std::array around{standing};
    REQUIRE(ground.keep(around));
    // The tile holding the ground under it.
    const DVec3 up = glm::normalize(standing.metres());
    const globe::TileKey key = tile_at(standing - (up * 10.5));
    REQUIRE(ground.holds(key));
    const globe::VoxelTree* tile = world.tile(key);
    REQUIRE(tile != nullptr);
    const globe::VoxelMesh drawn = globe::mesh_tile(
        world.layers(), key, *tile, [&](globe::TileKey at) { return world.tile(at); });
    // Through the middles of the tops of boxes, where the ground is walked on: Jolt's rays can
    // slip between triangles at a vertex.
    std::vector<std::size_t> tops;
    for (std::size_t corner = 0; corner + 2 < drawn.indices.size(); corner += 3) {
        const globe::VoxelVertex& first = drawn.vertices[drawn.indices[corner]];
        const DVec3 at = drawn.origin + DVec3{first.position};
        if (first.material != globe::Material::Water &&
            glm::dot(DVec3{first.normal}, glm::normalize(at)) > 0.99) {
            tops.push_back(corner);
        }
    }
    REQUIRE(tops.size() >= 3);
    std::size_t checked = 0;
    for (std::size_t k = 0; k < tops.size(); k += tops.size() / 3) {
        const std::size_t corner = tops[k];
        DVec3 middle{0.0};
        for (std::size_t n = 0; n < 3; ++n) {
            middle += DVec3{drawn.vertices[drawn.indices[corner + n]].position} / 3.0;
        }
        const WorldPosition on = WorldPosition{drawn.origin} + middle;
        const DVec3 out = glm::normalize(on.metres());
        const auto cast = space.cast_ray(on + (out * 2.0), out * -4.0, Faces::Both);
        REQUIRE(cast);
        const Hit hit = cast.value_or(Hit{});
        CHECK(glm::length(hit.at - on) < 1e-3);
        ++checked;
    }
    CHECK(checked > 0);
}

TEST_CASE("the ground is kept near the points, and let go far from them", "[physics][ground]") {
    globe::MadeWorld made{earth()};
    const globe::World& world = made.world();
    made.cover(land());
    Space space;
    Ground ground{space, world, {.reach = 30.0}};
    const WorldPosition here = over(land());
    const std::array around{here};
    REQUIRE(ground.keep(around));
    const globe::TileKey under = tile_at(here);
    CHECK(ground.holds(under));
    const std::size_t held = ground.size();
    CHECK(held >= 1);
    CHECK(space.size() == held);

    const std::array twice{here, here};
    REQUIRE(ground.keep(twice));
    CHECK(ground.size() == held);

    const DVec3 up = glm::normalize(here.metres());
    const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
    const DVec3 there = glm::normalize(up + (east * (1'000.0 / earth().planet().body.radius)));
    made.cover(there);
    const std::array away{over(there)};
    REQUIRE(ground.keep(away));
    CHECK_FALSE(ground.holds(under));
    CHECK(ground.holds(tile_at(away[0])));
    CHECK(space.size() == ground.size());
}

TEST_CASE("ground not made is kept as none, adding nothing to the space", "[physics][ground]") {
    globe::MadeWorld made{earth()};
    const globe::World& world = made.world();
    Space space;
    Ground ground{space, world, {.reach = 30.0}};
    const std::array around{over(land())};
    REQUIRE(ground.keep(around));
    CHECK(ground.holds(tile_at(around[0])));
    CHECK(ground.size() == 0);
    CHECK(space.size() == 0);
}

TEST_CASE("the ground over a cube face's edge is kept on both faces", "[physics][ground]") {
    globe::MadeWorld made{earth()};
    const globe::World& world = made.world();
    made.cover(glm::normalize(DVec3{1.001, 1.0, 0.3}));
    made.cover(glm::normalize(DVec3{1.0, 1.001, 0.3}));
    Space space;
    Ground ground{space, world, {.reach = 30.0}};
    const std::array around{over(glm::normalize(DVec3{1.0, 1.0, 0.3}))};
    REQUIRE(ground.keep(around));
    std::set<int> faces;
    for (const DVec3 side : {DVec3{1.0, 0.9999, 0.3}, DVec3{0.9999, 1.0, 0.3}}) {
        const globe::TileKey key = tile_at(over(glm::normalize(side)));
        if (ground.holds(key)) {
            faces.insert(key.face);
        }
    }
    CHECK(faces.size() == 2);
}

TEST_CASE("ground let go of, or moved, takes its bodies along", "[physics][ground]") {
    globe::MadeWorld made{earth()};
    const globe::World& world = made.world();
    made.cover(land());
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
    globe::MadeWorld made{earth()};
    globe::World& world = made.world();
    made.cover(land());
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
    // Ground drawn as whole voxels stands within one of the generator's own height.
    CHECK(before == Approx(10.0).margin(earth().layers().thickness));
    const std::uint64_t revision = world.revision();
    globe::dig(world, standing.metres() - (up * 10.0), 3.0);
    const std::vector<globe::BrickSite> changed = world.changed_since(revision);
    REQUIRE_FALSE(changed.empty());
    for (const globe::BrickSite brick : changed) {
        ground.changed(brick);
    }
    CHECK_FALSE(ground.holds(tile_at(standing)));
    REQUIRE(ground.keep(around));
    CHECK(ground.holds(tile_at(standing)));
    CHECK(drop() > before + 1.5);
}

TEST_CASE("there is ground under and round a walker, whatever the cell size", "[physics][ground]") {
    // Chunks wider than the reach: the walker's own chunk can have its middle farther than that.
    for (const double voxel : {5.0, 2.5, 1.0}) {
        globe::Planet planet;
        planet.body.seed = 1;
        planet.voxel = voxel;
        const globe::Generator generator{planet};
        const double fine = globe::voxel_lod(generator);
        int tried = 0;
        int missed = 0;
        for (const DVec3 from : {DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.2}, DVec3{-0.6, 0.5, 0.6},
                                 DVec3{0.3, -0.9, -0.3}, DVec3{-0.2, -0.2, 0.9}}) {
            const DVec3 up =
                globe::land_near(generator, glm::normalize(from)).value_or(glm::normalize(from));
            const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
            const DVec3 north = glm::cross(up, east);
            // Points within 50 m of the walker, a little inside the 60 m reach. Off its own lines:
            // some walkers stand in the plane between two tiles, and a ray along that seam can slip
            // through the float-wide crack between the tiles' bodies.
            std::vector<DVec3> points;
            for (int x = 0; x < 20; ++x) {
                for (int y = 0; y < 20; ++y) {
                    const double a = -47.5 + (5.0 * x);
                    const double b = -47.5 + (5.0 * y);
                    if ((a * a) + (b * b) <= 2'500.0) {
                        points.push_back(
                            glm::normalize(up + (((east * a) + (north * b)) / planet.body.radius)));
                    }
                }
            }
            // The world holds every tile they fall in, however narrow a face's cells are there.
            globe::MadeWorld made{generator};
            const globe::World& world = made.world();
            made.cover(up);
            for (const DVec3 at : points) {
                made.cover(at, 1);
            }
            Space space;
            Ground ground{space, world};
            const double top = globe::ground_below(generator, up, fine).value_or(0.0);
            const std::array around{WorldPosition{up * (top + 1.7)}};
            REQUIRE(ground.keep(around));
            for (const DVec3 at : points) {
                const double surface = globe::ground_below(generator, at, fine).value_or(0.0);
                ++tried;
                missed +=
                    space.cast_ray(WorldPosition{at * (surface + 20.0)}, at * -60.0, Faces::Both)
                        ? 0
                        : 1;
            }
        }
        INFO("cells of " << voxel << " m");
        CHECK(tried > 0);
        CHECK(missed == 0);
    }
}

TEST_CASE("ground is kept from tiles held alone, and kept anew when coarser ones replace them",
          "[physics][ground]") {
    globe::MadeWorld made{earth()};
    made.cover(land());
    constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
    globe::HeldTiles held;
    for (const globe::Site corner : made.volume().columns()) {
        for (const auto& [k, tree] : made.volume().column(corner)->tiles) {
            held.put({.level = 0,
                      .face = corner.face,
                      .i = corner.i / side,
                      .j = corner.j / side,
                      .k = k},
                     std::make_shared<const globe::VoxelTree>(tree));
        }
    }
    globe::Volume none;
    globe::World alone{earth().planet(), earth().layers(), none};
    alone.read_tiles(&held);
    Space space;
    Ground ground{space, alone, {.reach = 30.0}};
    const DVec3 up = land();
    const WorldPosition standing = over(up);
    const std::array around{standing};
    REQUIRE(ground.keep(around));
    const auto drop = [&] {
        const auto cast = space.cast_ray(standing, up * -200.0, Faces::Both);
        REQUIRE(cast);
        return glm::length(cast.value_or(Hit{}).at - standing);
    };
    CHECK(drop() == Approx(10.0).margin(earth().layers().thickness));

    // Sent coarser over it, rock to well under where it stood: walked on there instead.
    globe::TileKey coarse = tile_at(standing);
    coarse = coarse.parent().parent();
    const double depth = glm::length(standing.metres()) -
                         earth().layers().bottom(static_cast<int>(coarse.k * side * 4));
    held.put(coarse, std::make_shared<const globe::VoxelTree>(
                         globe::VoxelTree::build(globe::Volume::tile_side, [](int, int, int z) {
                             return z < 2 ? globe::Cube{.solid = globe::Material::Granite,
                                                        .solid_amount = globe::Voxel::full}
                                          : globe::Cube{};
                         })));
    ground.changed(coarse);
    CHECK_FALSE(ground.holds(tile_at(standing)));
    REQUIRE(ground.keep(around));
    CHECK(ground.holds(tile_at(standing)));
    CHECK(drop() == Approx(depth - (8.0 * earth().layers().thickness)).margin(0.5));
}

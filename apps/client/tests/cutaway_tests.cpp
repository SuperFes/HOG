#include <hog/client/cutaway.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <set>

using Catch::Approx;
using hog::DVec3;
using hog::WorldPosition;
using hog::client::cutaway_shell;

namespace {

constexpr std::array voxels{0.5, 2.5};

hog::globe::Generator world(double voxel) {
    hog::globe::Planet planet;
    planet.radius = 4'000.0;
    planet.floor_depth = 100.0;
    planet.sky_height = 500.0;
    planet.voxel = voxel;
    return hog::globe::Generator{planet};
}

} // namespace

TEST_CASE("the cutaway is the ground's inside seen from within, made of the voxels' materials",
          "[client][cutaway]") {
    const double voxel = GENERATE(from_range(voxels));
    CAPTURE(voxel);
    const hog::globe::Generator generator = world(voxel);
    hog::globe::MadeWorld made{generator};
    hog::globe::World& voxels_now = made.world();
    const DVec3 up = glm::normalize(DVec3{1.0, 0.3, 0.2});
    const double ground = generator.column(up, hog::globe::voxel_lod(generator)).ground();
    constexpr double radius = 12.0;
    // Made as deep as the shell goes, as digging there would make it.
    made.cover_down(up, generator.layers().layer_at(ground - 60.0 - (2.0 * radius)));

    // Deep in the rock, all round is ground.
    const hog::terrain::BlockMesh inside =
        cutaway_shell(voxels_now, WorldPosition{up * (ground - 60.0)}, radius);
    CHECK(inside.indices.size() > 10'000);
    for (const auto& vertex : inside.vertices) {
        CHECK(glm::length(DVec3{vertex.position}) == Approx(radius).margin(1e-4));
        // Facing the eye at the middle.
        CHECK(glm::dot(DVec3{vertex.normal}, glm::normalize(DVec3{vertex.position})) < -0.99);
    }

    // Out in the open air, none.
    const hog::terrain::BlockMesh open =
        cutaway_shell(voxels_now, WorldPosition{up * (ground + 60.0)}, radius);
    CHECK(open.indices.empty());

    // At the ground, only the part under it: some, but far from all.
    const hog::terrain::BlockMesh half =
        cutaway_shell(voxels_now, WorldPosition{up * ground}, radius);
    CHECK(half.indices.size() > inside.indices.size() / 4);
    CHECK(half.indices.size() < inside.indices.size() * 3 / 4);

    // A hole dug through the shell deep in the rock is open there.
    hog::globe::dig(voxels_now, (up * (ground - 60.0)) + (up * radius), 4.0);
    const hog::terrain::BlockMesh dug =
        cutaway_shell(voxels_now, WorldPosition{up * (ground - 60.0)}, radius);
    CHECK(dug.indices.size() < inside.indices.size());
    CHECK(dug.indices.size() > inside.indices.size() * 3 / 4);
}

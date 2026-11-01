#include <hog/client/globe_view.hpp>
#include <hog/globe/cell.hpp>
#include <hog/jobs/job_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

using hog::DVec3;
using hog::Vec3;
using hog::globe::Material;
using hog::globe::SurfaceMesh;

TEST_CASE("the globe is a preset or Earth at the world's size, or a planet file",
          "[client][globe]") {
    const auto earth = hog::client::globe_planet(std::nullopt, 7, 90'000.0);
    REQUIRE(earth);
    CHECK(earth->seed == 7);
    CHECK(earth->radius == 90'000.0);
    CHECK(earth->volatiles == hog::globe::Volatile::Water);
    const auto moon = hog::client::globe_planet("moon", 7, 90'000.0);
    REQUIRE(moon);
    CHECK(moon->radius == 90'000.0);
    CHECK(moon->volatiles == hog::globe::Volatile::None);
    CHECK_FALSE(hog::client::globe_planet("no such planet.toml", 7, 90'000.0));
}

TEST_CASE("every material has a colour of its own", "[client][globe]") {
    std::set<std::array<float, 3>> colours;
    for (int material = 0; material < hog::globe::material_count; ++material) {
        const Vec3 colour = hog::client::surface_colour(static_cast<Material>(material));
        CHECK(colour != Vec3{1.0F, 0.0F, 1.0F});
        colours.insert({colour.x, colour.y, colour.z});
    }
    CHECK(colours.size() == static_cast<std::size_t>(hog::globe::material_count));
}

TEST_CASE("chunks' surfaces become one mesh, each where it lies", "[client][globe]") {
    SurfaceMesh first;
    first.origin = DVec3{100.0, 0.0, 0.0};
    first.vertices = {{.position = Vec3{1.0F, 0.0F, 0.0F},
                       .normal = Vec3{1.0F, 0.0F, 0.0F},
                       .material = Material::Granite},
                      {.position = Vec3{0.0F, 1.0F, 0.0F},
                       .normal = Vec3{1.0F, 0.0F, 0.0F},
                       .material = Material::Granite},
                      {.position = Vec3{0.0F, 0.0F, 1.0F},
                       .normal = Vec3{1.0F, 0.0F, 0.0F},
                       .material = Material::Water}};
    first.indices = {0, 1, 2};
    SurfaceMesh second = first;
    second.origin = DVec3{100.0, 20.0, 0.0};
    second.indices = {2, 1, 0};
    const std::array meshes{&first, &second};
    const hog::terrain::BlockMesh blocks =
        hog::client::globe_blocks(meshes, DVec3{100.0, 10.0, 0.0});
    REQUIRE(blocks.vertices.size() == 6);
    CHECK(blocks.indices == std::vector<std::uint32_t>{0, 1, 2, 5, 4, 3});
    CHECK(blocks.vertices[0].position == Vec3{1.0F, -10.0F, 0.0F});
    CHECK(blocks.vertices[3].position == Vec3{1.0F, 10.0F, 0.0F});
    CHECK(blocks.vertices[2].colour == hog::client::surface_colour(Material::Water));
}

TEST_CASE("the globe's surfaces follow the eye a chunk at a time", "[client][globe]") {
    const hog::client::GlobeWorld globe{hog::globe::Planet{}};
    hog::jobs::JobSystem jobs{3};
    constexpr double reach = 25.0;
    hog::client::GlobeStream stream{globe.world, jobs, reach};
    const DVec3 here = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    stream.follow(here);
    CHECK(stream.in_flight());
    const hog::terrain::BlockMesh shown = stream.finish().value_or(hog::terrain::BlockMesh{});
    CHECK_FALSE(stream.in_flight());
    CHECK(shown.vertices.size() > 1'000);
    CHECK(shown.indices.size() % 3 == 0);
    const std::size_t near = stream.chunks();
    CHECK(near == hog::globe::chunks_near(globe.world, here, reach).size());

    // Going nowhere much changes nothing.
    const double radius = globe.generator.planet().radius;
    stream.follow(glm::normalize(here + DVec3{0.0, 0.0, 2.0 / radius}));
    CHECK_FALSE(stream.in_flight());
    CHECK_FALSE(stream.collect());

    // Far off, the chunks left behind are let go.
    const DVec3 there = glm::normalize(here + DVec3{0.0, 0.0, 400.0 / radius});
    stream.follow(there);
    REQUIRE(stream.finish());
    CHECK(stream.chunks() == hog::globe::chunks_near(globe.world, there, reach).size());
}

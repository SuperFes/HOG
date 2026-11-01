#include <hog/client/globe_view.hpp>
#include <hog/globe/cell.hpp>
#include <hog/jobs/job_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

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

TEST_CASE("a chunk's surface becomes blocks where it lies", "[client][globe]") {
    SurfaceMesh mesh;
    mesh.origin = DVec3{100.0, 20.0, 0.0};
    mesh.vertices = {{.position = Vec3{1.0F, 0.0F, 0.0F},
                      .normal = Vec3{1.0F, 0.0F, 0.0F},
                      .material = Material::Granite},
                     {.position = Vec3{0.0F, 1.0F, 0.0F},
                      .normal = Vec3{1.0F, 0.0F, 0.0F},
                      .material = Material::Granite},
                     {.position = Vec3{0.0F, 0.0F, 1.0F},
                      .normal = Vec3{1.0F, 0.0F, 0.0F},
                      .material = Material::Water}};
    mesh.indices = {2, 1, 0};
    const hog::terrain::BlockMesh blocks = hog::client::globe_blocks(mesh);
    REQUIRE(blocks.vertices.size() == 3);
    CHECK(blocks.origin.metres() == mesh.origin);
    CHECK(blocks.indices == mesh.indices);
    CHECK(blocks.vertices[0].position == Vec3{1.0F, 0.0F, 0.0F});
    CHECK(blocks.vertices[2].colour == hog::client::surface_colour(Material::Water));
}

namespace {

// Keys shown, as changes come in.
void apply(std::set<std::uint64_t>& shown, const hog::client::GlobeStream::Changes& changes) {
    for (const auto& [key, mesh] : changes.shown) {
        CHECK_FALSE(mesh->indices.empty());
        shown.insert(key);
    }
    for (const std::uint64_t key : changes.gone) {
        CHECK(shown.erase(key) == 1);
    }
}

std::set<std::uint64_t> keys_of(std::span<const hog::globe::Cell> chunks) {
    std::set<std::uint64_t> keys;
    for (const hog::globe::Cell chunk : chunks) {
        keys.insert(hog::client::globe_key(chunk));
    }
    return keys;
}

} // namespace

TEST_CASE("the globe's surfaces follow the eye, coarser further off", "[client][globe]") {
    const hog::client::GlobeWorld globe{hog::globe::Planet{}};
    hog::jobs::JobSystem jobs{3};
    hog::client::GlobeStream stream{globe.world, jobs, {.ratio = 1.5, .least = 120.0}};
    const double radius = globe.generator.planet().radius;
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    const auto eye_over = [&](DVec3 at, double height) {
        return at * (radius + hog::client::globe_ground(globe.generator, at) + height);
    };
    std::set<std::uint64_t> shown;
    stream.follow(eye_over(up, 0.0));
    CHECK(stream.in_flight());
    apply(shown, stream.finish());
    CHECK_FALSE(stream.in_flight());
    // Every chunk wanted is shown, at more than one detail.
    CHECK(shown == keys_of(stream.wanted()));
    std::set<int> details;
    for (const hog::globe::Cell chunk : stream.wanted()) {
        details.insert(hog::globe::detail_of(chunk, globe.generator.layers()));
    }
    CHECK(details.size() > 1);

    // Going nowhere much changes nothing.
    stream.follow(eye_over(glm::normalize(up + DVec3{0.0, 0.0, 2.0 / radius}), 0.0));
    CHECK_FALSE(stream.in_flight());
    CHECK(stream.collect().empty());

    // Far off, the chunks left behind are let go, and those beside the new meshed again.
    const DVec3 there = glm::normalize(up + DVec3{0.0, 0.0, 200.0 / radius});
    stream.follow(eye_over(there, 0.0));
    apply(shown, stream.finish());
    CHECK(shown == keys_of(stream.wanted()));
    CHECK(stream.chunks() == shown.size());
}

#include "held_chunks.hpp"
#include "sloped_ground.hpp"

#include <hog/client/block_meshes.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/shape_mesh.hpp>
#include <hog/globe/material.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using hog::DVec3;
using hog::client::GroundChanges;
using hog::grid::Key;

namespace {

std::vector<std::uint64_t> shown_keys(const GroundChanges& changes) {
    std::vector<std::uint64_t> keys;
    keys.reserve(changes.shown.size());
    for (const auto& shown : changes.shown) {
        keys.push_back(shown.key);
    }
    std::ranges::sort(keys);
    return keys;
}

} // namespace

TEST_CASE("block meshes are kept until a piece they read changes", "[client][block_meshes]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    hog::jobs::JobSystem jobs{2};
    hog::client::HeldPieces held{grid};
    REQUIRE(hog::testing::hold(held, *hog::sloped::sloped_root(grid)));
    REQUIRE(hog::testing::hold(held, *hog::sloped::sloped_finely(grid)));
    hog::client::BlockMeshes meshes{grid, jobs, 3};
    const DVec3 eye = hog::sloped::on_ground + (hog::sloped::normal() * 5.0);
    meshes.want(held, eye);
    CHECK(!meshes.settled());
    meshes.finish();
    REQUIRE(meshes.settled());
    const GroundChanges first = meshes.collect();
    const std::vector<std::uint64_t> all = shown_keys(first);
    REQUIRE(!all.empty());
    CHECK(std::ranges::adjacent_find(all) == all.end());
    CHECK(first.gone.empty());
    for (const auto& shown : first.shown) {
        CHECK(!shown.ground->indices.empty());
    }

    SECTION("wanted again as it is, nothing's meshed or changed") {
        meshes.want(held, eye);
        meshes.finish();
        CHECK(meshes.collect().empty());
    }
    SECTION("a piece sent anew meshes the blocks reading it again, and only those") {
        REQUIRE(hog::testing::hold(held, *hog::sloped::sloped_finely(grid)));
        meshes.want(held, eye);
        meshes.finish();
        const GroundChanges again = meshes.collect();
        const std::vector<std::uint64_t> remeshed = shown_keys(again);
        CHECK(!remeshed.empty());
        CHECK(remeshed.size() < all.size());
        CHECK(std::ranges::includes(all, remeshed));
        CHECK(again.gone.empty());
    }
}

TEST_CASE("block keys stay clear of tiles' and each other's", "[client][block_meshes]") {
    const Key node = hog::sloped::piece_root;
    CHECK(hog::client::block_key(node) != hog::client::block_key(node.child(0)));
    CHECK(hog::client::block_key(Key{}) != hog::client::block_key(node));
    CHECK((hog::client::block_key(Key{}) >> 62U) == 3U);
}

TEST_CASE("a shape mesh drawn blends its materials across triangles", "[client][block_meshes]") {
    hog::client::ShapeMesh mesh;
    mesh.origin = DVec3{0.0, 0.0, 1000.0};
    const auto granite = static_cast<std::uint16_t>(hog::globe::Material::Granite);
    const auto sand = static_cast<std::uint16_t>(hog::globe::Material::Sand);
    mesh.vertices = {
        {.position = {0.0F, 0.0F, 0.0F},
         .normal = {0.0F, 0.0F, 1.0F},
         .materials = {granite, sand},
         .share = 0.75F},
        {.position = {1.0F, 0.0F, 0.0F},
         .normal = {0.0F, 0.0F, 1.0F},
         .materials = {granite, sand},
         .share = 0.25F},
        {.position = {0.0F, 1.0F, 0.0F}, .normal = {}, .materials = {0, 0}, .share = 1.0F}};
    mesh.indices = {0, 1, 2};
    const hog::terrain::BlockMesh block = hog::client::block_of(mesh);
    REQUIRE(block.indices.size() == 3);
    const auto& corner = block.vertices[block.indices[0]];
    CHECK(corner.tiles == std::array<std::uint8_t, 3>{
                              static_cast<std::uint8_t>(granite), static_cast<std::uint8_t>(sand),
                              static_cast<std::uint8_t>(hog::globe::Material::Soil)});
    // A normal not known faces up from the planet's centre.
    CHECK(block.vertices[block.indices[2]].normal.z > 0.999F);
}

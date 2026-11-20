#include "held_chunks.hpp"
#include "sloped_ground.hpp"

#include <hog/client/block_meshes.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/shape_mesh.hpp>
#include <hog/globe/material.hpp>
#include <hog/grid/z_order.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
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

// Blocks missing at any step: of a coarse block gone, those finally drawn under it not drawn yet.
std::size_t holes_in(const std::vector<std::set<std::uint64_t>>& steps,
                     const std::vector<Key>& coarse, const std::vector<Key>& fine) {
    std::vector<std::pair<std::uint64_t, std::vector<std::uint64_t>>> replaced;
    for (const Key node : coarse) {
        std::vector<std::uint64_t> under;
        for (const Key finer : fine) {
            if (finer != node && node.contains(finer)) {
                under.push_back(hog::client::block_key(finer));
            }
        }
        replaced.emplace_back(hog::client::block_key(node), std::move(under));
    }
    std::size_t holes = 0;
    for (const std::set<std::uint64_t>& step : steps) {
        for (const auto& [key, under] : replaced) {
            if (!step.contains(key)) {
                holes += static_cast<std::size_t>(std::ranges::count_if(
                    under, [&](std::uint64_t finer) { return !step.contains(finer); }));
            }
        }
    }
    return holes;
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
    CHECK(meshes.stats().plans == 1);
    CHECK(meshes.stats().meshed >= all.size());
    CHECK(meshes.stats().wasted == 0);
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

TEST_CASE("blocks show as they're meshed, finer ones in place of a coarser all at once",
          "[client][block_meshes]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    hog::jobs::JobSystem jobs{2};
    hog::client::HeldPieces held{grid};
    REQUIRE(hog::testing::hold(held, *hog::sloped::sloped_root(grid)));
    REQUIRE(hog::testing::hold(held, *hog::sloped::sloped_finely(grid)));
    hog::client::BlockMeshes meshes{grid, jobs, 3};
    // Seen from high over the ground first, then from near it.
    const DVec3 far = hog::sloped::on_ground + (hog::sloped::normal() * 200.0);
    const DVec3 eye = hog::sloped::on_ground + (hog::sloped::normal() * 5.0);
    meshes.want(held, far);
    meshes.finish();
    // The blocks drawn with anything in them.
    const auto nodes = [&] {
        std::vector<Key> found;
        for (const hog::client::DrawnBlock& block : meshes.drawn()) {
            if (block.ground_triangles + block.water_triangles > 0) {
                found.push_back(block.block.node);
            }
        }
        return found;
    };
    const std::vector<Key> coarse = nodes();
    REQUIRE(!coarse.empty());
    std::set<std::uint64_t> drawn;
    for (const Key node : coarse) {
        drawn.insert(hog::client::block_key(node));
    }

    // Come near, what's wanted is drawn finer under some coarse blocks. What's drawn after each
    // change is kept, by the keys shown and gone.
    meshes.want(held, eye);
    std::vector<std::set<std::uint64_t>> steps;
    while (!meshes.settled()) {
        const GroundChanges changes = meshes.collect();
        if (changes.empty()) {
            continue;
        }
        for (const std::uint64_t key : changes.gone) {
            drawn.erase(key);
        }
        for (const auto& shown : changes.shown) {
            drawn.insert(shown.key);
        }
        steps.push_back(drawn);
    }
    const std::vector<Key> fine = nodes();
    REQUIRE(fine != coarse);
    REQUIRE(!steps.empty());
    std::set<std::uint64_t> last;
    for (const Key node : fine) {
        last.insert(hog::client::block_key(node));
    }
    CHECK(steps.back() == last);
    CHECK(holes_in(steps, coarse, fine) == 0);
    // Not all at once: something was shown before the last block was meshed.
    CHECK(steps.size() > 1);
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

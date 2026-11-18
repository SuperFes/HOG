#include "sloped_ground.hpp"

#include <hog/client/block_meshes.hpp>
#include <hog/client/held_pages.hpp>
#include <hog/jobs/job_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

TEST_CASE("block meshes are kept until a page they read changes", "[client][block_meshes]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    hog::jobs::JobSystem jobs{2};
    hog::client::HeldPages held{grid};
    REQUIRE(held.take(hog::sloped::sloped(grid, Key{})->encode()));
    REQUIRE(held.take(hog::sloped::sloped(grid, hog::sloped::page_root)->encode()));
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
    SECTION("a page sent anew meshes the blocks reading it again, and only those") {
        REQUIRE(held.take(hog::sloped::sloped(grid, hog::sloped::page_root, 2)->encode()));
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

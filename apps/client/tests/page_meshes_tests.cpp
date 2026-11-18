#include <hog/client/drawn_pages.hpp>
#include <hog/client/held_pages.hpp>
#include <hog/client/page_meshes.hpp>
#include <hog/globe/material.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/page.hpp>
#include <hog/jobs/job_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using hog::DVec3;
using hog::client::DrawnPage;
using hog::client::GroundChanges;
using hog::client::page_key;
using hog::grid::Key;

namespace {

// 1 m nodes: pages 64 m, rooted 5 deep, two side by side along x, the low one's low corner 64 m
// below the planet's centre on x.
constexpr Key low_page{.depth = 5, .x = 15, .y = 16, .z = 16};
constexpr Key high_page{.depth = 5, .x = 16, .y = 16, .z = 16};

// A slope through both, as the page's finest nodes have it.
hog::grid::Page sloped(const hog::grid::Grid& grid, Key root, std::uint32_t version) {
    const DVec3 normal = glm::normalize(DVec3{0.2, 0.3, 1.0});
    const DVec3 low = grid.low(root);
    auto tree = hog::grid::Tree<hog::grid::Shape>::build(6, [&](int x, int y, int z) {
        const DVec3 middle = low + DVec3{x + 0.5, y + 0.5, z + 0.5};
        return hog::grid::Surface::plane(normal, glm::dot(middle - DVec3{0.0, 32.0, 20.3}, normal),
                                         static_cast<std::uint16_t>(hog::globe::Material::Granite));
    });
    return {.root = root,
            .generator = 1,
            .shape = hog::grid::Held<hog::grid::Shape>{.version = version, .tree = std::move(tree)},
            .material = std::nullopt,
            .water = std::nullopt};
}

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

TEST_CASE("page meshes are kept until a page or how it's drawn changes", "[client][page_meshes]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    hog::jobs::JobSystem jobs{2};
    hog::client::HeldPages held{grid};
    REQUIRE(held.take(sloped(grid, low_page, 1).encode()));
    REQUIRE(held.take(sloped(grid, high_page, 1).encode()));
    hog::client::PageMeshes meshes{grid, jobs};
    const DVec3 eye{0.0, 32.0, 40.0};
    std::vector<DrawnPage> drawn{{.page = low_page, .levels = 6, .under = {}},
                                 {.page = high_page, .levels = 6, .under = {}}};
    meshes.want(held, drawn, eye);
    CHECK(!meshes.settled());
    meshes.finish();
    REQUIRE(meshes.settled());
    GroundChanges changes = meshes.collect();
    std::vector<std::uint64_t> expected{page_key(low_page, false), page_key(low_page, true),
                                        page_key(high_page, false), page_key(high_page, true)};
    std::ranges::sort(expected);
    CHECK(shown_keys(changes) == expected);
    CHECK(changes.gone.empty());
    for (const auto& shown : changes.shown) {
        CHECK(!shown.ground->indices.empty());
        CHECK(shown.ground->vertices.front().tiles[0] ==
              static_cast<std::uint8_t>(hog::globe::Material::Granite));
    }

    SECTION("wanted again as it is, nothing's meshed or changed") {
        meshes.want(held, drawn, eye);
        CHECK(meshes.in_flight() == 0);
        CHECK(meshes.settled());
        CHECK(meshes.collect().empty());
    }
    // The side they share is drawn by the low page's seam, as it reads both.
    SECTION("one drawn coarser meshes it again, and the seams by it") {
        drawn[1].levels = 4;
        meshes.want(held, drawn, eye);
        meshes.finish();
        changes = meshes.collect();
        expected = {page_key(low_page, true), page_key(high_page, false),
                    page_key(high_page, true)};
        std::ranges::sort(expected);
        CHECK(shown_keys(changes) == expected);
        CHECK(changes.gone.empty());
    }
    SECTION("a page sent anew meshes it again") {
        REQUIRE(held.take(sloped(grid, high_page, 2).encode()));
        meshes.want(held, drawn, eye);
        meshes.finish();
        expected = {page_key(low_page, true), page_key(high_page, false),
                    page_key(high_page, true)};
        std::ranges::sort(expected);
        CHECK(shown_keys(meshes.collect()) == expected);
    }
    SECTION("a page no longer drawn is gone") {
        drawn.pop_back();
        meshes.want(held, drawn, eye);
        meshes.finish();
        changes = meshes.collect();
        CHECK(changes.gone ==
              std::vector<std::uint64_t>{page_key(high_page, false), page_key(high_page, true)});
        // The low page's seam has no page beside it to meet now.
        CHECK(shown_keys(changes) == std::vector<std::uint64_t>{page_key(low_page, true)});
    }
}

TEST_CASE("page keys stay clear of tiles' and each other's", "[client][page_meshes]") {
    CHECK(page_key(low_page, false) != page_key(low_page, true));
    CHECK(page_key(low_page, false) != page_key(high_page, false));
    CHECK(page_key(Key{}, false) != page_key(low_page, false));
    CHECK(page_key(low_page, true) != ~std::uint64_t{0});
    CHECK((page_key(Key{}, false) >> 62U) == 3U);
}

TEST_CASE("a shape mesh drawn blends its materials across triangles", "[client][page_meshes]") {
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

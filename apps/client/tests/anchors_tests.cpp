#include "test_planet.hpp"

#include <hog/client/anchors.hpp>
#include <hog/client/scene.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/forest.hpp>

#include <catch2/catch_test_macros.hpp>
#include <test_device.hpp>

#include <cstddef>
#include <cstring>
#include <numbers>
#include <vector>

using hog::client::anchor_tile_level;
using hog::client::tile_anchors;
using hog::client::testing::world;
using hog::render::TreeAnchor;
using hog::render::testing::test_device;

namespace {

// The starting islands' middle, where the test planet grows its woods.
hog::WorldPosition islands_centre() {
    const double radius = world().geology().planet_radius();
    const auto& islands = world().geology().terrain().archipelago();
    return hog::WorldPosition{islands.islands().front().center * radius};
}

// A nearby tile that anchors trees, walking outward from the islands' middle.
hog::planet::CellId wooded_tile(const hog::client::BiomePalette& palette) {
    const double radius = world().geology().planet_radius();
    const hog::planet::CellId middle = hog::planet::cell_at(islands_centre(), anchor_tile_level);
    const hog::planet::FaceIJ at = middle.to_face_ij();
    for (std::uint32_t ring = 0; ring < 8; ++ring) {
        for (std::uint32_t i = at.i - ring; i <= at.i + ring; ++i) {
            for (std::uint32_t j = at.j - ring; j <= at.j + ring; ++j) {
                const hog::planet::CellId tile =
                    hog::planet::CellId::from_face_ij(at.face, i, j, anchor_tile_level);
                const hog::WorldPosition origin{hog::planet::cell_center(tile) * radius};
                if (!tile_anchors(world(), tile, origin, palette).anchors.empty()) {
                    return tile;
                }
            }
        }
    }
    FAIL("no wooded tile near the starting islands");
    return middle;
}

} // namespace

TEST_CASE("anchored trees are bound to their spots, the same every build", "[client][anchors]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double radius = world().geology().planet_radius();
    const hog::planet::CellId tile = wooded_tile(palette);
    const hog::WorldPosition origin{hog::planet::cell_center(tile) * radius};
    const std::vector<TreeAnchor> first = tile_anchors(world(), tile, origin, palette).anchors;
    const std::vector<TreeAnchor> again = tile_anchors(world(), tile, origin, palette).anchors;
    REQUIRE(!first.empty());
    REQUIRE(first.size() == again.size());
    REQUIRE(std::memcmp(first.data(), again.data(), first.size() * sizeof(TreeAnchor)) == 0);

    // Every tree stands within its tile, above the shore, near upright, a pine's size for its age.
    const double span = (std::numbers::pi / 2.0) * radius /
                        static_cast<double>(std::uint32_t{1} << anchor_tile_level);
    for (const TreeAnchor& anchor : first) {
        CHECK(glm::length(glm::dvec3{anchor.position}) < span * 1.5);
        CHECK(anchor.height > 0.5F);
        CHECK(anchor.height < 25.0F);
        const hog::DVec3 up = glm::normalize(origin.metres());
        CHECK(glm::dot(glm::dvec3{anchor.normal}, up) > 0.65);
        CHECK(hog::render::needles_of(anchor.colour).g > 0.0F);
        CHECK(anchor.age >= 0.0F);
        CHECK(anchor.age <= 1.0F);
    }
}

TEST_CASE("the open sea anchors no trees", "[client][anchors]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double radius = world().geology().planet_radius();
    // The far side of the planet from the starting islands is open ocean on the test world.
    const hog::WorldPosition opposite{-islands_centre().metres()};
    const hog::planet::CellId tile = hog::planet::cell_at(opposite, anchor_tile_level);
    const hog::WorldPosition origin{hog::planet::cell_center(tile) * radius};
    CHECK(tile_anchors(world(), tile, origin, palette).anchors.empty());
}

TEST_CASE("a tile's trees are kept in blocks, each a patch of ground of its own",
          "[client][anchors]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double radius = world().geology().planet_radius();
    const hog::planet::CellId tile = wooded_tile(palette);
    const hog::WorldPosition origin{hog::planet::cell_center(tile) * radius};
    const hog::client::TileTrees trees = tile_anchors(world(), tile, origin, palette);
    CHECK(trees.starts.front() == 0);
    CHECK(trees.starts.back() == trees.anchors.size());
    // A block spans a quarter of the tile's side: its trees stand within that of each other.
    const double span = (std::numbers::pi / 2.0) * radius /
                        static_cast<double>(std::uint32_t{1} << anchor_tile_level);
    for (std::size_t block = 0; block < hog::client::tile_blocks; ++block) {
        REQUIRE(trees.starts.at(block) <= trees.starts.at(block + 1));
        CHECK(trees.starts.at(block + 1) - trees.starts.at(block) <= 16);
        for (std::uint32_t a = trees.starts.at(block); a < trees.starts.at(block + 1); ++a) {
            for (std::uint32_t b = a; b < trees.starts.at(block + 1); ++b) {
                const glm::vec3 apart = trees.anchors.at(a).position - trees.anchors.at(b).position;
                CHECK(static_cast<double>(glm::length(apart)) < span * 0.4);
            }
        }
    }
}

TEST_CASE("the grown forest draws only the blocks near the eye, through the air",
          "[client][anchors][gpu]") {
    const auto device = test_device();
    auto pool = hog::render::AnchorPool::create(*device, {.tiles = 64, .frames = 1, .uploads = 64});
    REQUIRE(pool);
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double radius = world().geology().planet_radius();
    const hog::planet::CellId tile = wooded_tile(palette);
    const hog::DVec3 up = hog::planet::cell_center(tile);
    const double ground = hog::client::ground_under(world(), hog::WorldPosition{up * radius});
    const hog::WorldPosition standing{up * (radius + ground + 2.0)};
    hog::client::TreeAnchors anchors{palette};
    REQUIRE(anchors.update(*pool, 0, world(), standing, 300.0, 64));
    REQUIRE(anchors.size() > 0);

    const std::vector<hog::render::StandDraw> near = anchors.forest_draws({.eye = standing});
    REQUIRE(!near.empty());
    bool finest = false;
    for (const hog::render::StandDraw& draw : near) {
        CHECK(draw.count > 0);
        CHECK(draw.count <= 16);
        CHECK(draw.first + draw.count <= hog::render::tile_trees);
        CHECK(draw.distance < hog::render::forest_far);
        finest = finest || draw.detail == 0;
    }
    CHECK(finest);
    // Fewer than all the held blocks: the forest's reach is shorter than the stamps'.
    std::size_t held = 0;
    for (const hog::render::StandDraw& draw : anchors.draws({.eye = standing})) {
        held += draw.count;
    }
    std::size_t grown = 0;
    for (const hog::render::StandDraw& draw : near) {
        grown += draw.count;
    }
    CHECK(grown < held);

    // High over the same woods no grown tree is near, though the stamps still stand.
    const hog::WorldPosition flying{up * (radius + ground + 1'500.0)};
    CHECK(anchors.forest_draws({.eye = flying}).empty());
    CHECK(!anchors.draws({.eye = flying}).empty());
}

TEST_CASE("sight holds what's ahead and drops what's behind", "[client][anchors]") {
    const hog::render::Camera camera{.eye = hog::WorldPosition{hog::DVec3{0.0}},
                                     .forward = {0.0, 1.0, 0.0},
                                     .up = {0.0, 0.0, 1.0},
                                     .vertical_fov = 1.0};
    const hog::client::Sight sight = hog::client::sight_of(camera, 16.0 / 9.0);
    CHECK(sight.half_angle > 0.5);
    CHECK(sight.half_angle < 1.5);
    CHECK(hog::client::within_sight(sight, {0.0, 100.0, 0.0}, 1.0));
    CHECK(!hog::client::within_sight(sight, {0.0, -100.0, 0.0}, 1.0));
    // Just past the cone's edge, a ball big enough reaches back into it.
    CHECK(!hog::client::within_sight(sight, {100.0, 10.0, 0.0}, 1.0));
    CHECK(hog::client::within_sight(sight, {100.0, 10.0, 0.0}, 90.0));
    // The eye inside the ball sees it, whichever way it looks.
    CHECK(hog::client::within_sight(sight, {0.0, -5.0, 0.0}, 10.0));
    // Unbounded sight sees everything.
    CHECK(hog::client::within_sight({}, {0.0, -100.0, 0.0}, 1.0));
}

TEST_CASE("a frame's tree count sums its draws", "[client][anchors]") {
    const std::vector<hog::render::StandDraw> draws{{.slot = 0, .first = 0, .count = 12},
                                                    {.slot = 3, .first = 16, .count = 5}};
    CHECK(hog::client::anchors_in(draws) == 17);
    CHECK(hog::client::anchors_in({}) == 0);
}

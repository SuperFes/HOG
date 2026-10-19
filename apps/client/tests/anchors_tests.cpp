#include "test_planet.hpp"

#include <hog/client/anchors.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <numbers>
#include <vector>

using hog::client::anchor_tile_level;
using hog::client::tile_anchors;
using hog::client::testing::world;
using hog::render::TreeAnchor;

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
                if (!tile_anchors(world(), tile, origin, palette).empty()) {
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
    const std::vector<TreeAnchor> first = tile_anchors(world(), tile, origin, palette);
    const std::vector<TreeAnchor> again = tile_anchors(world(), tile, origin, palette);
    REQUIRE(!first.empty());
    REQUIRE(first.size() == again.size());
    REQUIRE(std::memcmp(first.data(), again.data(), first.size() * sizeof(TreeAnchor)) == 0);

    // Every tree stands within its tile, above the shore, near upright, grown to a pine's size.
    const double span = (std::numbers::pi / 2.0) * radius /
                        static_cast<double>(std::uint32_t{1} << anchor_tile_level);
    for (const TreeAnchor& anchor : first) {
        CHECK(glm::length(glm::dvec3{anchor.position}) < span * 1.5);
        CHECK(anchor.height > 2.0F);
        CHECK(anchor.height < 25.0F);
        const hog::DVec3 up = glm::normalize(origin.metres());
        CHECK(glm::dot(glm::dvec3{anchor.normal}, up) > 0.65);
        CHECK(anchor.colour.g > 0.0F);
    }
}

TEST_CASE("the open sea anchors no trees", "[client][anchors]") {
    const hog::client::BiomePalette palette = hog::client::biome_palette();
    const double radius = world().geology().planet_radius();
    // The far side of the planet from the starting islands is open ocean on the test world.
    const hog::WorldPosition opposite{-islands_centre().metres()};
    const hog::planet::CellId tile = hog::planet::cell_at(opposite, anchor_tile_level);
    const hog::WorldPosition origin{hog::planet::cell_center(tile) * radius};
    CHECK(tile_anchors(world(), tile, origin, palette).empty());
}

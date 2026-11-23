#include "flora_fixture.hpp"

#include <hog/flora/asset.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

using Catch::Approx;
using hog::flora::kept_index_count;
using hog::flora::kept_leaves;
using hog::flora::kept_pieces;
using hog::flora::leaf_at;
using hog::flora::leaf_corners;
using hog::flora::MeshRole;
using hog::flora::parse_species;

namespace {

std::vector<std::byte> fixture() {
    std::vector<std::byte> out(flora_fixture::bytes.size());
    std::ranges::transform(flora_fixture::bytes, out.begin(),
                           [](std::uint8_t b) { return static_cast<std::byte>(b); });
    return out;
}

} // namespace

TEST_CASE("a species baked by the tree lab parses", "[flora][asset]") {
    const auto bytes = fixture();
    const auto species = parse_species(bytes);
    REQUIRE(species.has_value());
    CHECK(species->name == "fixture");
    CHECK(species->colour_range == Approx(2.0));
    CHECK(species->leaf.corners == 6);
    // The leaves and wood share one, the cards' albedo, and the impostor's five, its normals the
    // same as the cards' and kept once.
    REQUIRE(species->textures.size() == 7);
    CHECK(species->textures[0].levels.size() == 2);
    CHECK(species->textures[0].levels[1].size() == 2 * 2 * 4);
    REQUIRE(species->variants.size() == 1);
    const auto& variant = species->variants[0];
    CHECK(variant.seed == 7);
    CHECK(variant.age == 30);
    REQUIRE(variant.meshes.size() == 3);
    CHECK(variant.meshes[0].role == MeshRole::Wood);
    CHECK(variant.meshes[2].role == MeshRole::Card);
    CHECK(hog::flora::mesh_level(variant, 0) == 0);
    CHECK(hog::flora::mesh_level(variant, 1) == 1);
    CHECK(hog::flora::mesh_level(variant, 2) == 1);
    CHECK(hog::flora::card_levels(variant) == 1);
    CHECK(variant.meshes[0].vertices.size() == variant.meshes[0].vertex_count * 32);
    REQUIRE(variant.leaves.has_value());
    CHECK(variant.leaves.value().count == 12);
    REQUIRE(variant.impostor.has_value());
    CHECK(variant.impostor.value().views == 2);
    CHECK(variant.impostor.value().cell == 4);
}

TEST_CASE("leaves rebuild where the tree lab made them", "[flora][asset]") {
    const auto bytes = fixture();
    const auto species = parse_species(bytes);
    REQUIRE(species.has_value());
    const auto& variant = species->variants[0];
    REQUIRE(variant.leaves.has_value());
    float worst = 0.0F;
    for (std::size_t i = 0; i < variant.leaves.value().count; ++i) {
        const auto corners =
            leaf_corners(species->leaf, leaf_at(variant, variant.leaves.value(), i));
        for (std::size_t c = 0; c < corners.size(); ++c) {
            const std::size_t at = ((i * 6) + c) * 3;
            const hog::Vec3 made{flora_fixture::corners.at(at), flora_fixture::corners.at(at + 1),
                                 flora_fixture::corners.at(at + 2)};
            worst = std::max(worst, glm::length(corners.at(c) - made));
        }
    }
    CHECK(worst < 0.003F);
}

TEST_CASE("levels of detail draw the prefixes the tree lab sorted for", "[flora][asset]") {
    const auto bytes = fixture();
    const auto species = parse_species(bytes);
    REQUIRE(species.has_value());
    const auto& variant = species->variants[0];
    for (std::size_t i = 0; i < flora_fixture::metres_per_pixel.size(); ++i) {
        const float px = flora_fixture::metres_per_pixel.at(i);
        const auto pieces = kept_pieces(variant.meshes[0], px);
        CHECK(pieces == flora_fixture::pieces.at(i));
        CHECK(kept_index_count(variant.meshes[0], pieces) == flora_fixture::indices.at(i));
        CHECK(kept_leaves(variant.leaves.value(), px) == flora_fixture::leaves.at(i));
    }
}

TEST_CASE("broken flora files are refused, not read past", "[flora][asset]") {
    auto bytes = fixture();
    CHECK_FALSE(parse_species(std::span{bytes}.first(bytes.size() - 40)).has_value());
    CHECK_FALSE(parse_species(std::span{bytes}.first(12)).has_value());
    bytes[0] = std::byte{'X'};
    CHECK_FALSE(parse_species(bytes).has_value());
}

TEST_CASE("half floats decode", "[flora][asset]") {
    CHECK(hog::flora::half_to_float(0x3C00) == 1.0F);
    CHECK(hog::flora::half_to_float(0xC000) == -2.0F);
    CHECK(hog::flora::half_to_float(0x7BFF) == 65504.0F);
    CHECK(hog::flora::half_to_float(0x0001) == Approx(5.96046e-8));
}

TEST_CASE("files from before levels were stored take their cards as one level", "[flora][asset]") {
    hog::flora::Variant variant;
    const auto mesh = [](MeshRole role, std::uint32_t dims, std::uint32_t level) {
        hog::flora::MeshPart part;
        part.role = role;
        part.dims = dims;
        part.level = level;
        return part;
    };
    variant.meshes = {mesh(MeshRole::Wood, 1, 0), mesh(MeshRole::Wood, 0, 0),
                      mesh(MeshRole::Wood, 0, 0), mesh(MeshRole::Card, 0, 0)};
    CHECK(hog::flora::mesh_level(variant, 0) == 0);
    CHECK(hog::flora::mesh_level(variant, 1) == 1);
    CHECK(hog::flora::mesh_level(variant, 3) == 1);
    variant.meshes = {mesh(MeshRole::Wood, 1, 0), mesh(MeshRole::Card, 0, 1),
                      mesh(MeshRole::Wood, 0, 2), mesh(MeshRole::Card, 0, 2)};
    CHECK(hog::flora::mesh_level(variant, 1) == 1);
    CHECK(hog::flora::mesh_level(variant, 2) == 2);
    CHECK(hog::flora::card_levels(variant) == 2);
    variant.meshes = {mesh(MeshRole::Wood, 1, 0)};
    CHECK(hog::flora::card_levels(variant) == 0);
}

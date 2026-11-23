#include <hog/core/hash.hpp>
#include <hog/flora/placement.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

using hog::DVec3;
using hog::flora::Kind;
using hog::flora::Planting;
using hog::flora::Spot;
using hog::flora::Woods;

namespace {

Planting three() {
    return {.species = {{.kind = Kind::Needleleaf, .variants = 8, .impostors = 4},
                        {.kind = Kind::Deciduous, .variants = 8, .impostors = 4},
                        {.kind = Kind::Deciduous, .variants = 5, .impostors = 2}},
            .spacing = 7.0};
}

Woods woods_of(double cover, std::array<double, hog::flora::kinds> shares) {
    return {.cover = cover, .shares = shares};
}

std::vector<Spot> many(std::size_t count) {
    std::vector<Spot> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back({.at = DVec3{0.0}, .hash = hog::hash_values(5U, i)});
    }
    return out;
}

} // namespace

TEST_CASE("the ground's two ways across are square to up and each other", "[flora][placement]") {
    for (const DVec3 up :
         {DVec3{0, 0, 1}, DVec3{0, 0, -1}, DVec3{1, 0, 0}, glm::normalize(DVec3{0.3, -0.8, 0.52}),
          glm::normalize(DVec3{0.01, 0.0, 1.0})}) {
        const auto across = hog::flora::across_of(up);
        CHECK(std::abs(glm::length(across.east) - 1.0) < 1e-12);
        CHECK(std::abs(glm::length(across.north) - 1.0) < 1e-12);
        CHECK(std::abs(glm::dot(across.east, up)) < 1e-12);
        CHECK(std::abs(glm::dot(across.north, up)) < 1e-12);
        CHECK(std::abs(glm::dot(across.east, across.north)) < 1e-12);
    }
}

TEST_CASE("spots cover a node's shadow, one to a cell, the same every time", "[flora][placement]") {
    const DVec3 middle{10.0, -40.0, 5000.0};
    const DVec3 up = glm::normalize(middle);
    const auto across = hog::flora::across_of(up);
    const auto spots = hog::flora::spots(middle, 16.0, up, 77U, 7.0);
    REQUIRE(spots.size() == 16); // 27.7 m wide in 4 cells
    const double half = 8.0 * std::numbers::sqrt3;
    std::array<int, 16> per_cell{};
    for (const Spot& spot : spots) {
        const DVec3 off = spot.at - middle;
        CHECK(std::abs(glm::dot(off, up)) < 1e-9);
        const double east = glm::dot(off, across.east) + half;
        const double north = glm::dot(off, across.north) + half;
        REQUIRE(east >= 0.0);
        REQUIRE(north >= 0.0);
        const auto i = static_cast<std::size_t>(east / (2.0 * half / 4.0));
        const auto j = static_cast<std::size_t>(north / (2.0 * half / 4.0));
        REQUIRE(i < 4);
        REQUIRE(j < 4);
        ++per_cell.at((j * 4) + i);
    }
    for (const int count : per_cell) {
        CHECK(count == 1);
    }
    const auto again = hog::flora::spots(middle, 16.0, up, 77U, 7.0);
    const auto other = hog::flora::spots(middle, 16.0, up, 78U, 7.0);
    for (std::size_t i = 0; i < spots.size(); ++i) {
        CHECK(again[i].at == spots[i].at);
        CHECK(again[i].hash == spots[i].hash);
        CHECK(other[i].hash != spots[i].hash);
    }
}

TEST_CASE("cover decides how many stand, and thinner woods keep a share of the same trees",
          "[flora][placement]") {
    const Planting planting = three();
    const std::array<double, 6> shares{0.0, 1.0, 0.0, 0.0, 0.0, 0.0};
    std::size_t full = 0;
    std::size_t half = 0;
    for (const Spot& spot : many(20000)) {
        CHECK_FALSE(hog::flora::plant(spot, woods_of(0.0, shares), planting).has_value());
        const auto whole = hog::flora::plant(spot, woods_of(1.0, shares), planting);
        REQUIRE(whole.has_value());
        ++full;
        if (const auto thin = hog::flora::plant(spot, woods_of(0.5, shares), planting)) {
            ++half;
            CHECK(thin->species == whole.value().species);
            CHECK(thin->variant == whole.value().variant);
        }
    }
    CHECK(std::abs((static_cast<double>(half) / static_cast<double>(full)) - 0.5) < 0.02);
}

TEST_CASE("kinds stand by their shares, and kinds with no species borrow one",
          "[flora][placement]") {
    const Planting planting = three();
    std::array<std::size_t, 3> chosen{};
    std::size_t borrowed = 0;
    const auto spots = many(20000);
    for (const Spot& spot : spots) {
        const auto tree =
            hog::flora::plant(spot, woods_of(1.0, {0.25, 0.75, 0, 0, 0, 0}), planting);
        REQUIRE(tree.has_value());
        ++chosen.at(tree.value().species);
        CHECK(tree.value().variant < planting.species.at(tree.value().species).variants);
        CHECK(tree.value().turn >= 0.0F);
        CHECK(tree.value().turn < 1.0F);
        CHECK(tree.value().scale >= 0.85F);
        CHECK(tree.value().scale <= 1.15F);
        CHECK(std::abs(tree.value().lean.x) <= 0.04F);
        CHECK(std::abs(tree.value().lean.y) <= 0.04F);
        CHECK(std::abs(tree.value().tint) <= 1.0F);
        const auto palm = hog::flora::plant(spot, woods_of(1.0, {0, 0, 0, 0, 1.0, 0}), planting);
        REQUIRE(palm.has_value());
        borrowed += planting.species.at(palm.value().species).kind == Kind::Deciduous ? 1U : 0U;
    }
    const auto share = [&](std::size_t n) {
        return static_cast<double>(n) / static_cast<double>(spots.size());
    };
    CHECK(std::abs(share(chosen[0]) - 0.25) < 0.02);
    CHECK(std::abs(share(chosen[1]) - 0.375) < 0.02);
    CHECK(std::abs(share(chosen[2]) - 0.375) < 0.02);
    CHECK(borrowed == spots.size());
}

// Server and clients must plant the same trees, so what a hash plants is fixed.
TEST_CASE("what a spot plants never changes", "[flora][placement]") {
    const Planting planting = three();
    std::uint64_t seen = 0;
    for (const Spot& spot : many(256)) {
        const auto tree =
            hog::flora::plant(spot, woods_of(0.6, {0.3, 0.5, 0.2, 0, 0, 0}), planting);
        if (!tree) {
            seen = hog::hash_values(seen, 0U);
            continue;
        }
        seen =
            hog::hash_values(seen, tree.value().species, tree.value().variant, tree.value().season,
                             static_cast<std::int64_t>(std::lround(tree.value().turn * 1e6F)),
                             static_cast<std::int64_t>(std::lround(tree.value().scale * 1e6F)),
                             static_cast<std::int64_t>(std::lround(tree.value().tint * 1e6F)));
    }
    CHECK(seen == 0x558d40150852e8aeU);
}

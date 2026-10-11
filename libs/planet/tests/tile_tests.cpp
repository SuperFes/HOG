#include <hog/planet/tile.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

using hog::DVec3;
using hog::planet::CellId;
using hog::planet::max_level;
using hog::planet::NoisePoint;
using hog::planet::sample_tile;
using hog::planet::tile_hash;

namespace {

const auto identity = [](CellId cell) { return cell; };

std::uint64_t hash_of(std::vector<std::int64_t> samples) {
    return tile_hash(std::span<const std::int64_t>{samples});
}

} // namespace

TEST_CASE("a tile of depth zero is its own sample", "[planet][tile]") {
    const CellId tile = CellId::from_face(4).child(2).child(1);
    CHECK(sample_tile(tile, 0, identity) == std::vector{tile});
}

TEST_CASE("tile samples run row by row over the tile's descendants", "[planet][tile]") {
    const CellId tile = CellId::from_face_ij(3, 5, 6, 3);
    constexpr int depth = 2;
    constexpr std::uint32_t side = 4;
    const auto samples = sample_tile(tile, depth, identity);
    REQUIRE(samples.size() == side * side);

    std::set<CellId> distinct;
    for (std::uint32_t j = 0; j < side; ++j) {
        for (std::uint32_t i = 0; i < side; ++i) {
            const CellId cell = samples[(j * side) + i];
            CHECK(cell.to_face_ij() ==
                  hog::planet::FaceIJ{
                      .face = 3, .i = (5 * side) + i, .j = (6 * side) + j, .level = 5});
            CHECK(tile.contains(cell));
            distinct.insert(cell);
        }
    }
    CHECK(distinct.size() == samples.size());
}

TEST_CASE("tile samples carry the sampler's result type", "[planet][tile]") {
    const auto levels =
        sample_tile(CellId::from_face(0), 1, [](CellId cell) { return cell.level(); });
    CHECK(levels == std::vector{1, 1, 1, 1});
}

TEST_CASE("tile hashes depend on every value, the order and the count", "[planet][tile]") {
    const std::uint64_t base = hash_of({1, 2, 3});
    CHECK(base == hash_of({1, 2, 3}));
    CHECK(base != hash_of({1, 2, 4}));
    CHECK(base != hash_of({2, 1, 3}));
    CHECK(base != hash_of({1, 2, 3, 0}));
    CHECK(hash_of({}) != hash_of({0}));

    static constexpr std::array<std::int32_t, 2> constant{7, -7};
    // Usable at compile time.
    static_assert(tile_hash(std::span<const std::int32_t>{constant}) != 0);
}

TEST_CASE("noise points scale positions into fixed point", "[planet][tile]") {
    using hog::planet::noise_point;
    const NoisePoint point = noise_point(DVec3{0.5, -1.25, 2.0}, 2.0);
    CHECK(point.x == hog::planet::fixed_one);
    CHECK(point.y == -5 * hog::planet::fixed_one / 2);
    CHECK(point.z == 4 * hog::planet::fixed_one);

    // Halfway cases round away from zero.
    const double half_step = 1.5 / static_cast<double>(hog::planet::fixed_one);
    const NoisePoint rounded = noise_point(DVec3{half_step, -half_step, 0.0}, 1.0);
    CHECK(rounded.x == 2);
    CHECK(rounded.y == -2);
    CHECK(rounded.z == 0);
}

TEST_CASE("tile preconditions", "[planet][tile]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    const CellId face = CellId::from_face(0);
    CHECK_THROWS_AS(sample_tile(CellId{}, 0, identity), AssertionFailed);
    CHECK_THROWS_AS(sample_tile(face, -1, identity), AssertionFailed);
    CHECK_THROWS_AS(sample_tile(face, hog::planet::max_tile_depth + 1, identity), AssertionFailed);
    CHECK_THROWS_AS(sample_tile(CellId::from_face_ij(0, 0, 0, max_level - 1), 2, identity),
                    AssertionFailed);
    CHECK_THROWS_AS(hog::planet::noise_point(DVec3{1.0, 0.0, 0.0}, 1 << 24), AssertionFailed);
}

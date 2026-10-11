#include <hog/planet/cell_id.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <random>
#include <vector>

using hog::planet::CellId;
using hog::planet::FaceIJ;
using hog::planet::max_level;

static_assert(CellId::from_face(0).is_valid());
static_assert(CellId::from_face(5).level() == 0);
static_assert(CellId::from_face_ij(2, 3, 1, 2).to_face_ij() ==
              FaceIJ{.face = 2, .i = 3, .j = 1, .level = 2});
static_assert(CellId::from_face(4).child(3).parent() == CellId::from_face(4));
static_assert(!CellId{}.is_valid());

TEST_CASE("face cells are the six roots", "[planet][cell]") {
    for (int face = 0; face < hog::planet::face_count; ++face) {
        const CellId cell = CellId::from_face(face);
        CHECK(cell.is_valid());
        CHECK(cell.face() == face);
        CHECK(cell.level() == 0);
        CHECK(cell.is_face());
    }
    CHECK(CellId::from_face(0) < CellId::from_face(1));
}

TEST_CASE("invalid raw values are rejected", "[planet][cell]") {
    CHECK_FALSE(CellId{}.is_valid());
    CHECK_FALSE(CellId::from_raw(0).is_valid());
    // Face 6 does not exist.
    CHECK_FALSE(CellId::from_raw((std::uint64_t{6} << 61) | (std::uint64_t{1} << 60)).is_valid());
    // A marker on an odd bit, or above a face cell's marker, is not a level.
    CHECK_FALSE(CellId::from_raw(std::uint64_t{1} << 1).is_valid());
    CHECK_FALSE(CellId::from_raw(std::uint64_t{1} << 61).is_valid());
    CHECK(CellId::from_raw(CellId::from_face_ij(3, 5, 9, 4).raw()).is_valid());
}

TEST_CASE("face, i, j and level round-trip exhaustively at low levels", "[planet][cell]") {
    for (int level = 0; level <= 5; ++level) {
        const std::uint32_t size = 1U << level;
        for (int face = 0; face < hog::planet::face_count; ++face) {
            for (std::uint32_t i = 0; i < size; ++i) {
                for (std::uint32_t j = 0; j < size; ++j) {
                    const FaceIJ expected{.face = face, .i = i, .j = j, .level = level};
                    const CellId cell = CellId::from_face_ij(expected);
                    REQUIRE(cell.is_valid());
                    REQUIRE(cell.to_face_ij() == expected);
                }
            }
        }
    }
}

TEST_CASE("face, i, j and level round-trip at every level", "[planet][cell]") {
    std::mt19937_64 rng{42};
    for (int level = 0; level <= max_level; ++level) {
        const std::uint32_t mask = level == 0 ? 0 : (~0U >> (32 - level));
        for (int sample = 0; sample < 200; ++sample) {
            const FaceIJ expected{.face = static_cast<int>(rng() % 6),
                                  .i = static_cast<std::uint32_t>(rng()) & mask,
                                  .j = static_cast<std::uint32_t>(rng()) & mask,
                                  .level = level};
            REQUIRE(CellId::from_face_ij(expected).to_face_ij() == expected);
        }
    }

    const std::uint32_t last = (1U << max_level) - 1;
    const CellId corner = CellId::from_face_ij(5, last, last, max_level);
    CHECK(corner.is_leaf());
    CHECK(corner.to_face_ij() == FaceIJ{.face = 5, .i = last, .j = last, .level = max_level});
    CHECK(corner == CellId::from_face(5).range_max());
}

TEST_CASE("children follow the curve and halve i and j", "[planet][cell]") {
    const CellId parent = CellId::from_face_ij(1, 2, 3, 2);
    for (int index = 0; index < 4; ++index) {
        const CellId child = parent.child(index);
        CHECK(child.level() == 3);
        CHECK(child.parent() == parent);
        const FaceIJ ij = child.to_face_ij();
        CHECK(ij.i == (2 * 2) + static_cast<std::uint32_t>(index >> 1));
        CHECK(ij.j == (3 * 2) + static_cast<std::uint32_t>(index & 1));
    }
    CHECK(parent.child(0) < parent.child(1));
    CHECK(parent.child(2) < parent.child(3));
}

TEST_CASE("parent at a level matches repeated parent()", "[planet][cell]") {
    const CellId leaf = CellId::from_face_ij(4, 123'456'789, 987'654'321, max_level);
    CellId walked = leaf;
    for (int level = max_level; level >= 0; --level) {
        REQUIRE(leaf.parent(level) == walked);
        REQUIRE(walked.level() == level);
        if (level > 0) {
            walked = walked.parent();
        }
    }
    CHECK(walked == CellId::from_face(4));
}

TEST_CASE("descendants lie in one contiguous range", "[planet][cell]") {
    const CellId cell = CellId::from_face_ij(2, 5, 6, 3);
    std::vector<CellId> descendants;
    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            descendants.push_back(cell.child(a).child(b));
        }
    }

    CHECK(std::ranges::is_sorted(descendants));
    for (const CellId descendant : descendants) {
        CHECK(cell.contains(descendant));
        CHECK(descendant.range_min() >= cell.range_min());
        CHECK(descendant.range_max() <= cell.range_max());
    }
    CHECK(cell.contains(cell));
    CHECK(cell.range_min().is_leaf());
    CHECK(cell.range_max().is_leaf());

    // Siblings don't overlap.
    CHECK(cell.child(0).range_max() < cell.child(1).range_min());
    CHECK_FALSE(cell.child(0).contains(cell.child(1)));
    CHECK_FALSE(cell.child(0).contains(cell));
    CHECK_FALSE(CellId::from_face(0).contains(CellId::from_face(1).range_min()));
}

TEST_CASE("cells print as face and child digits", "[planet][cell]") {
    CHECK(hog::planet::to_string(CellId::from_face(3)) == "3/");
    CHECK(hog::planet::to_string(CellId::from_face(3).child(2).child(1)) == "3/21");
    CHECK(hog::planet::to_string(CellId{}) == "invalid");
    CHECK(std::format("{:>6}", CellId::from_face(0).child(0)) == "   0/0");
}

TEST_CASE("cell operations check their preconditions", "[planet][cell]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;

    CHECK_THROWS_AS(CellId::from_face(6), AssertionFailed);
    CHECK_THROWS_AS(CellId::from_face_ij(0, 0, 0, max_level + 1), AssertionFailed);
    CHECK_THROWS_AS(CellId::from_face_ij(0, 4, 0, 2), AssertionFailed);
    CHECK_THROWS_AS(CellId::from_face(0).parent(), AssertionFailed);
    CHECK_THROWS_AS(CellId::from_face(0).range_min().child(0), AssertionFailed);
    CHECK_THROWS_AS(CellId::from_face(0).child(4), AssertionFailed);
    CHECK_THROWS_AS(CellId::from_face(0).child(0).parent(2), AssertionFailed);
    CHECK_THROWS_AS(CellId{}.level(), AssertionFailed);
}

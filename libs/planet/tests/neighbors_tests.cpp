#include <hog/planet/cube_face.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

using hog::planet::CellId;
using hog::planet::face_count;
using hog::planet::FaceIJ;

namespace {

std::vector<CellId> all_cells(int level) {
    std::vector<CellId> cells;
    const std::uint32_t size = 1U << level;
    for (int face = 0; face < face_count; ++face) {
        for (std::uint32_t i = 0; i < size; ++i) {
            for (std::uint32_t j = 0; j < size; ++j) {
                cells.push_back(CellId::from_face_ij(face, i, j, level));
            }
        }
    }
    return cells;
}

template <typename Range>
bool holds(const Range& range, CellId cell) {
    return std::ranges::find(range, cell) != std::ranges::end(range);
}

} // namespace

TEST_CASE("face frames are right-handed and cover every axis direction", "[planet][face]") {
    std::set<std::pair<int, int>> normals;
    for (int face = 0; face < face_count; ++face) {
        const auto frame = hog::planet::face_frame(face);
        CHECK(hog::planet::face_from_normal(frame.normal) == face);
        normals.emplace(frame.normal.index, frame.normal.sign);

        // u x v == normal for signed unit axes.
        CHECK(frame.u.index != frame.v.index);
        const int third = 3 - frame.u.index - frame.v.index;
        CHECK(third == frame.normal.index);
        const bool cyclic = (frame.u.index + 1) % 3 == frame.v.index;
        const int cross_sign = frame.u.sign * frame.v.sign * (cyclic ? 1 : -1);
        CHECK(cross_sign == frame.normal.sign);
    }
    CHECK(normals.size() == 6);
}

TEST_CASE("edge neighbours are symmetric and distinct at every low level", "[planet][neighbors]") {
    for (int level = 0; level <= 4; ++level) {
        for (const CellId cell : all_cells(level)) {
            const auto neighbors = hog::planet::edge_neighbors(cell);
            const std::set<CellId> distinct(neighbors.begin(), neighbors.end());
            REQUIRE(distinct.size() == 4);
            REQUIRE_FALSE(distinct.contains(cell));
            for (const CellId neighbor : neighbors) {
                REQUIRE(neighbor.level() == level);
                REQUIRE(holds(hog::planet::edge_neighbors(neighbor), cell));
            }
        }
    }
}

TEST_CASE("vertex neighbours are symmetric, and seven at cube corners", "[planet][neighbors]") {
    for (int level = 0; level <= 4; ++level) {
        std::size_t seven = 0;
        for (const CellId cell : all_cells(level)) {
            const auto neighbors = hog::planet::vertex_neighbors(cell);
            const std::set<CellId> distinct(neighbors.cells().begin(), neighbors.cells().end());
            REQUIRE(distinct.size() == neighbors.size());
            REQUIRE_FALSE(distinct.contains(cell));
            for (const CellId edge : hog::planet::edge_neighbors(cell)) {
                REQUIRE(distinct.contains(edge));
            }
            for (const CellId neighbor : neighbors.cells()) {
                REQUIRE(holds(hog::planet::vertex_neighbors(neighbor).cells(), cell));
            }

            if (level == 0) {
                REQUIRE(neighbors.size() == 4);
            } else {
                REQUIRE((neighbors.size() == 8 || neighbors.size() == 7));
                seven += neighbors.size() == 7 ? 1 : 0;
            }
        }
        // Eight cube corners, three faces each.
        CHECK(seven == (level == 0 ? 0 : 24));
    }
}

TEST_CASE("each face borders the four faces that are not itself or its opposite",
          "[planet][neighbors]") {
    for (int face = 0; face < face_count; ++face) {
        const auto neighbors = hog::planet::edge_neighbors(CellId::from_face(face));
        const int opposite = (face + 3) % face_count;
        for (const CellId neighbor : neighbors) {
            CHECK(neighbor.face() != face);
            CHECK(neighbor.face() != opposite);
        }
    }
}

TEST_CASE("crossing the +u edge of the +x face lands on the +y face", "[planet][neighbors]") {
    constexpr int level = 3;
    constexpr std::uint32_t last = (1U << level) - 1;
    const CellId cell = CellId::from_face_ij(0, last, 5, level);

    // +y's u axis is +z and its v axis is +x, so z carries over and x sits just inside the edge.
    CHECK(hog::planet::edge_neighbors(cell)[1].to_face_ij() ==
          FaceIJ{.face = 1, .i = 5, .j = last, .level = level});
    CHECK(hog::planet::edge_neighbors(cell)[0] == CellId::from_face_ij(0, last - 1, 5, level));
}

TEST_CASE("neighbours work at the deepest level", "[planet][neighbors]") {
    constexpr std::uint32_t last = (1U << hog::planet::max_level) - 1;
    const CellId corner = CellId::from_face_ij(3, last, last, hog::planet::max_level);
    CHECK(hog::planet::vertex_neighbors(corner).size() == 7);
    for (const CellId neighbor : hog::planet::edge_neighbors(corner)) {
        CHECK(neighbor.is_leaf());
        CHECK(holds(hog::planet::edge_neighbors(neighbor), corner));
    }
}

TEST_CASE("neighbour lookups reject invalid cells", "[planet][neighbors]") {
    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(hog::planet::edge_neighbors(CellId{}), hog::testing::AssertionFailed);
    CHECK_THROWS_AS(hog::planet::vertex_neighbors(CellId{}), hog::testing::AssertionFailed);
}

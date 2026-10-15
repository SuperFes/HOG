#include <hog/planet/neighbors.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/terrain/stitching.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::planet::CellId;
using hog::terrain::Corner;
using hog::terrain::patch_cells;
using hog::terrain::patch_depth;
using hog::terrain::patch_indices;
using hog::terrain::patch_side;
using hog::terrain::SelectedPatch;

namespace {

constexpr int last = patch_cells;

struct GridPoint {
    int i;
    int j;
};

GridPoint grid_point(std::uint16_t vertex) {
    return {.i = vertex % patch_side, .j = vertex / patch_side};
}

// The patch edge both ends of a segment lie along, if one.
std::optional<int> edge_along(std::uint16_t a, std::uint16_t b) {
    const GridPoint p = grid_point(a);
    const GridPoint q = grid_point(b);
    if (p.i == 0 && q.i == 0) {
        return 0;
    }
    if (p.i == last && q.i == last) {
        return 1;
    }
    if (p.j == 0 && q.j == 0) {
        return 2;
    }
    if (p.j == last && q.j == last) {
        return 3;
    }
    return std::nullopt;
}

struct BorderSegment {
    int edge;
    std::uint16_t a;
    std::uint16_t b;
};

// The segments of a stitching's outline: those only one of its triangles has.
std::vector<BorderSegment> outline(std::uint8_t edges) {
    const auto indices = patch_indices(edges);
    std::map<std::pair<std::uint16_t, std::uint16_t>, int> uses;
    for (std::size_t first = 0; first < indices.size(); first += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            ++uses[std::minmax(indices[first + k], indices[first + ((k + 1) % 3)])];
        }
    }
    std::vector<BorderSegment> segments;
    for (const auto& [ends, count] : uses) {
        if (count == 1) {
            segments.push_back({.edge = edge_along(ends.first, ends.second).value_or(-1),
                                .a = ends.first,
                                .b = ends.second});
        }
    }
    return segments;
}

Corner corner_of(CellId cell, std::uint16_t vertex) {
    const hog::planet::FaceIJ at = cell.to_face_ij();
    const GridPoint point = grid_point(vertex);
    return {.face = at.face,
            .i = (std::int64_t{at.i} << patch_depth) + point.i,
            .j = (std::int64_t{at.j} << patch_depth) + point.j,
            .level = at.level + patch_depth};
}

// A vertex as every patch naming its point names it, named at a `level` no finer than 29 and
// no coarser than the patch's vertices.
std::uint64_t key_of(CellId cell, std::uint16_t vertex, int level) {
    Corner corner = corner_of(cell, vertex);
    const int shift = level - corner.level;
    corner = hog::terrain::canonical(
        {.face = corner.face, .i = corner.i << shift, .j = corner.j << shift, .level = level});
    return (static_cast<std::uint64_t>(corner.face) << 60U) |
           (static_cast<std::uint64_t>(corner.i) << 30U) | static_cast<std::uint64_t>(corner.j);
}

std::optional<SelectedPatch> sorted_over(const std::vector<SelectedPatch>& by_cell, CellId cell) {
    const auto after = std::ranges::lower_bound(by_cell, cell, {}, &SelectedPatch::cell);
    if (after != by_cell.end() && after->cell.contains(cell)) {
        return *after;
    }
    if (after != by_cell.begin() && std::prev(after)->cell.contains(cell)) {
        return *std::prev(after);
    }
    return std::nullopt;
}

// Outline segments facing a drawn neighbour at the same level or coarser that it doesn't share.
// Finer neighbours are checked from their own side.
int cracks(const std::vector<SelectedPatch>& drawn, bool stitched) {
    static const auto outlines = [] {
        std::array<std::vector<BorderSegment>, hog::terrain::stitchings> built;
        for (std::size_t edges = 0; edges < built.size(); ++edges) {
            built.at(edges) = outline(static_cast<std::uint8_t>(edges));
        }
        return built;
    }();
    auto by_cell = drawn;
    std::ranges::sort(by_cell, {}, &SelectedPatch::cell);
    int level = 0;
    for (const SelectedPatch& patch : drawn) {
        level = std::max(level, patch.cell.level() + patch_depth);
    }
    REQUIRE(level <= 29);

    using Segment = std::pair<std::uint64_t, std::uint64_t>;
    std::vector<Segment> everywhere;
    std::vector<Segment> facing;
    for (const SelectedPatch& patch : drawn) {
        const auto neighbours = hog::planet::edge_neighbors(patch.cell);
        std::array<bool, 4> beside{};
        for (std::size_t edge = 0; edge < beside.size(); ++edge) {
            beside.at(edge) = sorted_over(by_cell, neighbours.at(edge)).has_value();
        }
        const std::uint8_t edges = stitched ? patch.coarser_edges : 0;
        for (const BorderSegment& segment : outlines.at(edges)) {
            const Segment named = std::minmax(key_of(patch.cell, segment.a, level),
                                              key_of(patch.cell, segment.b, level));
            everywhere.push_back(named);
            if (beside.at(static_cast<std::size_t>(segment.edge))) {
                facing.push_back(named);
            }
        }
    }
    std::ranges::sort(everywhere);
    int unshared = 0;
    for (const Segment& segment : facing) {
        const auto [first, end] = std::ranges::equal_range(everywhere, segment);
        unshared += std::distance(first, end) == 2 ? 0 : 1;
    }
    return unshared;
}

hog::terrain::Viewpoint looking(DVec3 eye, DVec3 forward) {
    forward = glm::normalize(forward);
    const DVec3 up = std::abs(forward.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    return {.eye = hog::WorldPosition{eye}, .forward = forward, .up = up};
}

} // namespace

TEST_CASE("every stitching covers the patch once, wound alike", "[terrain][stitching]") {
    for (int edges = 0; edges < hog::terrain::stitchings; ++edges) {
        const auto indices = patch_indices(static_cast<std::uint8_t>(edges));
        REQUIRE(indices.size() % 3 == 0);
        // Each coarser edge trades 31 triangles for 15 larger ones.
        CHECK(std::ssize(indices) / 3 ==
              (2 * last * last) - (16 * std::popcount(static_cast<unsigned>(edges))));
        int twice_area = 0;
        std::set<std::pair<std::uint16_t, std::uint16_t>> directed;
        std::set<std::uint16_t> used;
        for (std::size_t first = 0; first < indices.size(); first += 3) {
            const GridPoint a = grid_point(indices[first]);
            const GridPoint b = grid_point(indices[first + 1]);
            const GridPoint c = grid_point(indices[first + 2]);
            const int turn = ((b.i - a.i) * (c.j - a.j)) - ((b.j - a.j) * (c.i - a.i));
            CHECK(turn > 0);
            twice_area += turn;
            for (std::size_t k = 0; k < 3; ++k) {
                used.insert(indices[first + k]);
                CHECK(directed.insert({indices[first + k], indices[first + ((k + 1) % 3)]}).second);
            }
        }
        CHECK(twice_area == 2 * last * last);
        // Inside, every side is met by a neighbour running the other way.
        for (const auto& [from, to] : directed) {
            if (!directed.contains({to, from})) {
                CHECK(edge_along(from, to).has_value());
            }
        }
        // Coarser edges leave their odd vertices out, and nothing else is left out.
        int unused = 0;
        for (int edge = 0; edge < 4; ++edge) {
            unused += ((static_cast<unsigned>(edges) >> static_cast<unsigned>(edge)) & 1U) != 0
                          ? last / 2
                          : 0;
        }
        CHECK(std::ssize(used) == (patch_side * patch_side) - unused);
        for (const std::uint16_t vertex : used) {
            const GridPoint point = grid_point(vertex);
            const bool odd_on_coarser =
                ((edges & hog::terrain::edge_minus_u) != 0 && point.i == 0 && point.j % 2 != 0) ||
                ((edges & hog::terrain::edge_plus_u) != 0 && point.i == last && point.j % 2 != 0) ||
                ((edges & hog::terrain::edge_minus_v) != 0 && point.j == 0 && point.i % 2 != 0) ||
                ((edges & hog::terrain::edge_plus_v) != 0 && point.j == last && point.i % 2 != 0);
            CHECK(!odd_on_coarser);
        }
    }
}

TEST_CASE("patches face outwards on every face", "[terrain][stitching]") {
    for (int face = 0; face < hog::planet::face_count; ++face) {
        for (const CellId cell :
             {CellId::from_face_ij(face, 3, 9, 4), CellId::from_face_ij(face, 0, 0, 4)}) {
            for (const std::uint8_t edges : {std::uint8_t{0}, std::uint8_t{15}}) {
                const auto indices = patch_indices(edges);
                for (std::size_t first = 0; first < indices.size(); first += 3) {
                    const DVec3 a = hog::terrain::direction(corner_of(cell, indices[first]));
                    const DVec3 b = hog::terrain::direction(corner_of(cell, indices[first + 1]));
                    const DVec3 c = hog::terrain::direction(corner_of(cell, indices[first + 2]));
                    CHECK(glm::dot(glm::cross(b - a, c - a), a) > 0.0);
                }
            }
        }
    }
}

TEST_CASE("selected patches meet without cracks", "[terrain][stitching]") {
    constexpr double radius = 115'000.0;
    std::mt19937_64 random{11};
    std::uniform_real_distribution<double> unit{-1.0, 1.0};
    const auto any_direction = [&] {
        DVec3 direction{};
        do {
            direction = {unit(random), unit(random), unit(random)};
        } while (glm::length(direction) < 0.1);
        return glm::normalize(direction);
    };
    for (int view = 0; view < 6; ++view) {
        const double height = std::pow(10.0, 1.0 + (2.5 * (unit(random) + 1.0)));
        const DVec3 up = any_direction();
        const DVec3 eye = up * (radius + height);
        const DVec3 target = glm::normalize(up + (0.3 * any_direction())) * radius;
        const auto drawn = hog::terrain::select_patches(looking(eye, target - eye), radius);
        REQUIRE(!drawn.empty());
        CHECK(cracks(drawn, true) == 0);
        // Left unstitched, the finer side of a seam would crack.
        if (view == 0) {
            CHECK(cracks(drawn, false) > 0);
        }
    }
}

TEST_CASE("every stitching's skirts hang from its outline", "[terrain][stitching]") {
    // The edge and place along it of the vertex a skirt vertex hangs under.
    const auto above = [](std::uint16_t vertex) {
        const int skirt = vertex - hog::terrain::patch_grid_vertices;
        const int edge = skirt / patch_side;
        const auto point = hog::terrain::on_edge(edge, skirt % patch_side);
        return std::pair{edge, static_cast<std::uint16_t>((point.j * patch_side) + point.i)};
    };
    for (int edges = 0; edges < hog::terrain::stitchings; ++edges) {
        const auto indices = hog::terrain::skirt_indices(static_cast<std::uint8_t>(edges));
        REQUIRE(indices.size() % 3 == 0);
        std::multiset<std::pair<std::uint16_t, std::uint16_t>> hung;
        std::multiset<std::pair<std::uint16_t, std::uint16_t>> under;
        for (std::size_t first = 0; first < indices.size(); first += 3) {
            std::vector<std::uint16_t> tops;
            std::vector<std::uint16_t> bottoms;
            for (std::size_t k = 0; k < 3; ++k) {
                const std::uint16_t vertex = indices[first + k];
                REQUIRE(vertex < hog::terrain::patch_vertices);
                (vertex < hog::terrain::patch_grid_vertices ? tops : bottoms).push_back(vertex);
            }
            REQUIRE((tops.size() == 1 || tops.size() == 2));
            auto& ends = tops.size() == 2 ? hung : under;
            const auto& pair = tops.size() == 2 ? tops : bottoms;
            const std::uint16_t lone = tops.size() == 2 ? bottoms[0] : tops[0];
            const auto [a, b] = std::minmax(pair[0], pair[1]);
            if (tops.size() == 2) {
                ends.insert({a, b});
                // The bottom corner hangs under one of the top two, on their edge.
                const auto [edge, over] = above(lone);
                CHECK((over == a || over == b));
                CHECK(edge_along(a, b) == edge);
            } else {
                const auto [edge_a, over_a] = above(a);
                const auto [edge_b, over_b] = above(b);
                CHECK(edge_a == edge_b);
                ends.insert(std::minmax(over_a, over_b));
                CHECK((lone == over_a || lone == over_b));
            }
        }
        // A quad hangs from each outline segment, and from nothing else.
        std::multiset<std::pair<std::uint16_t, std::uint16_t>> segments;
        for (const BorderSegment& segment : outline(static_cast<std::uint8_t>(edges))) {
            segments.insert({segment.a, segment.b});
        }
        CHECK(hung == segments);
        CHECK(under == segments);
    }
}

TEST_CASE("a stitching beyond four edges is refused", "[terrain][stitching]") {
    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(patch_indices(hog::terrain::stitchings), hog::testing::AssertionFailed);
    CHECK_THROWS_AS(hog::terrain::skirt_indices(hog::terrain::stitchings),
                    hog::testing::AssertionFailed);
}

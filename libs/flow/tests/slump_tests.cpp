#include <hog/flow/slump.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace {

using hog::flow::Slump;
using hog::globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// 33 degrees over a column as wide as a voxel is tall.
const auto rise = static_cast<std::int64_t>(std::tan(33.0 * 3.14159265358979 / 180.0) * full);

// A square patch of columns linked to their eight neighbours.
struct Grid {
    int width;
    int depth;
    Slump slump;

    Grid(int w, int d) : width{w}, depth{d}, slump(static_cast<std::size_t>(w * d)) {
        constexpr std::array<std::array<int, 2>, 8> sides{
            {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}};
        for (int j = 0; j < d; ++j) {
            for (int i = 0; i < w; ++i) {
                Slump::Node& node = at(i, j);
                node.rise = rise;
                for (std::size_t s = 0; s < sides.size(); ++s) {
                    const int ni = i + sides[s][0];
                    const int nj = j + sides[s][1];
                    if (ni >= 0 && nj >= 0 && ni < w && nj < d) {
                        node.next[s] = (nj * w) + ni;
                    }
                }
            }
        }
    }
    Slump::Node& at(int i, int j) { return slump[static_cast<std::size_t>((j * width) + i)]; }
    std::int64_t surface(int i, int j) { return at(i, j).ground + at(i, j).grain; }
};

int settle(Grid& grid, int most = 20'000) {
    int steps = 0;
    while (steps < most && grid.slump.step() > 0) {
        ++steps;
    }
    return steps;
}

} // namespace

TEST_CASE("a pile on level ground spreads without losing any grain, then sleeps", "[slump]") {
    Grid grid{41, 41};
    grid.at(20, 20).grain = 60 * full;
    const std::int64_t before = grid.slump.total();

    const int steps = settle(grid);

    CHECK(steps < 20'000);
    CHECK(grid.slump.total() == before);
    CHECK(grid.slump.step() == 0);
    CHECK(grid.at(20, 20).grain < 60 * full);
}

TEST_CASE("a settled heap holds no steeper than its repose, any way across", "[slump]") {
    Grid grid{41, 41};
    grid.at(20, 20).grain = 60 * full;
    settle(grid);

    const auto diagonal =
        static_cast<std::int64_t>(std::llround(static_cast<double>(rise) * std::sqrt(2.0)));
    // Whole-unit steps leave a hair over, never a voxel's worth.
    const std::int64_t slack = full / 8;
    for (int j = 1; j < 40; ++j) {
        for (int i = 1; i < 40; ++i) {
            CHECK(grid.surface(i, j) - grid.surface(i + 1, j) <= rise + slack);
            CHECK(grid.surface(i, j) - grid.surface(i + 1, j + 1) <= diagonal + slack);
        }
    }
}

TEST_CASE("a heap comes to a cone, as far along a diagonal as along an axis", "[slump]") {
    Grid grid{61, 61};
    grid.at(30, 30).grain = 400 * full;
    settle(grid);

    // The heap's reach in true distance, where grain still lies.
    auto reach = [&](int di, int dj) {
        int steps = 0;
        while (grid.at(30 + ((steps + 1) * di), 30 + ((steps + 1) * dj)).grain > 0) {
            ++steps;
        }
        return steps * std::hypot(di, dj);
    };
    const double along = reach(1, 0);
    const double across = reach(1, 1);
    CHECK(along > 5.0);
    CHECK(across == Catch::Approx(along).epsilon(0.2));
}

TEST_CASE("grain on a slope too steep to hold runs downhill and stops where it can lie",
          "[slump]") {
    Grid grid{30, 5};
    // A bank of ground four voxels high at the left, a low floor to the right.
    for (int j = 0; j < 5; ++j) {
        for (int i = 0; i < 30; ++i) {
            grid.at(i, j).ground = i < 5 ? 4 * full : 0;
        }
        grid.at(5, j).grain = 2 * full;
    }
    const std::int64_t before = grid.slump.total();
    settle(grid);
    CHECK(grid.slump.total() == before);
    CHECK(grid.at(20, 2).grain == 0);
}

TEST_CASE("a column with no room takes none, and the grain stays where it was", "[slump]") {
    Grid grid{3, 1};
    grid.at(0, 0).grain = 10 * full;
    grid.at(1, 0).room = 0;
    grid.at(2, 0).room = 0;
    const std::int64_t before = grid.slump.total();
    settle(grid);
    CHECK(grid.slump.total() == before);
    CHECK(grid.at(0, 0).grain == 10 * full);
}

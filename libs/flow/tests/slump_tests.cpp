#include <hog/flow/slump.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

namespace {

using hog::flow::Slump;
using hog::globe::Voxel;

constexpr std::int64_t full = Voxel::full;
// 33 degrees over a column as wide as a voxel is tall.
const auto rise = static_cast<std::int64_t>(std::tan(33.0 * 3.14159265358979 / 180.0) * full);

int settle(Slump& slump, int most = 20'000) {
    int steps = 0;
    while (steps < most && slump.step(rise) > 0) {
        ++steps;
    }
    return steps;
}

} // namespace

TEST_CASE("a pile on level ground spreads without losing any grain, then sleeps", "[slump]") {
    Slump slump{41, 41};
    slump.grain(20, 20) = 60 * full;
    const std::int64_t before = slump.total();

    const int steps = settle(slump);

    CHECK(steps < 20'000);
    CHECK(slump.total() == before);
    CHECK(slump.step(rise) == 0);
    CHECK(slump.grain(20, 20) < 60 * full);
}

TEST_CASE("a settled heap holds no steeper than its repose, any way across", "[slump]") {
    Slump slump{41, 41};
    slump.grain(20, 20) = 60 * full;
    settle(slump);

    const auto diagonal = static_cast<std::int64_t>(std::llround(static_cast<double>(rise) * std::sqrt(2.0)));
    // Whole-unit steps leave a hair over, never a voxel's worth.
    const std::int64_t slack = full / 8;
    for (int j = 1; j < 40; ++j) {
        for (int i = 1; i < 40; ++i) {
            CHECK(slump.surface(i, j) - slump.surface(i + 1, j) <= rise + slack);
            CHECK(slump.surface(i, j) - slump.surface(i + 1, j + 1) <= diagonal + slack);
        }
    }
}

TEST_CASE("a heap comes to a cone, as far along a diagonal as along an axis", "[slump]") {
    Slump slump{61, 61};
    slump.grain(30, 30) = 400 * full;
    settle(slump);

    // The heap's reach in true distance, where grain still lies.
    auto reach = [&](int di, int dj) {
        int steps = 0;
        while (slump.grain(30 + ((steps + 1) * di), 30 + ((steps + 1) * dj)) > 0) {
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
    Slump slump{30, 5};
    // A bank of ground four voxels high at the left, a low floor to the right.
    for (int j = 0; j < 5; ++j) {
        for (int i = 0; i < 30; ++i) {
            slump.ground(i, j) = i < 5 ? 4 * full : 0;
        }
        slump.grain(5, j) = 2 * full;
    }
    const std::int64_t before = slump.total();
    settle(slump);
    CHECK(slump.total() == before);
    CHECK(slump.grain(20, 2) == 0);
}

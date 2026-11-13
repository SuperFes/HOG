#include <hog/flow/creep.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

using hog::flow::Creep;

namespace {

constexpr int side = 24;

Creep square(Creep::Law law = {}) {
    Creep creep{static_cast<std::size_t>(side * side), law};
    const auto at = [](int x, int y) {
        return (x < 0 || y < 0 || x >= side || y >= side) ? Creep::none : (y * side) + x;
    };
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            creep[static_cast<std::size_t>(at(x, y))].next = {at(x + 1, y), at(x, y + 1),
                                                              at(x - 1, y), at(x, y - 1)};
        }
    }
    return creep;
}

std::size_t at(int x, int y) {
    return (static_cast<std::size_t>(y) * std::size_t{side}) + static_cast<std::size_t>(x);
}

void run(Creep& creep, double years) {
    for (double left = years; left > 1.0e-9;) {
        left -= creep.step(left);
    }
}

} // namespace

TEST_CASE("a heap of cover spreads out and none of it is lost", "[creep]") {
    Creep creep = square();
    creep[at(12, 12)].cover = 3.0;
    const double before = creep.total();
    run(creep, 2000.0);
    CHECK(creep.total() == Catch::Approx(before).epsilon(1e-9));
    CHECK(creep[at(12, 12)].cover < 3.0);
    CHECK(creep[at(13, 12)].cover > 0.0);
    for (std::size_t n = 0; n < creep.size(); ++n) {
        CHECK(creep[n].cover >= 0.0);
    }
}

TEST_CASE("cover leaves a steep face for the foot, and gathers in a hollow", "[creep]") {
    Creep creep = square();
    // A slope of 0.8 falling in +x, then flat; even cover over all of it.
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            creep[at(x, y)].bed = x < 12 ? 2.5 * 0.8 * (12 - x) : 0.0;
            creep[at(x, y)].cover = 1.0;
        }
    }
    run(creep, 20000.0);
    const double face = creep[at(6, 12)].cover;
    const double foot = creep[at(14, 12)].cover;
    CHECK(face < 0.5);
    CHECK(foot > 1.0);
}

TEST_CASE("a rough surface of cover comes smoother in time", "[creep]") {
    Creep creep = square();
    for (std::size_t n = 0; n < creep.size(); ++n) {
        creep[n].bed = 0.0;
        creep[n].cover = n % 2 == 0 ? 2.0 : 0.2;
    }
    const auto roughness = [&] {
        double sum = 0.0;
        for (int y = 1; y + 1 < side; ++y) {
            for (int x = 1; x + 1 < side; ++x) {
                sum += std::abs(creep.surface(at(x, y)) - creep.surface(at(x + 1, y)));
            }
        }
        return sum;
    };
    const double before = roughness();
    run(creep, 3000.0);
    CHECK(roughness() < 0.1 * before);
}

TEST_CASE("weathering makes cover from the bed, less where there is plenty", "[creep]") {
    Creep creep = square();
    creep[at(1, 1)].cover = 3.0;
    creep.weather(100.0);
    CHECK(creep[at(2, 2)].cover > 0.0);
    CHECK(creep[at(2, 2)].cover > creep[at(1, 1)].cover - 3.0);
    CHECK(creep[at(1, 1)].cover - 3.0 < creep[at(2, 2)].cover);
}

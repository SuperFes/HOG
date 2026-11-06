#include <hog/flow/glacier.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using hog::flow::Glacier;

namespace {

// A square of columns, each linked to the four beside it.
Glacier square(int size) {
    Glacier glacier{static_cast<std::size_t>(size * size), {}};
    const auto at = [&](int x, int y) { return (x < 0 || y < 0 || x >= size || y >= size) ? Glacier::none : (y * size) + x; };
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            glacier[static_cast<std::size_t>(at(x, y))].next = {at(x + 1, y), at(x, y + 1),
                                                                at(x - 1, y), at(x, y - 1)};
        }
    }
    return glacier;
}

} // namespace

TEST_CASE("ice on a slope runs downhill, keeping all of it", "[glacier]") {
    Glacier glacier = square(12);
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 12; ++x) {
            glacier[static_cast<std::size_t>((y * 12) + x)].rock = 100.0 - (5.0 * x);
        }
    }
    glacier[12 * 5 + 1].ice = 40.0;
    const double before = glacier.total();
    for (int i = 0; i < 100; ++i) {
        glacier.step(1.0);
    }
    CHECK(glacier.total() == Catch::Approx(before).epsilon(1e-9));
    double downhill = 0.0;
    for (int y = 0; y < 12; ++y) {
        for (int x = 4; x < 12; ++x) {
            downhill += glacier[static_cast<std::size_t>((y * 12) + x)].ice;
        }
        for (int x = 0; x < 12; ++x) {
            CHECK(glacier[static_cast<std::size_t>((y * 12) + x)].ice >= 0.0);
        }
    }
    CHECK(downhill > 0.0);
}

TEST_CASE("ice on level bare rock wears nothing and goes nowhere", "[glacier]") {
    Glacier glacier = square(5);
    CHECK(glacier.step(1.0) == 1.0);
    CHECK(glacier.total() == 0.0);
    CHECK(glacier[12].rock == 0.0);
}

TEST_CASE("ice wears the bed it moves over and carries its rock down", "[glacier]") {
    Glacier glacier = square(6);
    for (std::size_t n = 0; n < glacier.size(); ++n) {
        glacier[n].rock = 50.0 - (4.0 * static_cast<double>(n % 6));
    }
    glacier[6 * 2 + 1].ice = 30.0;
    glacier[6 * 2 + 1].debris = 3.0;
    const double rock = glacier[6 * 2 + 2].rock;
    for (int i = 0; i < 50; ++i) {
        glacier.step(1.0);
    }
    CHECK(glacier[6 * 2 + 2].rock < rock);
    double debris = 0.0;
    for (std::size_t n = 0; n < glacier.size(); ++n) {
        debris += glacier[n].debris;
    }
    CHECK(debris == Catch::Approx(3.0).epsilon(1e-6));
}

#include <hog/formation/ice.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/regimes.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstddef>
#include <numeric>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::formation::Body;
using hog::formation::month_middle;
using hog::formation::Months;
using hog::formation::peak_of;
using hog::formation::pooled;
using hog::formation::Pooled;
using hog::formation::Regimes;
using hog::formation::runoff_shares;

namespace {

double sum_of(const Months& series) {
    return std::accumulate(series.begin(), series.end(), 0.0);
}

double least(const Months& series) {
    return *std::ranges::min_element(series);
}
double most(const Months& series) {
    return *std::ranges::max_element(series);
}

// Northern summer's warmest, a month past the longest day.
constexpr double northern = 0.25 + (1.0 / 12.0);

} // namespace

TEST_CASE("a year's peak falls between its months, towards the higher neighbour",
          "[formation][regimes]") {
    Months even{};
    even.fill(1.0);
    even[3] = 3.0;
    CHECK_THAT(peak_of(even), WithinAbs(month_middle(3), 1e-12));
    Months leaning = even;
    leaning[4] = 2.0;
    CHECK(peak_of(leaning) > month_middle(3));
    CHECK(peak_of(leaning) < month_middle(4));
    // Round the year's end.
    Months wrapping{};
    wrapping.fill(1.0);
    wrapping[0] = 3.0;
    wrapping[11] = 2.0;
    CHECK(peak_of(wrapping) < month_middle(0));
}

TEST_CASE("with no seasons a place gives its rivers the same every month", "[formation][regimes]") {
    const Body body;
    const Months shares = runoff_shares(25.0, 0.0, northern, 1.5, false, body);
    CHECK_THAT(sum_of(shares), WithinAbs(1.0, 1e-12));
    CHECK_THAT(most(shares) - least(shares), WithinAbs(0.0, 1e-12));
}

TEST_CASE("snow holds the winter's water until the thaw", "[formation][regimes]") {
    const Body body;
    const Months shares = runoff_shares(-2.0, 18.0, northern, 0.6, false, body);
    CHECK_THAT(sum_of(shares), WithinAbs(1.0, 1e-12));
    // Nothing runs in the depths of winter, half a year from the warmest.
    CHECK(shares[9] == 0.0);
    // The melt comes in a rush as it thaws, before the warmest is past.
    const double peak = peak_of(shares);
    CHECK(peak > 0.1);
    CHECK(peak < northern + (1.0 / 12.0));
    CHECK(most(shares) > 3.0 / 12.0);
}

TEST_CASE("warm summers give more of the rain back to the air", "[formation][regimes]") {
    const Body body;
    const Months shares = runoff_shares(16.0, 9.0, northern, 0.4, false, body);
    CHECK_THAT(sum_of(shares), WithinAbs(1.0, 1e-12));
    // Lowest about the warmest, which falls between two months, highest about the coldest.
    const double low = least(shares);
    CHECK(std::min(shares[3], shares[4]) == low);
    CHECK(shares[9] > 2.0 * low);
}

TEST_CASE("ice gives its water as the summer melts it", "[formation][regimes]") {
    const Body body;
    const Months shares = runoff_shares(-6.0, 12.0, northern, 0.3, true, body);
    CHECK_THAT(sum_of(shares), WithinAbs(1.0, 1e-12));
    CHECK(shares[9] == 0.0);
    CHECK_THAT(peak_of(shares), WithinAbs(northern, 0.5 / 12.0));
}

TEST_CASE("a lake holds back what flows through it, the more the wider it is",
          "[formation][regimes]") {
    Months steady{};
    steady.fill(50.0);
    Months none{};
    const Pooled level = pooled(steady, none, 1e7, true);
    CHECK_THAT(most(level.level), WithinAbs(0.0, 1e-6));
    CHECK_THAT(level.out[6], WithinRel(50.0, 1e-6));

    // A spring flood through it.
    Months flood = steady;
    flood[1] = 300.0;
    flood[2] = 200.0;
    const Pooled small = pooled(flood, none, 1e5, true);
    const Pooled wide = pooled(flood, none, 1e9, true);
    // What goes in comes out, the year round.
    CHECK_THAT(sum_of(wide.out), WithinRel(sum_of(flood), 0.01));
    // A small one passes the flood on; a wide one spreads it over the year, rising less and later.
    CHECK(most(small.out) > 0.9 * most(flood));
    CHECK(most(wide.out) < 0.6 * most(flood));
    CHECK(most(wide.level) < most(small.level));
    CHECK(peak_of(wide.level) > peak_of(small.level));
}

TEST_CASE("a closed lake rises by what flows in over what the air takes", "[formation][regimes]") {
    Months in{};
    in.fill(10.0);
    in[2] = 40.0;
    Months loss{};
    // Taking all of it, the year round.
    loss.fill(sum_of(in) / Regimes::months / 1e8);
    const Pooled closed = pooled(in, loss, 1e8, false);
    CHECK(most(closed.out) == 0.0);
    CHECK(least(closed.level) == 0.0);
    // The month's 30 extra cubic metres a second over 1e8 square metres, about a month.
    CHECK_THAT(most(closed.level), WithinRel(30.0 * 2.63e6 / 1e8, 0.15));
}

TEST_CASE("rivers rise and fall about their year-round flow", "[formation][regimes]") {
    const Body body;
    const hog::formation::Sphere layout{5, body.seed};
    const hog::formation::Sphere sphere{6, body.seed + 1};
    const hog::formation::Plates plates{body, layout};
    const hog::formation::Crust crust = hog::formation::crust_on(sphere, plates, body);
    const hog::formation::Climate climate =
        hog::formation::climate_on(sphere, crust.base, crust.sea, body);
    const hog::formation::Rivers rivers = hog::formation::rivers_on(sphere, crust, climate, body);
    const hog::formation::Ice ice = hog::formation::ice_on(sphere, crust, climate, rivers, body);
    const hog::formation::Lakes lakes = hog::formation::lakes_on(sphere, rivers, climate, body);
    const Regimes regimes = hog::formation::regimes_on(sphere, climate, rivers, ice, lakes, body);
    REQUIRE(regimes.low.size() == sphere.size());
    REQUIRE(regimes.lake_rise.size() == lakes.lakes.size());
    std::size_t seasonal = 0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        CHECK(regimes.low[point] >= 0.0);
        CHECK(regimes.low[point] <= 1.0 + 1e-9);
        CHECK(regimes.high[point] >= 1.0 - 1e-9);
        CHECK(regimes.high[point] <= Regimes::months + 1e-9);
        CHECK(regimes.peak[point] >= 0.0);
        CHECK(regimes.peak[point] < 1.0);
        if (regimes.high[point] > 1.5) {
            ++seasonal;
        }
    }
    CHECK(seasonal > 0);
    for (const double rise : regimes.lake_rise) {
        CHECK(rise >= 0.0);
    }
}

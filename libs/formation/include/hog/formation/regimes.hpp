#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/ice.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sphere.hpp>

#include <array>
#include <vector>

namespace hog::formation {

// How rivers and lakes rise and fall through the year (Pardé's regimes): snow waits on the land
// for the thaw, ice melts through the summer, the warmer months give more of the rain back to the
// air, and lakes hold back what flows through them.
struct Regimes {
    static constexpr int months = 12;
    // Each point's river through the year, as shares of its flow the year round.
    std::vector<double> low;  // its lowest month's
    std::vector<double> high; // its highest month's
    std::vector<double> peak; // when it's highest, the share of the year from the March equinox
    // Each lake's level: metres its highest stands over its lowest, and when.
    std::vector<double> lake_rise;
    std::vector<double> lake_peak;
};

using Months = std::array<double, Regimes::months>;

// The share of the year from the March equinox a month's middle falls at.
[[nodiscard]] double month_middle(int month) noexcept;

// When a series through the year is highest, as a share of the year, between its months.
[[nodiscard]] double peak_of(const Months& series) noexcept;

// What a place gives its rivers each month as shares of what it gives in the year, which sum to
// 1. Its temperature swings `swing` either side of `mean`, warmest at `warmest`, a share of the
// year from the March equinox. `rain` metres a year are what the rivers get of it in all; the
// air takes the rest of what falls, as much as open water would lose (more in the warm months),
// so what falls is that much more, evenly the year round. Below freezing it lies as snow until
// the thaw melts it. Under ice, all of it comes off as the summer melts the ice.
[[nodiscard]] Months runoff_shares(double mean, double swing, double warmest, double rain,
                                   bool iced, const Body& body);

// A lake of `area` square metres, cubic metres a second flowing in each month and `loss` metres a
// second the air takes off it, which spills over a sill as wide as its outlet's river, or is
// closed: how far over its lowest its level stands each month, and what leaves it.
struct Pooled {
    Months level{};
    Months out{};
};
[[nodiscard]] Pooled pooled(const Months& in, const Months& loss, double area, bool open);

[[nodiscard]] Regimes regimes_on(const Sphere& sphere, const Climate& climate, const Rivers& rivers,
                                 const Ice& ice, const Lakes& lakes, const Body& body);

} // namespace hog::formation

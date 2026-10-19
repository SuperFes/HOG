#pragma once

#include <hog/core/math.hpp>

#include <cstdint>

namespace hog::flora {

// The year at a place as plants keep it, by the day's length.
struct Daylight {
    float hours = 12.0F;   // of a 24-hour day the sun is up
    float longest = 12.0F; // hours on the year's longest day there
    bool lengthening = false;
};

// How a broadleaf follows the year. Its leaves turn as the days shorten past `turn` hours, so
// further from the equator they turn sooner and in the tropics never, and are all down `fall`
// hours after. They come out again once the lengthening days are `flush` of the way from the
// shortest to the longest. Evergreens leave `turn` at 0 and keep theirs.
struct Leafing {
    float turn = 0.0F;   // hours of daylight
    float fall = 1.0F;   // hours
    float flush = 0.8F;  // 0 the shortest day, 1 the longest
    float spread = 0.0F; // hours either way one tree's dates fall from its kind's
    // The colours its leaves turn through, first and last, in linear light.
    Vec3 first{0.0F};
    Vec3 last{0.0F};
};

// A crown's leaves at a moment of the year.
struct Leaves {
    float turned = 0.0F; // 0 green, 1 as deep into autumn's colours as they go
    float held = 1.0F;   // the share still on the tree
};

// How early a tree keeps its kind's dates, -1 late to 1 early, from its dice and age: the same
// every year, older trees turning sooner.
[[nodiscard]] float own_season(std::uint32_t hash, float age) noexcept;

// The leaves of a tree `own` early, on a day of `daylight`.
[[nodiscard]] Leaves leaves_at(const Leafing& leafing, float own,
                               const Daylight& daylight) noexcept;

} // namespace hog::flora

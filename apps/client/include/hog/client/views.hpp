#pragma once

#include <hog/client/options.hpp>
#include <hog/client/scene.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/calendar.hpp>

#include <span>
#include <string_view>

namespace hog::client {

// A view of the planet held fixed, for pictures to compare from one change to the next.
struct FixedView {
    std::string_view name;
    Place place;
    double heading = 0.0;     // degrees east of north
    double pitch = 0.0;       // degrees above the horizon
    double time_of_day = 0.5; // local, from 0 at midnight to 1
};

// Round the island nearest the start, at times from morning to night.
[[nodiscard]] std::span<const FixedView> fixed_views() noexcept;

// The view as a shot, on a date fixed so the season doesn't change.
[[nodiscard]] Shot shot_of(const FixedView& view, const planet::World& world,
                           const sim::Calendar& calendar);

} // namespace hog::client

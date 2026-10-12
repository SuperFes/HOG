#pragma once

#include <hog/core/math.hpp>
#include <hog/probe/messages.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace hog::viewer {

// The point of the planet under a map point.
[[nodiscard]] probe::Inspect inspect_at(DVec2 point) noexcept;
// Where an inspected point lies on the map.
[[nodiscard]] DVec2 inspected_point(const probe::Inspect& inspect) noexcept;

struct Reading {
    std::string name;
    std::string value;

    friend bool operator==(const Reading&, const Reading&) = default;
};

// Layers sharing the part of their names before the first slash, such as "weather".
struct ReadingGroup {
    std::string name;
    std::vector<Reading> readings;

    friend bool operator==(const ReadingGroup&, const ReadingGroup&) = default;
};

// A value with its unit, such as "12.5 °C", or a biome by name.
[[nodiscard]] std::string reading_text(float value, std::string_view unit);

// The inspected cell's values by group, in the order the server offers its layers; empty when
// they don't match its layers.
[[nodiscard]] std::vector<ReadingGroup> readings(const probe::Welcome& welcome,
                                                 const probe::Inspection& inspection);

// The ground under the point, from the surface down.
[[nodiscard]] std::vector<Reading> column_readings(const probe::Column& column);

} // namespace hog::viewer

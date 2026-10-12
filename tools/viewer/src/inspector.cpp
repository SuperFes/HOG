#include <hog/planet/climate.hpp>
#include <hog/planet/geology.hpp>
#include <hog/viewer/inspector.hpp>

#include <cmath>
#include <format>
#include <utility>

namespace hog::viewer {

namespace {

std::string metres(float value) {
    return std::format("{:.4g} m", value);
}

template <typename Enum>
std::string named(std::uint8_t value, int count) {
    if (std::cmp_greater_equal(value, count)) {
        return std::format("unknown {}", value);
    }
    return std::string{planet::to_string(static_cast<Enum>(value))};
}

std::string rock_name(std::uint8_t rock) {
    return named<planet::Rock>(rock, planet::rock_count);
}

} // namespace

probe::Inspect inspect_at(DVec2 point) noexcept {
    return probe::Inspect{.latitude = (0.5 - point.y) * 180.0,
                          .longitude = (point.x - 0.5) * 360.0};
}

DVec2 inspected_point(const probe::Inspect& inspect) noexcept {
    return DVec2{(inspect.longitude / 360.0) + 0.5, 0.5 - (inspect.latitude / 180.0)};
}

std::string reading_text(float value, std::string_view unit) {
    if (unit == "biome" && value >= 0.0F && value == std::floor(value)) {
        return named<planet::Biome>(static_cast<std::uint8_t>(value), planet::biome_count);
    }
    // Four significant figures, but never as an exponent once past the thousands.
    std::string text =
        std::abs(value) >= 1'000.0F ? std::format("{:.0f}", value) : std::format("{:.4g}", value);
    if (!unit.empty()) {
        text += ' ';
        text += unit;
    }
    return text;
}

std::vector<ReadingGroup> readings(const probe::Welcome& welcome,
                                   const probe::Inspection& inspection) {
    std::vector<ReadingGroup> groups;
    if (inspection.values.size() != welcome.layers.size()) {
        return groups;
    }
    for (std::size_t index = 0; index < welcome.layers.size(); ++index) {
        const probe::LayerInfo& layer = welcome.layers[index];
        const std::size_t slash = layer.name.find('/');
        const std::string group = slash == std::string::npos ? "" : layer.name.substr(0, slash);
        const std::string field =
            slash == std::string::npos ? layer.name : layer.name.substr(slash + 1);
        if (groups.empty() || groups.back().name != group) {
            groups.push_back(ReadingGroup{.name = group, .readings = {}});
        }
        groups.back().readings.push_back(
            Reading{.name = field, .value = reading_text(inspection.values[index], layer.unit)});
    }
    return groups;
}

std::vector<Reading> column_readings(const probe::Column& column) {
    std::vector<Reading> lines{
        {.name = "elevation", .value = metres(column.elevation)},
        {.name = "biome", .value = named<planet::Biome>(column.biome, planet::biome_count)},
        {.name = "crust", .value = column.continental ? "continental" : "oceanic"},
    };
    float depth = 0.0F;
    const auto layer = [&](std::string_view name, std::uint8_t rock, float thickness) {
        if (thickness > 0.0F) {
            lines.push_back({.name = std::string{name},
                             .value = std::format("{}, {} to {}", rock_name(rock), metres(depth),
                                                  metres(depth + thickness))});
        }
        depth += thickness;
    };
    layer("cover", column.cover, column.cover_thickness);
    layer("sediment", column.sediment, column.sediment_thickness);
    lines.push_back(
        {.name = "bedrock",
         .value = std::format("{}, from {}", rock_name(column.bedrock), metres(depth))});
    for (const probe::Seam& seam : column.seams) {
        lines.push_back({.name = named<planet::Resource>(seam.resource, planet::resource_count),
                         .value = std::format("{} to {}", metres(seam.top),
                                              metres(seam.top + seam.thickness))});
    }
    return lines;
}

} // namespace hog::viewer

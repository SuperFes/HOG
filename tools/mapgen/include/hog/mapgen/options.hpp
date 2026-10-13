#pragma once

#include <hog/core/result.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace hog::mapgen {

enum class Layer : std::uint8_t {
    Height,
    Biome,
    Temperature,
    Moisture,
    Deposits,
    Water,
};

inline constexpr int layer_count = static_cast<int>(Layer::Water) + 1;

[[nodiscard]] std::string_view to_string(Layer layer) noexcept;

// The whole planet, longitude across and latitude down.
struct PlanetView {
    friend bool operator==(const PlanetView&, const PlanetView&) = default;
};

// One cube face, s across and t up.
struct FaceView {
    int face = 0;

    friend bool operator==(const FaceView&, const FaceView&) = default;
};

// A square region seen from straight above its centre, north up.
struct RegionView {
    double latitude = 0.0;  // degrees
    double longitude = 0.0; // degrees
    double width = 0.0;     // metres across

    friend bool operator==(const RegionView&, const RegionView&) = default;
};

// A square region centred on the starting islands, wherever the seed put them.
struct IslandsView {
    double width = 0.0; // metres across

    friend bool operator==(const IslandsView&, const IslandsView&) = default;
};

using View = std::variant<PlanetView, FaceView, RegionView, IslandsView>;

// Weather maps: frames from a start date, after the weather has had time to settle.
struct WeatherFrames {
    int days = 0;  // how long to follow it; none when 0
    int every = 6; // hours between frames
    std::chrono::sys_days start{std::chrono::year{2026} / 1 / 1};
    double cell_km = 6.0;

    friend bool operator==(const WeatherFrames&, const WeatherFrames&) = default;
};

// Wildlife maps: one per species every so often, with totals in a CSV. The weather frames' start
// and grid apply to these too.
struct WildlifeFrames {
    int years = 0;  // how long to follow it; none when 0
    int every = 30; // days between maps
    std::filesystem::path ecology = "config/ecology.toml";

    friend bool operator==(const WildlifeFrames&, const WildlifeFrames&) = default;
};

struct Options {
    std::filesystem::path config = "config/planet.toml";
    std::optional<std::uint64_t> seed;
    View view = PlanetView{};
    std::vector<Layer> layers; // every layer when empty
    int size = 1'024;          // image width in pixels
    std::filesystem::path output = "maps";
    WeatherFrames weather;
    WildlifeFrames wildlife;
    bool help = false;
};

[[nodiscard]] std::string_view usage() noexcept;

// Arguments without the program name.
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view> arguments);

} // namespace hog::mapgen

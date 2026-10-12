#include <hog/mapgen/options.hpp>
#include <hog/planet/cell_id.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <system_error>

namespace hog::mapgen {

namespace {

using planet::face_count;

constexpr int max_size = 16'384;

[[nodiscard]] std::unexpected<Error> invalid(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

template <typename T>
[[nodiscard]] std::optional<T> parse_integer(std::string_view text) {
    T value{};
    const char* last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

// strtod rather than from_chars, which some standard libraries lack for floating point.
[[nodiscard]] std::optional<double> parse_double(std::string_view text) {
    const std::string copy{text};
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != std::to_address(copy.end()) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<Layer> parse_layer(std::string_view name) {
    for (int layer = 0; layer < layer_count; ++layer) {
        if (to_string(static_cast<Layer>(layer)) == name) {
            return static_cast<Layer>(layer);
        }
    }
    return std::nullopt;
}

// LAT,LON,KM
[[nodiscard]] Result<RegionView> parse_region(std::string_view text) {
    std::vector<double> numbers;
    while (true) {
        const auto comma = text.find(',');
        const auto number = parse_double(text.substr(0, comma));
        if (!number) {
            break;
        }
        numbers.push_back(*number);
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    if (numbers.size() != 3) {
        return invalid("--region takes LAT,LON,KM");
    }
    const RegionView region{
        .latitude = numbers[0], .longitude = numbers[1], .width = numbers[2] * 1'000.0};
    if (std::abs(region.latitude) > 90.0 || std::abs(region.longitude) > 180.0) {
        return invalid("--region latitude must be within ±90 and longitude within ±180");
    }
    if (region.width <= 0.0) {
        return invalid("--region width must be positive");
    }
    return region;
}

// YYYY-MM-DD
[[nodiscard]] std::optional<std::chrono::sys_days> parse_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    const auto year = parse_integer<int>(text.substr(0, 4));
    const auto month = parse_integer<unsigned>(text.substr(5, 2));
    const auto day = parse_integer<unsigned>(text.substr(8, 2));
    if (!year || !month || !day) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year}, std::chrono::month{*month},
                                           std::chrono::day{*day}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date};
}

constexpr int max_weather_days = 366;
constexpr int max_frame_hours = 24 * 30;

// Options that take a value.
using Handler = Result<void> (*)(Options&, std::string_view);

struct Flag {
    std::string_view name;
    Handler handler;
};
constexpr std::array flags{
    Flag{.name = "--config",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             options.config = std::filesystem::path{value};
             return {};
         }},
    Flag{.name = "--out",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             options.output = std::filesystem::path{value};
             return {};
         }},
    Flag{.name = "--seed",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             options.seed = parse_integer<std::uint64_t>(value);
             if (!options.seed) {
                 return invalid(std::format("--seed {} is not a whole number", value));
             }
             return {};
         }},
    Flag{.name = "--face",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto face = parse_integer<int>(value);
             if (!face || *face < 0 || *face >= face_count) {
                 return invalid(std::format("--face {} is not 0 to 5", value));
             }
             options.view = FaceView{.face = *face};
             return {};
         }},
    Flag{.name = "--region",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto region = parse_region(value);
             if (!region) {
                 return std::unexpected(region.error());
             }
             options.view = *region;
             return {};
         }},
    Flag{.name = "--islands",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto width = parse_double(value);
             if (!width || *width <= 0.0) {
                 return invalid(std::format("--islands {} is not a positive width", value));
             }
             options.view = IslandsView{.width = *width * 1'000.0};
             return {};
         }},
    Flag{.name = "--layer",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto layer = parse_layer(value);
             if (!layer) {
                 return invalid(std::format("unknown layer {}", value));
             }
             options.layers.push_back(*layer);
             return {};
         }},
    Flag{.name = "--size",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto size = parse_integer<int>(value);
             if (!size || *size < 2 || *size > max_size) {
                 return invalid(std::format("--size {} is not 2 to {}", value, max_size));
             }
             options.size = *size;
             return {};
         }},
    Flag{.name = "--weather",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto days = parse_integer<int>(value);
             if (!days || *days < 1 || *days > max_weather_days) {
                 return invalid(
                     std::format("--weather {} is not 1 to {} days", value, max_weather_days));
             }
             options.weather.days = *days;
             return {};
         }},
    Flag{.name = "--every",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto hours = parse_integer<int>(value);
             if (!hours || *hours < 1 || *hours > max_frame_hours) {
                 return invalid(
                     std::format("--every {} is not 1 to {} hours", value, max_frame_hours));
             }
             options.weather.every = *hours;
             return {};
         }},
    Flag{.name = "--date",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto date = parse_date(value);
             if (!date) {
                 return invalid(std::format("--date {} is not a date like 2026-01-31", value));
             }
             options.weather.start = *date;
             return {};
         }},
    Flag{.name = "--cell-km",
         .handler = [](Options& options, std::string_view value) -> Result<void> {
             const auto km = parse_double(value);
             if (!km || *km <= 0.0) {
                 return invalid(std::format("--cell-km {} is not a positive size", value));
             }
             options.weather.cell_km = *km;
             return {};
         }},
};

} // namespace

std::string_view to_string(Layer layer) noexcept {
    switch (layer) {
    case Layer::Height:
        return "height";
    case Layer::Biome:
        return "biome";
    case Layer::Temperature:
        return "temperature";
    case Layer::Moisture:
        return "moisture";
    case Layer::Deposits:
        return "deposits";
    }
    return "unknown";
}

std::string_view usage() noexcept {
    return R"(usage: hog-mapgen [options]

Writes PNG maps of a generated planet, one per layer, named <view>-<layer>.png.

  --config PATH        planet config (default config/planet.toml)
  --seed N             overrides the config's seed
  --planet             the whole planet, longitude across and latitude down (default)
  --face F             one cube face, 0 to 5
  --region LAT,LON,KM  a square KM across, centred on LAT,LON in degrees
  --islands KM         a square KM across, centred on the starting islands
  --layer NAME         height, biome, temperature, moisture or deposits; repeat for
                       several (default: all)
  --size PIXELS        image width (default 1024); the planet view is half as tall
  --out DIR            output directory (default maps)
  --weather DAYS       instead of the layers, follows the weather for DAYS days and
                       writes sky and air maps, named <view>-weather-<time>-<map>.png
  --every HOURS        hours between weather maps (default 6)
  --date YYYY-MM-DD    when the weather maps start (default 2026-01-01), after ten days
                       for the weather to settle
  --cell-km KM         weather grid cells at most KM across (default 6)
  --help               shows this
)";
}

Result<Options> parse_options(std::span<const std::string_view> arguments) {
    Options options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view name = arguments[index];
        if (name == "--help" || name == "-h") {
            options.help = true;
            continue;
        }
        if (name == "--planet") {
            options.view = PlanetView{};
            continue;
        }
        const Flag* flag = nullptr;
        for (const Flag& candidate : flags) {
            if (candidate.name == name) {
                flag = &candidate;
            }
        }
        if (flag == nullptr) {
            return invalid(std::format("unknown option {}", name));
        }
        if (index + 1 >= arguments.size()) {
            return invalid(std::format("{} needs a value", name));
        }
        if (auto handled = flag->handler(options, arguments[++index]); !handled) {
            return std::unexpected(std::move(handled.error()));
        }
    }
    if (options.layers.empty()) {
        for (int layer = 0; layer < layer_count; ++layer) {
            options.layers.push_back(static_cast<Layer>(layer));
        }
    }
    return options;
}

} // namespace hog::mapgen

#include <hog/client/options.hpp>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace hog::client {

namespace {

constexpr int most_threads = 256;

std::optional<int> integer(std::string_view text) {
    int value = 0;
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

// strtod rather than from_chars, which some standard libraries lack for floating point.
std::optional<double> real(std::string_view text) {
    const std::string copy{text};
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != std::to_address(copy.end()) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<Place> place(std::string_view text) {
    const std::size_t first = text.find(',');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t second = text.find(',', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const auto latitude = real(text.substr(0, first));
    const auto longitude = real(text.substr(first + 1, second - first - 1));
    const auto altitude = real(text.substr(second + 1));
    if (!latitude || !longitude || !altitude || std::abs(*latitude) > 90.0) {
        return std::nullopt;
    }
    return Place{.latitude = *latitude, .longitude = *longitude, .altitude = *altitude};
}

// A fraction of the day from HH:MM.
std::optional<double> time_of_day(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<int> hours = integer(text.substr(0, colon));
    const std::optional<int> minutes = integer(text.substr(colon + 1));
    if (!hours || !minutes || *hours < 0 || *hours > 23 || *minutes < 0 || *minutes > 59) {
        return std::nullopt;
    }
    return ((*hours * 60.0) + *minutes) / (24.0 * 60.0);
}

// Reads an option that takes a value.
Result<void> read_option(Options& options, std::string_view option, std::string_view value) {
    if (option == "--planet") {
        options.planet = std::filesystem::path{value};
    } else if (option == "--at") {
        options.start = place(value);
        if (!options.start) {
            return make_error(
                ErrorCode::InvalidArgument,
                std::format("--at takes latitude,longitude,altitude in degrees and metres, not {}",
                            value));
        }
    } else if (option == "--time") {
        options.time_of_day = time_of_day(value);
        if (!options.time_of_day) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("--time takes a time of day as HH:MM, not {}", value));
        }
    } else if (option == "--connect") {
        options.connect = std::string{value};
    } else if (option == "--login") {
        options.login = std::string{value};
    } else if (option == "--name") {
        options.name = std::string{value};
    } else if (option == "--companion") {
        options.companion = std::filesystem::path{value};
    } else if (option == "--capture") {
        options.capture = std::filesystem::path{value};
    } else if (option == "--threads") {
        const std::optional<int> threads = integer(value);
        if (!threads || *threads < 0 || *threads > most_threads) {
            return make_error(
                ErrorCode::InvalidArgument,
                std::format("--threads must be from 0 to {}, not {}", most_threads, value));
        }
        options.threads = static_cast<std::uint32_t>(*threads);
    } else if (option == "--quit-after") {
        const std::optional<int> seconds = integer(value);
        if (!seconds || *seconds < 0) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("--quit-after takes whole seconds, not {}", value));
        }
        options.quit_after = std::chrono::seconds{*seconds};
    } else {
        return make_error(ErrorCode::InvalidArgument, std::format("unknown argument {}", option));
    }
    return {};
}

// Options that only make sense with others.
Result<void> check_together(const Options& options) {
    if (options.login && (!options.name || !options.connect)) {
        return make_error(ErrorCode::InvalidArgument,
                          "--login needs --name to log in as and --connect to go on to");
    }
    if (options.name && !options.login) {
        return make_error(ErrorCode::InvalidArgument, "--name is for --login");
    }
    return {};
}

} // namespace

std::string_view usage() noexcept {
    return R"(usage: hog-client [options]

Flies or walks over the planet.

options:
  --planet PATH          the planet's config (default config/planet.toml)
  --at LAT,LON,ALT       start at degrees of latitude and longitude, metres above
                         sea level (default over the starting islands)
  --time HH:MM           start at this local time of day (default the planet's own)
  --threads N            baking the ground (default one per hardware thread)
  --connect ADDRESS      connect to a server at an IP address and port, such as
                         127.0.0.1:7675
  --login ADDRESS        log in there first, such as 127.0.0.1:4647, with --name
  --name NAME            who to log in as
  --companion PATH       a drawing on white paper of someone 3 m tall to fly along
                         with (default res/Matt.png)
  --alone                fly without them
  --quit-after S         quit after this many seconds
  --capture PATH         save the last frame drawn as a PNG, on quitting
  --timings              show how long frames take from the start
  --software             draw with a software Vulkan device, such as lavapipe
  --validate             check Vulkan use with its validation layer, failing on errors;
                         on by default in debug builds
  --no-validate          don't
  --reload-shaders       compile shaders again and draw with them as they're edited;
                         on by default in debug builds
  --no-reload-shaders    don't
  -h, --help             show this help

flying:
  W A S D                forwards, left, back, right
  Space, C               up, down
  Shift                  faster
  right mouse button     hold to look around
  mouse wheel            speed up or slow down
  F                      walk

walking:
  W A S D                forwards, left, back, right
  Shift                  run
  right mouse button     hold to look around
  F                      fly

time:
  ] [                    run the planet's time faster, slower

  F3                     show or hide how long frames take
)";
}

Result<Options> parse_options(std::span<const std::string_view> arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "-h" || argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument == "--alone") {
            options.companion.reset();
            continue;
        }
        if (argument == "--timings") {
            options.timings = true;
            continue;
        }
        if (argument == "--software") {
            options.software = true;
            continue;
        }
        if (argument == "--validate") {
            options.validate = true;
            continue;
        }
        if (argument == "--no-validate") {
            options.validate = false;
            continue;
        }
        if (argument == "--reload-shaders") {
            options.reload_shaders = true;
            continue;
        }
        if (argument == "--no-reload-shaders") {
            options.reload_shaders = false;
            continue;
        }
        if (i + 1 == arguments.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("unknown argument {}, or it needs a value", argument));
        }
        if (auto read = read_option(options, argument, arguments[++i]); !read) {
            return std::unexpected(std::move(read.error()));
        }
    }
    if (auto together = check_together(options); !together) {
        return std::unexpected(std::move(together.error()));
    }
    return options;
}

std::filesystem::path locate(const std::filesystem::path& path, const std::filesystem::path& from) {
    std::error_code error;
    if (path.is_absolute() || std::filesystem::exists(path, error)) {
        return path;
    }
    for (std::filesystem::path at = from; !at.empty(); at = at.parent_path()) {
        if (std::filesystem::exists(at / path, error)) {
            return at / path;
        }
        if (at == at.parent_path()) {
            break;
        }
    }
    return path;
}

} // namespace hog::client

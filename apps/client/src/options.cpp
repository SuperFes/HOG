#include <hog/client/options.hpp>

#include <array>
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

Result<void> read_clouds(Options& options, std::string_view value) {
    const std::optional<render::CloudQuality> quality = render::cloud_quality_named(value);
    if (!quality) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("--clouds takes low, medium or high, not {}", value));
    }
    options.clouds = *quality;
    return {};
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
    } else if (option == "--simulate") {
        auto simulation = net::parse_simulation(value);
        if (!simulation) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("--simulate: {}", simulation.error().message));
        }
        options.simulate = *simulation;
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
    } else if (option == "--clouds") {
        return read_clouds(options, value);
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

// Options that stand alone, and what each sets.
struct Flag {
    std::string_view name;
    void (*set)(Options&);
};

constexpr std::array flags{
    Flag{.name = "-h", .set = [](Options& options) { options.help = true; }},
    Flag{.name = "--help", .set = [](Options& options) { options.help = true; }},
    Flag{.name = "--alone", .set = [](Options& options) { options.companion.reset(); }},
    Flag{.name = "--timings", .set = [](Options& options) { options.timings = true; }},
    Flag{.name = "--chat-filter", .set = [](Options& options) { options.chat_filter = true; }},
    Flag{.name = "--no-chat-filter", .set = [](Options& options) { options.chat_filter = false; }},
    Flag{.name = "--grass", .set = [](Options& options) { options.grass = true; }},
    Flag{.name = "--no-grass", .set = [](Options& options) { options.grass = false; }},
    Flag{.name = "--software", .set = [](Options& options) { options.software = true; }},
    Flag{.name = "--validate", .set = [](Options& options) { options.validate = true; }},
    Flag{.name = "--no-validate", .set = [](Options& options) { options.validate = false; }},
    Flag{.name = "--reload-shaders",
         .set = [](Options& options) { options.reload_shaders = true; }},
    Flag{.name = "--no-reload-shaders",
         .set = [](Options& options) { options.reload_shaders = false; }},
};

// The flag named `argument`, or none.
const Flag* flag_named(std::string_view argument) noexcept {
    for (const Flag& flag : flags) {
        if (flag.name == argument) {
            return &flag;
        }
    }
    return nullptr;
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
    if (!options.simulate.off() && !options.connect) {
        return make_error(ErrorCode::InvalidArgument, "--simulate is for --connect");
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
  --simulate SETTINGS    a bad network to connect over, such as
                         lag=75,jitter=10,loss=2,reorder=1 in ms and percent, each
                         added both ways
  --companion PATH       a drawing on white paper of someone 3 m tall to fly along
                         with (default res/Matt.png)
  --alone                fly without them
  --quit-after S         quit after this many seconds
  --capture PATH         save the last frame drawn as a PNG, on quitting
  --timings              show how long frames take from the start
  --chat-filter          cussing in chat as comic-book grawlix; on by default
  --no-chat-filter       cussing as it was said; hate speech reads as small
                         talk about the weather either way
  --clouds QUALITY       draw the clouds at low, medium or high quality, for what
                         the GPU can afford (default high)
  --no-grass             don't grow grass near the eye, for what the GPU can afford
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

chat, when connected:
  Enter                  say something: Enter again sends it, Esc puts it away

time:
  ] [                    run this client's daylight faster, slower
  } {                    run the server's whole world faster, slower, where its
                         config allows time control

  F3                     show or hide how long frames take
  F4                     draw the clouds at the next quality
)";
}

Result<Options> parse_options(std::span<const std::string_view> arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (const Flag* const flag = flag_named(argument); flag != nullptr) {
            flag->set(options);
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

#include <hog/viewer/options.hpp>

#include <charconv>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace hog::viewer {

namespace {

constexpr int narrowest = 64;
constexpr int widest = 8'192;

std::optional<int> integer(std::string_view text) {
    int value = 0;
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

// Reads an option that takes a value.
Result<void> read_option(Options& options, std::string_view option, std::string_view value) {
    if (option == "--host") {
        options.host = value;
    } else if (option == "--overlay") {
        const std::optional<Overlay> overlay = overlay_named(value);
        if (!overlay) {
            return make_error(ErrorCode::InvalidArgument, std::format("no overlay {}", value));
        }
        options.overlay = *overlay;
    } else if (option == "--species") {
        options.species = value;
    } else if (option == "--capture") {
        options.capture = std::filesystem::path{value};
    } else if (option == "--quit-after") {
        const std::optional<int> seconds = integer(value);
        if (!seconds || *seconds < 0) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("--quit-after takes whole seconds, not {}", value));
        }
        options.quit_after = std::chrono::seconds{*seconds};
    } else if (option == "--port") {
        const std::optional<int> port = integer(value);
        if (!port || *port < 1 ||
            std::cmp_greater(*port, std::numeric_limits<std::uint16_t>::max())) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("--port must be from 1 to 65535, not {}", value));
        }
        options.port = static_cast<std::uint16_t>(*port);
    } else if (option == "--width") {
        const std::optional<int> width = integer(value);
        if (!width || *width < narrowest || *width > widest) {
            return make_error(
                ErrorCode::InvalidArgument,
                std::format("--width must be from {} to {}, not {}", narrowest, widest, value));
        }
        options.width = *width;
    } else {
        return make_error(ErrorCode::InvalidArgument, std::format("unknown argument {}", option));
    }
    return {};
}

} // namespace

std::string_view usage() noexcept {
    return R"(usage: hog-viewer [options]

Shows a running server's world: weather, plants and wildlife on a map of the planet.

options:
  --host HOST     where the server runs (default 127.0.0.1)
  --port PORT     its [debug] port (default 7777)
  --width PIXELS  of the rendered map (default 1024)
  --overlay NAME  shown first: terrain, biome, minerals, temperature, air, sky,
                  pressure, dryness, plants, fire or wildlife (default terrain)
  --species NAME  shown first on the wildlife overlay
  --quit-after S  quit after this many seconds
  --capture PATH  save the last frame drawn as a PNG, on quitting
  -h, --help      show this help
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
        if (i + 1 == arguments.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("unknown argument {}, or it needs a value", argument));
        }
        if (auto read = read_option(options, argument, arguments[++i]); !read) {
            return std::unexpected(std::move(read.error()));
        }
    }
    return options;
}

} // namespace hog::viewer

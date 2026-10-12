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

} // namespace

std::string_view usage() noexcept {
    return R"(usage: hog-viewer [options]

Shows a running server's world: weather, plants and wildlife on a map of the planet.

options:
  --host HOST     where the server runs (default 127.0.0.1)
  --port PORT     its [debug] port (default 7777)
  --width PIXELS  of the rendered map (default 1024)
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
        if (argument != "--host" && argument != "--port" && argument != "--width") {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("unknown argument {}", argument));
        }
        if (i + 1 == arguments.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("{} needs a value", argument));
        }
        const std::string_view value = arguments[++i];
        if (argument == "--host") {
            options.host = value;
        } else if (argument == "--port") {
            const std::optional<int> port = integer(value);
            if (!port || *port < 1 ||
                std::cmp_greater(*port, std::numeric_limits<std::uint16_t>::max())) {
                return make_error(ErrorCode::InvalidArgument,
                                  std::format("--port must be from 1 to 65535, not {}", value));
            }
            options.port = static_cast<std::uint16_t>(*port);
        } else {
            const std::optional<int> width = integer(value);
            if (!width || *width < narrowest || *width > widest) {
                return make_error(
                    ErrorCode::InvalidArgument,
                    std::format("--width must be from {} to {}, not {}", narrowest, widest, value));
            }
            options.width = *width;
        }
    }
    return options;
}

} // namespace hog::viewer

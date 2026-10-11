#include <hog/app/options.hpp>

#include <format>

namespace hog::app {

std::string_view usage() noexcept {
    return R"(usage: hog-server [options]

Runs a world: loads its newest snapshot, catches up to now, and ticks until interrupted.

options:
  --config PATH   server config (default config/server.toml)
  -h, --help      show this help
)";
}

Result<Options> parse_options(std::span<const std::string_view> arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "-h" || argument == "--help") {
            options.help = true;
        } else if (argument == "--config") {
            if (i + 1 == arguments.size()) {
                return make_error(ErrorCode::InvalidArgument, "--config needs a path");
            }
            options.config = arguments[++i];
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("unknown argument {}", argument));
        }
    }
    return options;
}

} // namespace hog::app

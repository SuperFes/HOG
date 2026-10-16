#include <hog/login_app/options.hpp>

#include <charconv>
#include <format>
#include <memory>

namespace hog::login_app {

std::string_view usage() noexcept {
    return R"(usage: hog-login [options]

Hands out connect tokens to anyone asking with a fit name: a stand-in for the login service. Its
signing key is made on first run; game servers check tokens with the public key it writes.

options:
  --port N        where clients ask (default 4647)
  --keys DIR      where login.key and login.pub are kept (default var)
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
        if (argument != "--port" && argument != "--keys") {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("unknown argument {}", argument));
        }
        if (i + 1 == arguments.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("{} needs a value", argument));
        }
        const std::string_view value = arguments[++i];
        if (argument == "--keys") {
            options.keys = value;
            continue;
        }
        std::uint16_t port = 0;
        const char* last = std::to_address(value.end());
        const auto [end, parsed] = std::from_chars(std::to_address(value.begin()), last, port);
        if (parsed != std::errc{} || end != last || port == 0) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("--port takes 1 to 65535, not {}", value));
        }
        options.port = port;
    }
    return options;
}

} // namespace hog::login_app

#include <hog/bot/options.hpp>

#include <charconv>
#include <format>
#include <memory>

namespace hog::bot {

namespace {

constexpr std::uint32_t most_bots = 100;

template <typename T>
std::optional<T> number(std::string_view text) {
    T value = 0;
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

Result<void> read_option(Options& options, std::string_view option, std::string_view value) {
    if (option == "--planet") {
        options.planet = std::filesystem::path{value};
    } else if (option == "--connect") {
        options.connect = std::string{value};
    } else if (option == "--login") {
        options.login = std::string{value};
    } else if (option == "--name") {
        options.name = std::string{value};
    } else if (option == "--count") {
        const auto count = number<std::uint32_t>(value);
        if (!count || *count == 0 || *count > most_bots) {
            return make_error(
                ErrorCode::InvalidArgument,
                std::format("--count must be from 1 to {}, not {}", most_bots, value));
        }
        options.count = *count;
    } else if (option == "--seed") {
        const auto seed = number<std::uint64_t>(value);
        if (!seed) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("--seed takes a whole number, not {}", value));
        }
        options.seed = *seed;
    } else if (option == "--run-for") {
        const auto seconds = number<std::uint32_t>(value);
        if (!seconds) {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("--run-for takes whole seconds, not {}", value));
        }
        options.run_for = std::chrono::seconds{*seconds};
    } else {
        return make_error(ErrorCode::InvalidArgument, std::format("unknown argument {}", option));
    }
    return {};
}

} // namespace

std::string_view usage() noexcept {
    return R"(usage: hog-bot [options]

Walks headless players over the planet for testing: each connects like a
client and wanders the ground near where it started.

options:
  --connect ADDRESS      the server, an IP address and port such as 127.0.0.1:7675
  --login ADDRESS        log each bot in there first, as NAME-1, NAME-2 and so on
  --name NAME            what the bots are called (default bot)
  --count N              how many bots, 1 to 100 (default 1)
  --planet PATH          the planet's config, the same the server runs
                         (default config/planet.toml)
  --seed N               where they start and wander (default 1)
  --run-for S            quit after this many seconds (default: run until stopped)
  -h, --help             show this help
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
    if (options.connect.empty() && !options.help) {
        return make_error(ErrorCode::InvalidArgument, "--connect is needed: whose server to walk");
    }
    return options;
}

} // namespace hog::bot

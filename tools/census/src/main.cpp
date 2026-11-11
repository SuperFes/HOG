#include <hog/census/census.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/jobs/job_system.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view usage =
    "usage: hog-census [--planet FILE] [--seeds N,N,...] [--level N] [--lakes N]\n"
    "Counts what the generator makes of the world over every cell of a level's grid (9 by\n"
    "default): land and sea, inland water still and running, and what tops the land; for each\n"
    "seed given, the planet's own by default.\n";

struct Options {
    hog::globe::Planet planet;
    std::vector<std::uint64_t> seeds;
    int level = 9;
    std::size_t lakes = 10;
};

std::optional<double> number(std::string_view text) {
    const std::string part{text};
    char* end = nullptr;
    const double value = std::strtod(part.c_str(), &end);
    if (part.empty() || end != std::to_address(part.end()) || !std::isfinite(value) ||
        value < 0.0) {
        return std::nullopt;
    }
    return value;
}

bool apply(Options& options, std::string_view name, std::string_view value) {
    if (name == "--planet") {
        const auto loaded = hog::globe::load_planet(value);
        if (!loaded) {
            std::cerr << std::format("hog-census: {}\n", loaded.error());
        }
        options.planet = loaded.value_or(options.planet);
        return loaded.has_value();
    }
    if (name == "--seeds") {
        options.seeds.clear();
        for (std::string_view rest = value; !rest.empty();) {
            const std::size_t comma = rest.find(',');
            const auto seed = number(rest.substr(0, comma));
            if (!seed) {
                return false;
            }
            options.seeds.push_back(static_cast<std::uint64_t>(*seed));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
        return !options.seeds.empty();
    }
    const auto given = number(value);
    if (!given) {
        return false;
    }
    if (name == "--level" && *given <= 12.0) {
        options.level = static_cast<int>(*given);
    } else if (name == "--lakes") {
        options.lakes = static_cast<std::size_t>(*given);
    } else {
        return false;
    }
    return true;
}

int run(std::span<const std::string_view> arguments) {
    Options options;
    if (const auto loaded = hog::globe::load_planet("config/planet.toml")) {
        options.planet = *loaded;
    }
    for (std::size_t k = 0; k < arguments.size(); k += 2) {
        if (k + 1 >= arguments.size() || !apply(options, arguments[k], arguments[k + 1])) {
            std::cerr << usage;
            return EXIT_FAILURE;
        }
    }
    if (options.seeds.empty()) {
        options.seeds.push_back(options.planet.body.seed);
    }
    hog::jobs::JobSystem jobs;
    const hog::globe::EachOf each = [&](std::size_t count,
                                        const std::function<void(std::size_t)>& body) {
        jobs.parallel_for(static_cast<std::uint32_t>(count), 1,
                          [&](hog::jobs::Range range, std::uint32_t) {
                              for (std::uint32_t n = range.begin; n < range.end; ++n) {
                                  body(n);
                              }
                          });
    };
    for (const std::uint64_t seed : options.seeds) {
        hog::globe::Planet planet = options.planet;
        planet.body.seed = seed;
        const auto started = std::chrono::steady_clock::now();
        const hog::globe::Generator generator{planet};
        const auto formed = std::chrono::steady_clock::now();
        const std::vector<hog::census::Spot> spots =
            hog::census::survey(generator, options.level, each);
        const auto surveyed = std::chrono::steady_clock::now();
        const std::optional<double> sea = generator.sea_level();
        const hog::census::Census census =
            hog::census::count(options.level, spots, planet.body.radius,
                               sea ? std::optional{*sea - planet.body.radius} : std::nullopt);
        const auto seconds = [](auto from, auto to) {
            return std::chrono::duration<double>(to - from).count();
        };
        std::cout << std::format("seed {} (formed {:.1f} s, surveyed {:.1f} s)\n", seed,
                                 seconds(started, formed), seconds(formed, surveyed))
                  << hog::census::report(census, options.lakes) << '\n';
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span raw{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(raw.begin() + 1, raw.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-census: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

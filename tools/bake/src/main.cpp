#include <hog/bake/bake.hpp>
#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/globe/planet.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view usage =
    "usage: hog-bake --store DIR (--coarse METRES | --at LAT,LON [--across METRES] | both)\n"
    "                [--planet FILE] [--threads N] [--map-gb N]\n"
    "Bakes the world's grid into the store at DIR ahead of play: over the whole planet down\n"
    "to the first level whose nodes are --coarse metres across or less, and within --across\n"
    "metres (1000 by default) about --at down to the finest. Pages already held as deep as\n"
    "that as this generator makes them are kept, so a stopped bake carries on. The planet is\n"
    "config/planet.toml where that's found, else the default.\n";

struct Options {
    hog::globe::Planet planet;
    std::optional<std::filesystem::path> store;
    std::uint32_t threads = 0;
    std::uint64_t map_gb = 256;
    std::optional<double> coarse;
    std::optional<std::pair<double, double>> at; // degrees
    double across = 1000.0;
};

std::optional<double> number(std::string_view text) {
    const std::string part{text};
    char* end = nullptr;
    const double value = std::strtod(part.c_str(), &end);
    if (part.empty() || end != std::to_address(part.end()) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// Each option and its value; false on anything it doesn't know.
bool apply(Options& options, std::string_view name, std::string_view value) {
    if (name == "--planet") {
        const auto loaded = hog::globe::load_planet(value);
        if (!loaded) {
            std::cerr << std::format("hog-bake: {}\n", loaded.error());
        }
        options.planet = loaded.value_or(options.planet);
        return loaded.has_value();
    }
    if (name == "--store") {
        options.store = value;
        return true;
    }
    if (name == "--at") {
        const std::size_t comma = value.find(',');
        const auto latitude = number(value.substr(0, comma));
        const auto longitude =
            comma == std::string_view::npos ? std::nullopt : number(value.substr(comma + 1));
        if (!latitude || !longitude) {
            return false;
        }
        options.at = {*latitude, *longitude};
        return true;
    }
    const auto given = number(value);
    if (!given || *given < 0.0) {
        return false;
    }
    if (name == "--threads") {
        options.threads = static_cast<std::uint32_t>(*given);
    } else if (name == "--map-gb" && *given >= 1.0) {
        options.map_gb = static_cast<std::uint64_t>(*given);
    } else if (name == "--coarse" && *given > 0.0) {
        options.coarse = given;
    } else if (name == "--across" && *given > 0.0) {
        options.across = *given;
    } else {
        return false;
    }
    return true;
}

hog::DVec3 direction_of(double latitude, double longitude) {
    constexpr double degrees = std::numbers::pi / 180.0;
    const double lat = latitude * degrees;
    const double lon = longitude * degrees;
    return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

int run(std::span<const std::string_view> arguments) {
    Options options;
    if (std::filesystem::exists("config/planet.toml")) {
        if (const auto loaded = hog::globe::load_planet("config/planet.toml")) {
            options.planet = *loaded;
        }
    }
    const auto refuse = [](std::string_view why) {
        std::cerr << std::format("hog-bake: {}\n\n{}", why, usage);
        return EXIT_FAILURE;
    };
    for (std::size_t k = 0; k < arguments.size(); k += 2) {
        if (k + 1 >= arguments.size()) {
            return refuse(std::format("{} needs a value", arguments[k]));
        }
        if (!apply(options, arguments[k], arguments[k + 1])) {
            return refuse(std::format("can't take {} {}", arguments[k], arguments[k + 1]));
        }
    }
    if (!options.store) {
        return refuse("--store is required");
    }
    if (!options.coarse && !options.at) {
        return refuse("give --coarse, --at, or both, to say what to bake");
    }
    const auto formed = std::chrono::steady_clock::now();
    const hog::globe::Generator generator{options.planet, [](std::string_view stage) {
                                              std::cerr << std::format("forming: {}\n", stage);
                                          }};
    std::cerr << std::format(
        "formed in {:.1f} s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - formed).count());
    auto store = hog::baked::PageStore::open(
        *options.store, hog::baked::baking_of(generator),
        {.map_bytes = options.map_gb << 30U, .readers = 1024, .read_only = false});
    if (!store) {
        std::cerr << std::format("hog-bake: {}\n", store.error());
        return EXIT_FAILURE;
    }
    hog::bake::Reach reach;
    if (options.coarse) {
        reach.coarse = *options.coarse;
    }
    if (options.at) {
        const hog::DVec3 direction = direction_of(options.at->first, options.at->second);
        reach.around =
            direction * generator.column(direction, hog::globe::voxel_lod(generator)).ground();
        reach.across = options.across;
    }
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    if (options.coarse) {
        const std::uint8_t depth = hog::bake::depth_of(grid, *options.coarse);
        std::cerr << std::format("everywhere down to level {} of {}, nodes {:.3g} m across\n",
                                 depth, grid.depth(), grid.size(depth));
    }
    hog::jobs::JobSystem jobs{options.threads};
    const auto started = std::chrono::steady_clock::now();
    const auto baked = hog::bake::bake(
        generator, *store, jobs, reach, std::size_t{4} * jobs.thread_count(),
        [&](std::size_t done, std::size_t waiting) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cerr << std::format("{} pages done, {:.1f} a second, {} found waiting, {:.0f} s\n",
                                     done, static_cast<double>(done) / std::max(seconds, 1e-3),
                                     waiting, seconds);
        });
    if (!baked) {
        std::cerr << std::format("hog-bake: {}\n", baked.error());
        return EXIT_FAILURE;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto megabytes = [](std::size_t bytes) { return static_cast<double>(bytes) / 1e6; };
    std::cout << std::format(
        "{} pages filled in {:.1f} s, {} already held; trees: shape {:.1f} MB, material {:.1f} "
        "MB, water {:.1f} MB, woods {:.1f} MB\n",
        baked->filled, seconds, baked->kept, megabytes(baked->shape_bytes),
        megabytes(baked->material_bytes), megabytes(baked->water_bytes),
        megabytes(baked->woods_bytes));
    if (const auto stats = store->stats()) {
        std::cout << std::format("store: {} trees, {:.1f} MB\n", stats->trees,
                                 megabytes(stats->bytes));
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
        std::cerr << "hog-bake: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

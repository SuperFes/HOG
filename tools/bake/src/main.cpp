#include <hog/bake/bake.hpp>
#include <hog/baked/store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/jobs/job_system.hpp>

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
    "usage: hog-bake --store DIR [--planet FILE] [--threads N] [--level L] [--map-gb N]\n"
    "                (--face F | --at LAT,LON [--across METRES])\n"
    "Bakes the world's voxels into the store at DIR ahead of play, each voxel 2^L of the\n"
    "finest a side (0 by default): a whole face of the planet, or a square --across metres\n"
    "(1000 by default) about --at. Blocks already baked as finely are left as they are, so a\n"
    "stopped bake carries on. Bake coarse before fine: a coarser bake writes over finer.\n";

struct Options {
    hog::globe::Planet planet;
    std::optional<std::filesystem::path> store;
    std::uint32_t threads = 0;
    std::uint8_t level = 0;
    std::uint64_t map_gb = 256;
    std::optional<std::uint8_t> face;
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
    } else if (name == "--level" && *given < 16.0) {
        options.level = static_cast<std::uint8_t>(*given);
    } else if (name == "--map-gb" && *given >= 1.0) {
        options.map_gb = static_cast<std::uint64_t>(*given);
    } else if (name == "--face" && *given < 6.0) {
        options.face = static_cast<std::uint8_t>(*given);
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
    for (std::size_t k = 0; k < arguments.size(); k += 2) {
        if (k + 1 >= arguments.size() || !apply(options, arguments[k], arguments[k + 1])) {
            std::cerr << usage;
            return EXIT_FAILURE;
        }
    }
    if (!options.store || options.face.has_value() == options.at.has_value()) {
        std::cerr << usage;
        return EXIT_FAILURE;
    }
    auto store = hog::baked::Store::open(*options.store, {.map_bytes = options.map_gb << 30U});
    if (!store) {
        std::cerr << std::format("hog-bake: {}\n", store.error());
        return EXIT_FAILURE;
    }
    const auto formed = std::chrono::steady_clock::now();
    const hog::globe::Generator generator{options.planet, [](std::string_view stage) {
                                              std::cerr << std::format("forming: {}\n", stage);
                                          }};
    std::cerr << std::format(
        "formed in {:.1f} s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - formed).count());
    const std::vector<hog::globe::TileKey> blocks =
        options.face ? hog::bake::face_blocks(generator, *options.face, options.level)
                     : hog::bake::blocks_around(generator,
                                                direction_of(options.at->first, options.at->second),
                                                options.across, options.level);
    hog::jobs::JobSystem jobs{options.threads};
    const auto started = std::chrono::steady_clock::now();
    const auto baked = hog::bake::bake(
        generator, *store, jobs, blocks, options.level, std::size_t{2} * jobs.thread_count(),
        [&](std::size_t done, std::size_t total) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cerr << std::format(
                "{} of {} blocks, {:.0f} s, {:.0f} s left\n", done, total, seconds,
                seconds * static_cast<double>(total - done) / static_cast<double>(done));
        });
    if (!baked) {
        std::cerr << std::format("hog-bake: {}\n", baked.error());
        return EXIT_FAILURE;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto stats = store->stats();
    std::cout << std::format("{} blocks baked in {:.1f} s, {:.1f} MB of pages; {} already baked\n",
                             baked->made, seconds, static_cast<double>(baked->bytes) / 1e6,
                             baked->skipped);
    if (stats) {
        std::cout << std::format("store: {} records, {:.1f} MB\n", stats->records,
                                 static_cast<double>(stats->bytes) / 1e6);
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

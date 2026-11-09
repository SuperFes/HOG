#include <hog/build_world/build_world.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/store/sqlite_volume_archive.hpp>

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
    "usage: hog-build-world --world FILE [--planet FILE]\n"
    "                       [--seed N] [--threads N] (--at LAT,LON [--across N] | --all)\n"
    "Makes the world's voxels into FILE ahead of play, a tile column at a time: --across tile\n"
    "columns square about --at (8 by default), or every one with --all. Tile columns already\n"
    "in FILE are left as they are, so a stopped build carries on.\n";

struct Options {
    hog::globe::Planet planet;
    std::optional<std::filesystem::path> world;
    std::uint32_t threads = 0;
    std::optional<std::pair<double, double>> at; // degrees
    std::uint32_t across = 8;
    bool all = false;
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
            std::cerr << std::format("hog-build-world: {}\n", loaded.error());
        }
        options.planet = loaded.value_or(options.planet);
        return loaded.has_value();
    }
    if (name == "--world") {
        options.world = value;
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
    if (name == "--seed") {
        options.planet.seed = static_cast<std::uint64_t>(*given);
    } else if (name == "--threads") {
        options.threads = static_cast<std::uint32_t>(*given);
    } else if (name == "--across") {
        options.across = static_cast<std::uint32_t>(*given);
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
    for (std::size_t k = 0; k < arguments.size(); ++k) {
        if (arguments[k] == "--all") {
            options.all = true;
        } else if (k + 1 >= arguments.size() || !apply(options, arguments[k], arguments[k + 1])) {
            std::cerr << usage;
            return EXIT_FAILURE;
        } else {
            ++k;
        }
    }
    if (!options.world || options.all == options.at.has_value()) {
        std::cerr << usage;
        return EXIT_FAILURE;
    }
    auto archive = hog::store::SqliteVolumeArchive::open(*options.world);
    if (!archive) {
        std::cerr << std::format("hog-build-world: {}\n", archive.error());
        return EXIT_FAILURE;
    }
    const hog::globe::Generator generator{options.planet};
    const std::vector<hog::build_world::Area> areas =
        options.all
            ? hog::build_world::whole(generator)
            : std::vector{hog::globe::around(
                  generator, direction_of(options.at->first, options.at->second), options.across)};
    hog::jobs::JobSystem jobs{options.threads};
    const auto started = std::chrono::steady_clock::now();
    const auto built = hog::build_world::build(
        generator, *archive, jobs, areas, 512, [&](std::size_t done, std::size_t total) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cerr << std::format(
                "\r{} of {} tile columns, {:.0f} s, {:.0f} s left   ", done, total, seconds,
                seconds * static_cast<double>(total - done) / static_cast<double>(done));
        });
    std::cerr << '\n';
    if (!built) {
        std::cerr << std::format("hog-build-world: {}\n", built.error());
        return EXIT_FAILURE;
    }
    std::cout << std::format("{} tile columns made, {} MB; {} already there\n", built->made,
                             built->bytes / 1'000'000, built->skipped);
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span raw{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(raw.begin() + 1, raw.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-build-world: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

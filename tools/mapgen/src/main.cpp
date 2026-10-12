#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/mapgen/weather_render.hpp>
#include <hog/planet/geology.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/weather.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

// The weather settles into its own patterns over this long before the first map.
constexpr std::chrono::days warm_up{10};

int write(const std::filesystem::path& path, const hog::mapgen::Image& image) {
    if (const auto written = hog::mapgen::write_png(path, image); !written) {
        std::cerr << std::format("hog-mapgen: {}\n", written.error());
        return EXIT_FAILURE;
    }
    std::cout << path.string() << '\n';
    return EXIT_SUCCESS;
}

int render_weather(const hog::mapgen::Options& options, const hog::planet::PlanetConfig& config,
                   hog::jobs::JobSystem& jobs) {
    const hog::planet::World world{config};
    const hog::sim::WeatherConfig weather_config{
        .level = std::min(hog::planet::level_for(options.weather.cell_km * 1'000.0, config.radius),
                          hog::sim::CellGrid::max_level),
        .axial_tilt = config.axial_tilt,
        .planet_radius = config.radius,
        .seed = config.seed,
    };
    const hog::GameTime start{options.weather.start.time_since_epoch()};
    hog::sim::Weather weather{weather_config,
                              hog::sim::ground_for(hog::sim::CellGrid{weather_config.level}, world),
                              start - warm_up};
    const hog::GameTime end = start + std::chrono::days{options.weather.days};
    for (hog::GameTime time = start; time <= end;
         time += std::chrono::hours{options.weather.every}) {
        weather.advance_to(time, jobs);
        for (const auto map : {hog::mapgen::WeatherMap::Sky, hog::mapgen::WeatherMap::Air}) {
            const auto image = hog::mapgen::render_weather(world.geology(), weather, config.radius,
                                                           options.view, map, options.size, jobs);
            const auto path =
                options.output / hog::mapgen::weather_file_name(options.view, map, time);
            if (write(path, image) != EXIT_SUCCESS) {
                return EXIT_FAILURE;
            }
        }
    }
    return EXIT_SUCCESS;
}

int run(std::span<const std::string_view> arguments) {
    const auto options = hog::mapgen::parse_options(arguments);
    if (!options) {
        std::cerr << std::format("hog-mapgen: {}\n\n{}", options.error().message,
                                 hog::mapgen::usage());
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::cout << hog::mapgen::usage();
        return EXIT_SUCCESS;
    }
    auto config = hog::planet::load_planet_config(options->config);
    if (!config) {
        std::cerr << std::format("hog-mapgen: {}\n", config.error());
        return EXIT_FAILURE;
    }
    if (options->seed) {
        config->seed = *options->seed;
    }
    std::error_code error;
    std::filesystem::create_directories(options->output, error);
    if (error) {
        std::cerr << std::format("hog-mapgen: creating {}: {}\n", options->output.string(),
                                 error.message());
        return EXIT_FAILURE;
    }

    hog::jobs::JobSystem jobs;
    if (options->weather.days > 0) {
        return render_weather(*options, *config, jobs);
    }
    const hog::planet::Geology geology{*config};
    for (const hog::mapgen::Layer layer : options->layers) {
        const auto image =
            hog::mapgen::render(geology, config->radius, options->view, layer, options->size, jobs);
        if (write(options->output / hog::mapgen::file_name(options->view, layer), image) !=
            EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
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
        std::cerr << "hog-mapgen: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

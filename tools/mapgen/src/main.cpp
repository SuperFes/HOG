#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/mapgen/season_render.hpp>
#include <hog/mapgen/weather_render.hpp>
#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/geology.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/ecology.hpp>
#include <hog/sim/forest.hpp>
#include <hog/sim/vegetation.hpp>
#include <hog/sim/weather.hpp>
#include <hog/sim/wildlife.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

// The weather settles into its own patterns over this long before the first map.
constexpr std::chrono::days warm_up{10};
// Long enough for the weather's running means of rain and warmth to forget where they started.
constexpr std::chrono::days season_warm_up{60};

int write(const std::filesystem::path& path, const hog::mapgen::Image& image) {
    if (const auto written = hog::mapgen::write_png(path, image); !written) {
        std::cerr << std::format("hog-mapgen: {}\n", written.error());
        return EXIT_FAILURE;
    }
    std::cout << path.string() << '\n';
    return EXIT_SUCCESS;
}

hog::sim::WeatherConfig weather_for(const hog::mapgen::Options& options,
                                    const hog::planet::PlanetConfig& config) {
    return hog::sim::WeatherConfig{
        .level = std::min(hog::planet::level_for(options.weather.cell_km * 1'000.0, config.radius),
                          hog::sim::CellGrid::max_level),
        .axial_tilt = config.axial_tilt,
        .planet_radius = config.radius,
        .seed = config.seed,
    };
}

int render_weather(const hog::mapgen::Options& options, const hog::planet::PlanetConfig& config,
                   hog::jobs::JobSystem& jobs) {
    const hog::planet::World world{config};
    const hog::sim::WeatherConfig weather_config = weather_for(options, config);
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

// Steps the weather and wildlife together, as the server does, writing maps and totals as it goes.
int render_wildlife(const hog::mapgen::Options& options, const hog::planet::PlanetConfig& config,
                    hog::jobs::JobSystem& jobs) {
    auto ecology = hog::sim::load_ecology(options.wildlife.ecology);
    if (!ecology) {
        std::cerr << std::format("hog-mapgen: {}: {}\n", options.wildlife.ecology.string(),
                                 ecology.error());
        return EXIT_FAILURE;
    }
    const hog::planet::World world{config};
    const hog::sim::WeatherConfig weather_config = weather_for(options, config);
    const hog::sim::CellGrid grid{weather_config.level};
    const hog::GameTime start{options.weather.start.time_since_epoch()};
    hog::sim::Weather weather{weather_config, hog::sim::ground_for(grid, world), start - warm_up};
    weather.advance_to(start, jobs);
    hog::sim::WildlifeConfig wildlife_config{
        .level = weather_config.level, .planet_radius = config.radius, .seed = config.seed};
    wildlife_config.ecology = std::move(*ecology);
    hog::sim::Wildlife wildlife{wildlife_config, hog::sim::habitat_for(grid, world), start};

    const auto csv_path = options.output / "wildlife.csv";
    std::ofstream csv{csv_path};
    csv << hog::mapgen::wildlife_header(wildlife.config().ecology) << '\n';
    const hog::GameTime end = start + std::chrono::days{365 * options.wildlife.years};
    for (hog::GameTime time = start; time <= end;
         time += std::chrono::days{options.wildlife.every}) {
        while (wildlife.next_step() <= time) {
            const hog::GameTime at = wildlife.next_step();
            weather.advance_to(at, jobs);
            wildlife.advance_to(at, hog::sim::conditions_of(weather), jobs);
        }
        csv << hog::mapgen::wildlife_row(wildlife) << '\n';
        const auto& species = wildlife.config().ecology.species;
        for (std::uint32_t kind = 0; kind < species.size(); ++kind) {
            const auto image = hog::mapgen::render_wildlife(
                world.geology(), wildlife, kind, config.radius, options.view, options.size, jobs);
            const auto path = options.output / hog::mapgen::wildlife_file_name(
                                                   options.view, species[kind].name, time);
            if (write(path, image) != EXIT_SUCCESS) {
                return EXIT_FAILURE;
            }
        }
    }
    if (!csv) {
        std::cerr << std::format("hog-mapgen: writing {} failed\n", csv_path.string());
        return EXIT_FAILURE;
    }
    std::cout << csv_path.string() << '\n';
    return EXIT_SUCCESS;
}

// The same day as the start `months` later, or the month's last where it's shorter.
std::chrono::sys_days months_after(std::chrono::sys_days start, int months) {
    const std::chrono::year_month_day date{start};
    const std::chrono::year_month month =
        std::chrono::year_month{date.year(), date.month()} + std::chrono::months{months};
    const std::chrono::day last = std::chrono::year_month_day_last{month / std::chrono::last}.day();
    return std::chrono::sys_days{month / std::min(date.day(), last)};
}

// Steps the weather, land and plants together for a year, as the server does, writing each map
// once a month and a sheet of the year's frames per map.
int render_seasons(const hog::mapgen::Options& options, const hog::planet::PlanetConfig& config,
                   hog::jobs::JobSystem& jobs) {
    auto vegetation = hog::sim::load_vegetation(options.seasons.vegetation);
    if (!vegetation) {
        std::cerr << std::format("hog-mapgen: {}: {}\n", options.seasons.vegetation.string(),
                                 vegetation.error());
        return EXIT_FAILURE;
    }
    const hog::planet::World world{config};
    const hog::sim::WeatherConfig weather_config = weather_for(options, config);
    const hog::sim::CellGrid grid{weather_config.level};
    const hog::GameTime start{options.weather.start.time_since_epoch()};
    hog::sim::Weather weather{weather_config, hog::sim::ground_for(grid, world),
                              start - season_warm_up};
    weather.advance_to(start, jobs);
    hog::sim::Forest forest{hog::sim::ForestConfig{.level = weather_config.level,
                                                   .planet_radius = config.radius,
                                                   .seed = config.seed,
                                                   .vegetation = *vegetation},
                            hog::sim::soil_for(grid, world), start};

    constexpr int months = 12;
    std::array<std::vector<hog::mapgen::Image>, hog::mapgen::season_map_count> frames;
    for (int month = 0; month < months; ++month) {
        const std::chrono::sys_days day = months_after(options.weather.start, month);
        const hog::GameTime time{day.time_since_epoch()};
        while (forest.next_step() <= time) {
            const hog::GameTime at = forest.next_step();
            weather.advance_to(at, jobs);
            forest.advance_to(at, hog::sim::forest_conditions_of(weather), jobs);
        }
        weather.advance_to(time, jobs);
        const std::chrono::year_month_day date{day};
        for (int map = 0; map < hog::mapgen::season_map_count; ++map) {
            const auto kind = static_cast<hog::mapgen::SeasonMap>(map);
            auto image = hog::mapgen::render_season(world.geology(), weather, forest, config.radius,
                                                    options.view, kind, options.size, jobs);
            const auto path = options.output / hog::mapgen::season_file_name(
                                                   options.view, kind, date.year() / date.month());
            if (write(path, image) != EXIT_SUCCESS) {
                return EXIT_FAILURE;
            }
            frames.at(static_cast<std::size_t>(map)).push_back(std::move(image));
        }
    }
    constexpr int columns = 4;
    for (int map = 0; map < hog::mapgen::season_map_count; ++map) {
        const auto kind = static_cast<hog::mapgen::SeasonMap>(map);
        const auto path = options.output / hog::mapgen::season_sheet_name(options.view, kind);
        if (write(path, hog::mapgen::contact_sheet(frames.at(static_cast<std::size_t>(map)),
                                                   columns)) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
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
    if (options->wildlife.years > 0) {
        return render_wildlife(*options, *config, jobs);
    }
    if (options->seasons.on) {
        return render_seasons(*options, *config, jobs);
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

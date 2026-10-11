#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/geology.hpp>
#include <hog/planet/planet_config.hpp>

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

    const hog::planet::Geology geology{*config};
    hog::jobs::JobSystem jobs;
    for (const hog::mapgen::Layer layer : options->layers) {
        const auto image =
            hog::mapgen::render(geology, config->radius, options->view, layer, options->size, jobs);
        const auto path = options->output / hog::mapgen::file_name(options->view, layer);
        if (const auto written = hog::mapgen::write_png(path, image); !written) {
            std::cerr << std::format("hog-mapgen: {}\n", written.error());
            return EXIT_FAILURE;
        }
        std::cout << path.string() << '\n';
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

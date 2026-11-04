#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/stills/stills.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view usage =
    "usage: hog-stills [--preset earth|mars|venus|moon | --planet FILE] [--seed N] [--out DIR]\n"
    "                  [--width N] [--threads N] [--at LAT,LON[,HEADING]] [--span M]\n"
    "                  [--bottom M] [--top M] [--depth M] [--dig M] [--drainage LEVEL]\n"
    "Writes map.png, and with --at, relief.png and section.png there; the section runs from\n"
    "--bottom to --top metres off the ground under its middle. --depth adds plan.png, a slice\n"
    "that far under the ground everywhere, open space dark. --dig digs a ball that wide in the\n"
    "ground under --at and adds voxels.png, the section's window drawn from the voxels.\n";

struct Options {
    hog::globe::Planet planet;
    std::filesystem::path out = ".";
    int width = 1024;
    int threads = 4;
    std::optional<hog::stills::Place> at;
    double span = 20'000.0;
    double bottom = -500.0; // metres from the ground under --at
    double top = 500.0;
    std::optional<double> depth; // of a plan slice under the ground
    std::optional<double> dig;   // a ball's radius, dug at the ground under --at
    std::optional<int> drainage; // the cell level to work out where rain runs on, for drainage.png
};

// strtod rather than from_chars, which some standard libraries lack for floating point.
std::optional<std::vector<double>> numbers(std::string_view text) {
    std::vector<double> values;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string part{text.substr(0, comma)};
        char* end = nullptr;
        const double value = std::strtod(part.c_str(), &end);
        if (part.empty() || end != std::to_address(part.end()) || !std::isfinite(value)) {
            return std::nullopt;
        }
        values.push_back(value);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return values;
}

std::optional<double> number(std::string_view text) {
    const auto list = numbers(text);
    return list && list->size() == 1 ? std::optional{list->front()} : std::nullopt;
}

// Each option and its value; false on anything it doesn't know.
bool apply(Options& options, std::string_view name, std::string_view value) {
    if (name == "--preset") {
        const auto planet = hog::globe::preset(value);
        options.planet = planet.value_or(options.planet);
        return planet.has_value();
    }
    if (name == "--planet") {
        const auto loaded = hog::globe::load_planet(value);
        if (!loaded) {
            std::cerr << std::format("hog-stills: {}\n", loaded.error());
        }
        options.planet = loaded.value_or(options.planet);
        return loaded.has_value();
    }
    if (name == "--out") {
        options.out = value;
        return true;
    }
    if (name == "--at") {
        const auto list = numbers(value);
        if (!list || list->size() < 2 || list->size() > 3) {
            return false;
        }
        options.at = hog::stills::Place{.latitude = (*list)[0],
                                        .longitude = (*list)[1],
                                        .heading = list->size() == 3 ? (*list)[2] : 90.0};
        return true;
    }
    const auto given = number(value);
    if (!given) {
        return false;
    }
    if (name == "--seed") {
        options.planet.seed = static_cast<std::uint64_t>(*given);
    } else if (name == "--width") {
        options.width = static_cast<int>(*given);
    } else if (name == "--threads") {
        options.threads = static_cast<int>(*given);
    } else if (name == "--span") {
        options.span = *given;
    } else if (name == "--bottom") {
        options.bottom = *given;
    } else if (name == "--top") {
        options.top = *given;
    } else if (name == "--depth") {
        options.depth = given;
    } else if (name == "--dig") {
        options.dig = given;
    } else if (name == "--drainage") {
        options.drainage = static_cast<int>(*given);
    } else {
        return false;
    }
    return true;
}

int write(const std::filesystem::path& path, const hog::stills::Image& image) {
    if (const auto written = hog::stills::write_png(path, image); !written) {
        std::cerr << std::format("hog-stills: {}\n", written.error());
        return EXIT_FAILURE;
    }
    std::cout << path.string() << '\n';
    return EXIT_SUCCESS;
}

int run(std::span<const std::string_view> arguments) {
    Options options;
    options.planet = hog::globe::preset("earth").value_or(options.planet);
    if (arguments.size() % 2 != 0) {
        std::cerr << usage;
        return EXIT_FAILURE;
    }
    for (std::size_t k = 0; k < arguments.size(); k += 2) {
        if (!apply(options, arguments[k], arguments[k + 1])) {
            std::cerr << usage;
            return EXIT_FAILURE;
        }
    }
    std::filesystem::create_directories(options.out);
    const hog::globe::Generator generator{options.planet};
    if (const int failed = write(options.out / "map.png",
                                 hog::stills::map(generator, options.width, options.threads))) {
        return failed;
    }
    if (options.drainage) {
        if (const int failed = write(options.out / "drainage.png",
                                     hog::stills::drainage(generator, options.width,
                                                           *options.drainage, options.threads))) {
            return failed;
        }
    }
    if (!options.at) {
        return EXIT_SUCCESS;
    }
    if (const int failed = write(options.out / "relief.png",
                                 hog::stills::relief(generator, *options.at, options.span,
                                                     options.width, options.threads))) {
        return failed;
    }
    if (options.depth) {
        if (const int failed =
                write(options.out / "plan.png",
                      hog::stills::plan(generator, *options.at, options.span, *options.depth,
                                        options.width, options.threads))) {
            return failed;
        }
    }
    // The section's window is taken from the ground under its middle.
    const double ground =
        hog::globe::ground_below(generator, options.at->direction(), options.span / options.width)
            .value_or(options.planet.radius) -
        options.planet.radius;
    if (const int failed = write(options.out / "section.png",
                                 hog::stills::section(generator, *options.at, options.span,
                                                      ground + options.bottom, ground + options.top,
                                                      options.width, options.threads))) {
        return failed;
    }
    if (!options.dig) {
        return 0;
    }
    hog::globe::VoxelStore store{generator.layers()};
    hog::globe::World world{generator, store};
    const double radius = options.planet.radius;
    hog::globe::dig(world, options.at->direction() * (radius + ground), *options.dig);
    return write(options.out / "voxels.png",
                 hog::stills::voxel_section(world, *options.at, options.span,
                                            ground + options.bottom, ground + options.top,
                                            options.width));
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span raw{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(raw.begin() + 1, raw.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-stills: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

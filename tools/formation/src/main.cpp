#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/sphere.hpp>
#include <hog/formation_lab/pictures.hpp>

#include <hog/core/image.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using hog::DVec3;
using hog::formation::Sphere;
using hog::formation_lab::Rgb;

constexpr std::string_view usage =
    "usage: hog-formation [--seed N] [--plates N] [--continents N] [--land SHARE]\n"
    "                     [--speed DEG_PER_MYR] [--age MYR] [--radius M]\n"
    "                     [--layout SPLITS] [--detail SPLITS] [--width N] [--out DIR]\n"
    "Forms a planet in stages and writes a map of each stage's result to DIR:\n"
    "plates.png (plates, light crust, and how they meet), base.png (how the crust stands),\n"
    "uplift.png (where rock is pushed up), temperature.png and rain.png (yearly means).\n";

struct Options {
    hog::formation::Body body;
    int layout = 5; // splits of the sphere the plates are laid out on
    int detail = 7; // splits of the sphere the results are held on
    int width = 2048;
    std::filesystem::path out = ".";
};

std::optional<double> number(std::string_view text) {
    const std::string copy{text};
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != std::to_address(copy.end()) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

bool apply(Options& options, std::string_view name, std::string_view value) {
    if (name == "--out") {
        options.out = value;
        return true;
    }
    const auto parsed = number(value);
    if (!parsed) {
        return false;
    }
    const double n = *parsed;
    const auto whole = [n] { return static_cast<int>(std::lround(n)); };
    if (name == "--seed") {
        options.body.seed = static_cast<std::uint64_t>(std::llround(std::abs(n)));
    } else if (name == "--plates") {
        options.body.plates = std::clamp(whole(), 1, 1000);
    } else if (name == "--continents") {
        options.body.continents = std::clamp(whole(), 1, 1000);
    } else if (name == "--land") {
        options.body.land_share = std::clamp(n, 0.0, 1.0);
    } else if (name == "--speed") {
        options.body.plate_speed = std::max(n, 0.0);
    } else if (name == "--age") {
        options.body.boundary_age = std::max(n, 0.0);
    } else if (name == "--radius") {
        options.body.radius = std::max(n, 1.0);
    } else if (name == "--layout") {
        options.layout = std::clamp(whole(), 2, Sphere::max_subdivisions);
    } else if (name == "--detail") {
        options.detail = std::clamp(whole(), 2, Sphere::max_subdivisions);
    } else if (name == "--width") {
        options.width = std::clamp(whole(), 64, 16'384);
    } else {
        return false;
    }
    return true;
}

bool write(const std::filesystem::path& path, const hog::Image& image) {
    if (const auto written = hog::write_png(path, image); !written) {
        std::cerr << "hog-formation: can't write " << path.string() << "\n";
        return false;
    }
    std::cout << "wrote " << path.string() << "\n";
    return true;
}

double seconds_since(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

int run(const Options& options) {
    const hog::formation::Body& body = options.body;
    auto at = std::chrono::steady_clock::now();
    const Sphere layout{options.layout, body.seed};
    const Sphere sphere{options.detail, body.seed + 1};
    std::cout << std::format("spheres: {} and {} points in {:.2f} s\n", layout.size(),
                             sphere.size(), seconds_since(at));
    at = std::chrono::steady_clock::now();
    const hog::formation::Plates plates{body, layout};
    const hog::formation::Crust crust = hog::formation::crust_on(sphere, plates, body);
    std::cout << std::format("plates and crust in {:.2f} s\n", seconds_since(at));

    std::filesystem::create_directories(options.out);
    const double step = 3.14159265358979 / (options.width / 2.0);
    bool ok = true;

    std::uint32_t start = 0;
    ok = write(options.out / "plates.png",
               hog::formation_lab::map_picture(options.width, [&](const DVec3& direction) {
                   start = sphere.nearest(direction, start);
                   if (crust.meeting[start] != hog::formation::Meeting::None &&
                       crust.boundary[start] < 1.2 * sphere.spacing()) {
                       return hog::formation_lab::meeting_colour(crust.meeting[start]);
                   }
                   const Rgb plate = hog::formation_lab::plate_colour(crust.plate[start]);
                   return hog::formation_lab::scaled(plate, 0.55 + (0.5 * crust.light[start]));
               })) &&
         ok;

    std::uint32_t lit = 0;
    ok = write(options.out / "base.png",
               hog::formation_lab::map_picture(options.width, [&](const DVec3& direction) {
                   const Sphere::Spot spot = sphere.locate(direction, start);
                   start = spot.corners[0];
                   const double height = sphere.sample(crust.base, spot);
                   const double light = hog::formation_lab::shade(
                       sphere, crust.base, direction, body.radius, step, 1.0, lit);
                   return hog::formation_lab::scaled(hog::formation_lab::height_colour(height),
                                                     height < 0.0 ? 1.0 : light);
               })) &&
         ok;

    const double most = std::max(1e-12, *std::ranges::max_element(crust.uplift));
    static constexpr std::array<Rgb, 5> heat{
        {{20, 20, 30}, {60, 40, 120}, {180, 50, 90}, {240, 140, 40}, {250, 240, 160}}};
    ok = write(options.out / "uplift.png",
               hog::formation_lab::map_picture(options.width, [&](const DVec3& direction) {
                   const Sphere::Spot spot = sphere.locate(direction, start);
                   start = spot.corners[0];
                   const double share = sphere.sample(crust.uplift, spot) / most;
                   return hog::formation_lab::ramp(std::sqrt(std::max(share, 0.0)), heat);
               })) &&
         ok;

    at = std::chrono::steady_clock::now();
    const hog::formation::Climate climate =
        hog::formation::climate_on(sphere, crust.base, body);
    std::cout << std::format("climate in {:.2f} s\n", seconds_since(at));
    static constexpr std::array<Rgb, 6> cold_to_hot{{{40, 40, 140},
                                                     {80, 140, 220},
                                                     {220, 230, 230},
                                                     {240, 200, 80},
                                                     {220, 90, 40},
                                                     {120, 20, 20}}};
    ok = write(options.out / "temperature.png",
               hog::formation_lab::map_picture(options.width, [&](const DVec3& direction) {
                   const Sphere::Spot spot = sphere.locate(direction, start);
                   start = spot.corners[0];
                   const double celsius = sphere.sample(climate.temperature, spot);
                   const Rgb colour = hog::formation_lab::ramp((celsius + 40.0) / 80.0, cold_to_hot);
                   return sphere.sample(crust.base, spot) < 0.0
                              ? hog::formation_lab::scaled(colour, 0.7)
                              : colour;
               })) &&
         ok;
    static constexpr std::array<Rgb, 5> dry_to_wet{
        {{150, 90, 40}, {220, 200, 120}, {120, 190, 90}, {30, 120, 70}, {20, 60, 140}}};
    ok = write(options.out / "rain.png",
               hog::formation_lab::map_picture(options.width, [&](const DVec3& direction) {
                   const Sphere::Spot spot = sphere.locate(direction, start);
                   start = spot.corners[0];
                   const double metres = sphere.sample(climate.rain, spot);
                   const Rgb colour =
                       hog::formation_lab::ramp(std::sqrt(std::max(metres, 0.0) / 3.0), dry_to_wet);
                   return sphere.sample(crust.base, spot) < 0.0
                              ? hog::formation_lab::scaled(colour, 0.45)
                              : colour;
               })) &&
         ok;
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        const std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
        for (std::size_t n = 1; n < arguments.size(); n += 2) {
            const std::string_view name = arguments[n];
            if (name == "--help" || n + 1 >= arguments.size() ||
                !apply(options, name, arguments[n + 1])) {
                std::cerr << usage;
                return name == "--help" ? 0 : 2;
            }
        }
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "hog-formation: " << error.what() << "\n";
        return 1;
    }
}

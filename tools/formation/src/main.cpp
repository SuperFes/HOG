#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/soil.hpp>
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
#include <numbers>
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
    "uplift.png (where rock is pushed up), temperature.png and rain.png (yearly means),\n"
    "height.png (land worn by its rivers) and soil.png (how deep soil lies).\n";

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

// What every map is drawn with: where it goes, how wide, and the sphere results are held on.
struct Canvas {
    const Options* options = nullptr;
    const Sphere* sphere = nullptr;
    double step = 0.0; // radians between the samples relief is lit from

    [[nodiscard]] bool draw(std::string_view name,
                            const std::function<Rgb(const DVec3&, const Sphere::Spot&)>& colour) const {
        std::uint32_t start = 0;
        return write(options->out / name,
                     hog::formation_lab::map_picture(options->width, [&](const DVec3& direction) {
                         const Sphere::Spot spot = sphere->locate(direction, start);
                         start = spot.corners[0];
                         return colour(direction, spot);
                     }));
    }

    [[nodiscard]] double light(std::span<const double> heights, const DVec3& direction,
                               std::uint32_t& start) const {
        return hog::formation_lab::shade(*sphere, heights, direction, options->body.radius, step,
                                         1.0, start);
    }
};

int river_width(double share) {
    if (share > 0.1) {
        return 3;
    }
    return share > 0.02 ? 2 : 1;
}

bool draw_crust(const Canvas& canvas, const hog::formation::Crust& crust) {
    const Sphere& sphere = *canvas.sphere;
    bool ok = canvas.draw("plates.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (crust.meeting[near] != hog::formation::Meeting::None &&
            crust.boundary[near] < 1.2 * sphere.spacing()) {
            return hog::formation_lab::meeting_colour(crust.meeting[near]);
        }
        const Rgb plate = hog::formation_lab::plate_colour(crust.plate[near]);
        return hog::formation_lab::scaled(plate, 0.55 + (0.5 * crust.light[near]));
    });
    std::uint32_t lit = 0;
    ok = canvas.draw("base.png",
                     [&](const DVec3& direction, const Sphere::Spot& spot) {
                         const double height = sphere.sample(crust.base, spot);
                         const double light = canvas.light(crust.base, direction, lit);
                         return hog::formation_lab::scaled(
                             hog::formation_lab::height_colour(height), height < 0.0 ? 1.0 : light);
                     }) &&
         ok;
    const double most = std::max(1e-12, *std::ranges::max_element(crust.uplift));
    static constexpr std::array<Rgb, 5> heat{hog::formation_lab::rgb(20, 20, 30),
                                             hog::formation_lab::rgb(60, 40, 120),
                                             hog::formation_lab::rgb(180, 50, 90),
                                             hog::formation_lab::rgb(240, 140, 40),
                                             hog::formation_lab::rgb(250, 240, 160)};
    return canvas.draw("uplift.png",
                       [&](const DVec3&, const Sphere::Spot& spot) {
                           const double share = sphere.sample(crust.uplift, spot) / most;
                           return hog::formation_lab::ramp(std::sqrt(std::max(share, 0.0)), heat);
                       }) &&
           ok;
}

bool draw_climate(const Canvas& canvas, const hog::formation::Crust& crust,
                  const hog::formation::Climate& climate) {
    const Sphere& sphere = *canvas.sphere;
    static constexpr std::array<Rgb, 6> cold_to_hot{
        hog::formation_lab::rgb(40, 40, 140),  hog::formation_lab::rgb(80, 140, 220),
        hog::formation_lab::rgb(220, 230, 230), hog::formation_lab::rgb(240, 200, 80),
        hog::formation_lab::rgb(220, 90, 40),  hog::formation_lab::rgb(120, 20, 20)};
    const bool ok = canvas.draw("temperature.png", [&](const DVec3&, const Sphere::Spot& spot) {
        const double celsius = sphere.sample(climate.temperature, spot);
        const Rgb colour = hog::formation_lab::ramp((celsius + 40.0) / 80.0, cold_to_hot);
        const bool sea = sphere.sample(crust.base, spot) < 0.0;
        return sea ? hog::formation_lab::scaled(colour, 0.7) : colour;
    });
    static constexpr std::array<Rgb, 5> dry_to_wet{
        hog::formation_lab::rgb(150, 90, 40), hog::formation_lab::rgb(220, 200, 120),
        hog::formation_lab::rgb(120, 190, 90), hog::formation_lab::rgb(30, 120, 70),
        hog::formation_lab::rgb(20, 60, 140)};
    return canvas.draw("rain.png",
                       [&](const DVec3&, const Sphere::Spot& spot) {
                           const double metres = sphere.sample(climate.rain, spot);
                           const Rgb colour = hog::formation_lab::ramp(
                               std::sqrt(std::max(metres, 0.0) / 3.0), dry_to_wet);
                           const bool sea = sphere.sample(crust.base, spot) < 0.0;
                           return sea ? hog::formation_lab::scaled(colour, 0.45) : colour;
                       }) &&
           ok;
}

// Standing water over the land, if any: open lakes blue, closed ones greener, salt flats white.
std::optional<Rgb> water_colour(const hog::formation::Lakes& lakes, std::uint32_t near) {
    if (lakes.salt_flat[near] != 0) {
        return hog::formation_lab::rgb(235, 230, 215);
    }
    if (lakes.lake[near] == hog::formation::Lakes::none) {
        return std::nullopt;
    }
    return lakes.lakes[lakes.lake[near]].open ? hog::formation_lab::rgb(50, 110, 200)
                                              : hog::formation_lab::rgb(60, 150, 150);
}

bool draw_rivers(const Canvas& canvas, const hog::formation::Crust& crust,
                 const hog::formation::Rivers& rivers, const hog::formation::Lakes& lakes) {
    const Sphere& sphere = *canvas.sphere;
    std::uint32_t start = 0;
    std::uint32_t lit = 0;
    hog::Image map =
        hog::formation_lab::map_picture(canvas.options->width, [&](const DVec3& direction) {
            const Sphere::Spot spot = sphere.locate(direction, start);
            start = spot.corners[0];
            const double height = sphere.sample(rivers.height, spot);
            if (height < 0.0 && crust.sea[sphere.nearest(direction, start)] != 0) {
                return hog::formation_lab::height_colour(height);
            }
            if (const auto water = water_colour(lakes, sphere.nearest(direction, start))) {
                return *water;
            }
            return hog::formation_lab::scaled(hog::formation_lab::height_colour(height),
                                              canvas.light(rivers.height, direction, lit));
        });
    // Each river drawn from a point to where its water goes, wider as more passes.
    double biggest = 1.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (crust.sea[point] == 0) {
            biggest = std::max(biggest, lakes.discharge[point]);
        }
    }
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const double share = lakes.discharge[point] / biggest;
        const bool in_lake = lakes.lake[point] != hog::formation::Lakes::none;
        if (crust.sea[point] == 0 && !in_lake && share >= 0.003) {
            hog::formation_lab::draw_line(map, sphere.point(point),
                                          sphere.point(rivers.receiver[point]),
                                          hog::formation_lab::rgb(40, 90, 200), river_width(share));
        }
    }
    return write(canvas.options->out / "height.png", map);
}

bool draw_soil(const Canvas& canvas, const hog::formation::Rivers& rivers,
               const hog::formation::Soil& soil) {
    const Sphere& sphere = *canvas.sphere;
    static constexpr std::array<Rgb, 5> bare_to_deep{
        hog::formation_lab::rgb(150, 150, 150), hog::formation_lab::rgb(210, 190, 140),
        hog::formation_lab::rgb(170, 120, 70), hog::formation_lab::rgb(110, 70, 40),
        hog::formation_lab::rgb(60, 35, 20)};
    std::uint32_t lit = 0;
    return canvas.draw("soil.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        if (sphere.sample(rivers.height, spot) < 0.0) {
            return hog::formation_lab::rgb(20, 30, 60);
        }
        const double depth = sphere.sample(soil.depth, spot);
        return hog::formation_lab::scaled(hog::formation_lab::ramp(depth / 2.5, bare_to_deep),
                                          canvas.light(rivers.height, direction, lit));
    });
}

double soiled_share(const Sphere& sphere, const hog::formation::Crust& crust,
                    const hog::formation::Soil& soil) {
    double soiled = 0.0;
    double land = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (crust.sea[point] == 0) {
            land += sphere.area(point);
            soiled += soil.depth[point] > 0.0 ? sphere.area(point) : 0.0;
        }
    }
    return soiled / std::max(land, 1e-12);
}

int run(const Options& options) {
    const hog::formation::Body& body = options.body;
    auto at = std::chrono::steady_clock::now();
    const Sphere layout{options.layout, body.seed};
    const Sphere sphere{options.detail, body.seed + 1};
    std::cout << std::format("spheres: {} and {} points in {:.2f} s\n", layout.size(),
                             sphere.size(), seconds_since(at));
    std::filesystem::create_directories(options.out);
    const Canvas canvas{
        .options = &options, .sphere = &sphere, .step = std::numbers::pi / (options.width / 2.0)};

    at = std::chrono::steady_clock::now();
    const hog::formation::Plates plates{body, layout};
    const hog::formation::Crust crust = hog::formation::crust_on(sphere, plates, body);
    std::cout << std::format("plates and crust in {:.2f} s\n", seconds_since(at));
    bool ok = draw_crust(canvas, crust);

    at = std::chrono::steady_clock::now();
    const hog::formation::Climate climate = hog::formation::climate_on(sphere, crust.base, crust.sea, body);
    std::cout << std::format("climate in {:.2f} s\n", seconds_since(at));
    ok = draw_climate(canvas, crust, climate) && ok;

    at = std::chrono::steady_clock::now();
    const hog::formation::Rivers rivers = hog::formation::rivers_on(sphere, crust, climate, body);
    std::cout << std::format("rivers in {:.2f} s, settled after {} routings, highest {:.0f} m\n",
                             seconds_since(at), rivers.rounds,
                             *std::ranges::max_element(rivers.height));
    const hog::formation::Lakes lakes = hog::formation::lakes_on(sphere, rivers, climate, body);
    const auto open = std::ranges::count_if(lakes.lakes, [](const auto& lake) { return lake.open; });
    std::cout << std::format("{} lakes, {} open and {} closed; {} salt flats\n",
                             lakes.lakes.size(), open,
                             static_cast<std::ptrdiff_t>(lakes.lakes.size()) - open,
                             std::ranges::count(lakes.salt_flat, std::uint8_t{1}));
    ok = draw_rivers(canvas, crust, rivers, lakes) && ok;

    const hog::formation::Soil soil = hog::formation::soil_on(sphere, rivers, climate, body);
    std::cout << std::format("soil on {:.0f}% of the land, deepest {:.2f} m\n",
                             100.0 * soiled_share(sphere, crust, soil),
                             *std::ranges::max_element(soil.depth));
    ok = draw_soil(canvas, rivers, soil) && ok;
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

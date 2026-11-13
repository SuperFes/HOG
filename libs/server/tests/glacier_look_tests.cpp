#include <hog/flow/simulation.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>
#include <hog/server/ice_bake.hpp>
#include <hog/server/ice_climate.hpp>
#include <hog/testing/world.hpp>
#include <hog/weather/field.hpp>

#include <catch2/catch_test_macros.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <print>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using hog::DVec3;
using namespace hog::globe;

// The air's temperature over a column, brought down to the reference level at the lapse rate.
double reference_temperature(const Generator& generator, const Generator::Column& column) {
    const Planet& planet = generator.planet();
    const double height = std::max(column.ground(), planet.body.radius) + 20.0;
    return column.sample(height).temperature +
           (hog::weather::lapse_rate(planet) * (height - planet.body.radius));
}

// Bakes ice onto the highest mountain of the default planet and writes a top-down picture of it
// to $HOG_LOOK_DIR. Hidden: it takes a while and is for looking at.
TEST_CASE("a mountain's snow settles into glaciers", "[.glacier_look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* dir = std::getenv("HOG_LOOK_DIR");
    REQUIRE(dir != nullptr);
    const Generator& generator = hog::testing::small_world();
    const Layers& layers = generator.layers();
    const Planet& planet = generator.planet();

    // The highest ground over a sphere of directions.
    DVec3 peak{0.0, 0.0, 1.0};
    double highest = 0.0;
    constexpr int points = 40000;
    for (int k = 0; k < points; ++k) {
        const double z = 1.0 - (((2.0 * k) + 1.0) / points);
        const double r = std::sqrt(1.0 - (z * z));
        const double a = 2.399963 * k;
        const DVec3 d{r * std::cos(a), r * std::sin(a), z};
        const double ground = generator.column(d, 200.0).ground();
        if (ground > highest) {
            highest = ground;
            peak = d;
        }
    }
    std::println("peak {:.0f} m over the reference, direction {:.3f} {:.3f} {:.3f}",
                 highest - planet.body.radius, peak.x, peak.y, peak.z);

    constexpr std::uint32_t side = 320;
    const auto at = [](std::uint32_t x, std::uint32_t y) { return (std::size_t{y} * side) + x; };
    const Cell centre = cell_under(peak, layers.level);
    const std::uint32_t i0 = centre.i - (side / 2);
    const std::uint32_t j0 = centre.j - (side / 2);
    const auto direction_of = [&](std::uint32_t i, std::uint32_t j) {
        return glm::normalize(middle_of({.face = centre.face, .i = i, .j = j, .layer = 0}, layers));
    };
    // The ground and temperature of every column, and the bricks from its ground to 200 m over.
    std::set<BrickSite> bricks;
    std::vector<double> ground(std::size_t{side} * side);
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const auto column = generator.column(direction_of(i0 + x, j0 + y), 1.0);
            ground[at(x, y)] = column.ground();
            const int low = std::max(layers.layer_at(column.ground()) - 1, 0);
            const int high = layers.layer_at(column.ground() + 250.0);
            for (auto layer = static_cast<std::uint32_t>(low); std::cmp_less_equal(layer, high);
                 layer += brick_side) {
                bricks.insert(
                    brick_of({.face = centre.face, .i = i0 + x, .j = j0 + y, .layer = layer}));
            }
            bricks.insert(brick_of({.face = centre.face,
                                    .i = i0 + x,
                                    .j = j0 + y,
                                    .layer = static_cast<std::uint32_t>(high)}));
        }
    }
    std::println("{} bricks", bricks.size());
    {
        const hog::weather::Climate here{
            .surface = reference_temperature(generator, generator.column(peak, 1.0)),
            .swing = 12.0,
            .precipitation = 0.15};
        const auto line = hog::weather::snowline(planet, here, 9000.0);
        std::println("surface {:.1f} K, snowline {:.0f} m, balance at peak {:.2f} m/yr",
                     here.surface, line.value_or(-1.0),
                     hog::weather::ice_balance(planet, here, highest - planet.body.radius).net());
    }

    hog::globe::MadeWorld made{generator};
    World& world = made.world();
    made.cover(peak, 5);
    hog::flow::Simulation simulation{world};
    const hog::server::IceMap map = hog::server::bake_ice_map(
        generator, {.level = centre.level, .face = centre.face, .i = i0, .j = j0}, side / 8, 8,
        [&](const Cell& column) {
            const auto here = generator.column(direction_of(column.i, column.j), 1.0);
            return hog::weather::Climate{.surface = reference_temperature(generator, here),
                                         .swing = 12.0,
                                         .precipitation = 0.15};
        },
        300);
    simulation.lay_ice(hog::server::laid_from(map));

    // Top-down: height shading, ice tinted blue by its depth.
    std::vector<std::uint8_t> pixels(std::size_t{side} * side * 3);
    double most_ice = 0.0;
    double total = 0.0;
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            double ice = 0.0;
            const std::uint32_t base =
                static_cast<std::uint32_t>(std::max(layers.layer_at(ground[at(x, y)]) - 4, 0));
            for (std::uint32_t layer = base; layer < base + 140; ++layer) {
                const Site site{.face = centre.face, .i = i0 + x, .j = j0 + y, .layer = layer};
                if (!hog::globe::in_layers(layers, brick_of(site))) {
                    continue;
                }
                const auto voxel = world.at(site);
                if (voxel.grain == Material::Ice) {
                    ice += static_cast<double>(voxel.grain_amount) / Voxel::full * layers.thickness;
                }
            }
            most_ice = std::max(most_ice, ice);
            total += ice;
            const double shade =
                std::clamp(0.4 + (((ground[at(x, y)] - highest) / 2500.0) + 0.6), 0.1, 1.0);
            const double t = std::clamp(ice / 40.0, 0.0, 1.0);
            const std::size_t p = at(x, y) * 3;
            pixels[p] = static_cast<std::uint8_t>(255 * shade * (1.0 - (0.6 * t)));
            pixels[p + 1] = static_cast<std::uint8_t>(255 * shade * (1.0 - (0.3 * t)));
            pixels[p + 2] = static_cast<std::uint8_t>(255 * shade);
        }
    }
    std::println("ice: most {:.1f} m, mean {:.2f} m", most_ice,
                 total / (static_cast<double>(side) * side));
    const std::string path = std::string{dir} + "/glacier.png";
    std::vector<unsigned char> encoded;
    stbi_write_png_to_func(
        [](void* sink, void* data, int size) {
            auto* out = static_cast<std::vector<unsigned char>*>(sink);
            const std::span bytes{static_cast<unsigned char*>(data),
                                  static_cast<std::size_t>(size)};
            out->insert(out->end(), bytes.begin(), bytes.end());
        },
        &encoded, static_cast<int>(side), static_cast<int>(side), 3, pixels.data(),
        static_cast<int>(side) * 3);
    std::ofstream out{path, std::ios::binary};
    for (const unsigned char byte : encoded) {
        out.put(static_cast<char>(byte));
    }
    CHECK(most_ice >= 0.0);
}

} // namespace

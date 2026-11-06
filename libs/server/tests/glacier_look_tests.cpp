#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/flow/simulation.hpp>
#include <hog/server/ice_climate.hpp>
#include <hog/weather/field.hpp>

#include <stb_image_write.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <set>
#include <vector>

namespace {

using hog::DVec3;
using namespace hog::globe;

// The air's temperature over a column, brought down to the reference level at the lapse rate.
double reference_temperature(const Generator& generator, const Generator::Column& column) {
    const Planet& planet = generator.planet();
    const double height = std::max(column.ground(), planet.radius) + 20.0;
    return column.sample(height).temperature +
           (hog::weather::lapse_rate(planet) * (height - planet.radius));
}

// Bakes ice onto the highest mountain of the default planet and writes a top-down picture of it
// to $HOG_LOOK_DIR. Hidden: it takes a while and is for looking at.
TEST_CASE("a mountain's snow settles into glaciers", "[.glacier_look]") {
    const char* dir = std::getenv("HOG_LOOK_DIR");
    REQUIRE(dir != nullptr);
    const Generator generator{Planet{}};
    const Layers& layers = generator.layers();
    const Planet& planet = generator.planet();

    // The highest ground over a sphere of directions.
    DVec3 peak{0.0, 0.0, 1.0};
    double highest = 0.0;
    constexpr int points = 40000;
    for (int k = 0; k < points; ++k) {
        const double z = 1.0 - ((2.0 * k + 1.0) / points);
        const double r = std::sqrt(1.0 - z * z);
        const double a = 2.399963 * k;
        const DVec3 d{r * std::cos(a), r * std::sin(a), z};
        const double ground = generator.column(d, 200.0).ground();
        if (ground > highest) {
            highest = ground;
            peak = d;
        }
    }
    std::printf("peak %.0f m over the reference, direction %.3f %.3f %.3f\n",
                highest - planet.radius, peak.x, peak.y, peak.z);

    constexpr std::uint32_t side = 128;
    const auto at = [](std::uint32_t x, std::uint32_t y) { return static_cast<std::size_t>((y * side) + x); };
    const Cell centre = cell_under(peak, layers.level);
    const std::uint32_t i0 = centre.i - (side / 2);
    const std::uint32_t j0 = centre.j - (side / 2);
    const auto direction_of = [&](std::uint32_t i, std::uint32_t j) {
        return glm::normalize(middle_of({centre.face, i, j, 0}, layers));
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
            for (auto layer = static_cast<std::uint32_t>(low); layer <= static_cast<std::uint32_t>(high); layer += brick_side) {
                bricks.insert(brick_of({centre.face, i0 + x, j0 + y, layer}));
            }
            bricks.insert(brick_of({centre.face, i0 + x, j0 + y, static_cast<std::uint32_t>(high)}));
        }
    }
    std::printf("%zu bricks\n", bricks.size());
    {
        const hog::weather::Climate here{
            .surface = reference_temperature(generator, generator.column(peak, 1.0)),
            .swing = 12.0,
            .precipitation = 0.15};
        const auto line = hog::weather::snowline(planet, here, 9000.0);
        std::printf("surface %.1f K, snowline %.0f m, balance at peak %.2f m/yr\n", here.surface,
                    line.value_or(-1.0),
                    hog::weather::ice_balance(planet, here, highest - planet.radius).net());
    }

    VoxelStore store{layers};
    World world{generator, store};
    hog::flow::Simulation simulation{world};
    simulation.set_ice_balance(hog::server::ice_balance(planet, [&](const Cell& column) {
        const auto here = generator.column(direction_of(column.i, column.j), 1.0);
        return hog::weather::Climate{
            .surface = reference_temperature(generator, here), .swing = 12.0, .precipitation = 0.15};
    }));
    const std::vector<BrickSite> list(bricks.begin(), bricks.end());
    const int rounds = std::getenv("HOG_LOOK_ROUNDS") != nullptr ? std::atoi(std::getenv("HOG_LOOK_ROUNDS")) : 4;
    for (int round = 0; round < rounds; ++round) {
        simulation.age_ice(list, 5.0);
    }

    // Top-down: height shading, ice tinted blue by its depth.
    std::vector<std::uint8_t> pixels(std::size_t{side} * side * 3);
    double most_ice = 0.0;
    double total = 0.0;
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            double ice = 0.0;
            const std::uint32_t base = static_cast<std::uint32_t>(std::max(layers.layer_at(ground[at(x, y)]) - 4, 0));
            for (std::uint32_t layer = base; layer < base + 140; ++layer) {
                const Site site{centre.face, i0 + x, j0 + y, layer};
                if (!store.holds_site(brick_of(site)) || store.brick(brick_of(site)) == nullptr) {
                    continue;
                }
                const auto voxel = world.at(site);
                if (voxel.grain == Material::Ice) {
                    ice += static_cast<double>(voxel.grain_amount) / Voxel::full * layers.thickness;
                }
            }
            most_ice = std::max(most_ice, ice);
            total += ice;
            const double shade = std::clamp(0.4 + ((ground[at(x, y)] - highest) / 2500.0 + 0.6), 0.1, 1.0);
            const double t = std::clamp(ice / 40.0, 0.0, 1.0);
            const std::size_t p = at(x, y) * 3;
            pixels[p] = static_cast<std::uint8_t>(255 * shade * (1.0 - (0.6 * t)));
            pixels[p + 1] = static_cast<std::uint8_t>(255 * shade * (1.0 - (0.3 * t)));
            pixels[p + 2] = static_cast<std::uint8_t>(255 * shade);
        }
    }
    std::printf("ice: most %.1f m, mean %.2f m\n", most_ice, total / (static_cast<double>(side) * side));
    const std::string path = std::string{dir} + "/glacier.png";
    std::vector<unsigned char> encoded;
    stbi_write_png_to_func(
        [](void* sink, void* data, int size) {
            auto* out = static_cast<std::vector<unsigned char>*>(sink);
            const auto* bytes = static_cast<unsigned char*>(data);
            out->insert(out->end(), bytes, bytes + size);
        },
        &encoded, static_cast<int>(side), static_cast<int>(side), 3, pixels.data(),
        static_cast<int>(side) * 3);
    std::ofstream{path, std::ios::binary}.write(reinterpret_cast<const char*>(encoded.data()),
                                                static_cast<std::streamsize>(encoded.size()));
    CHECK(most_ice >= 0.0);
}

} // namespace

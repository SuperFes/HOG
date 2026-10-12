#include <hog/mapgen/weather_render.hpp>
#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/viewer/overlay.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace hog;
using hog::mapgen::Rgb;
using hog::viewer::Frame;
using hog::viewer::Overlay;

namespace {

constexpr int level = 2;
constexpr int width = 64;

struct Planet {
    sim::CellGrid grid{level};
    std::vector<probe::Layer> layers;

    void set(const std::string& name, float value) {
        std::erase_if(layers, [&name](const probe::Layer& layer) { return layer.name == name; });
        layers.push_back(
            probe::Layer{.name = name, .values = std::vector<float>(grid.size(), value)});
    }
    std::vector<float>& values(const std::string& name) {
        return std::ranges::find(layers, name, &probe::Layer::name)->values;
    }

    [[nodiscard]] Frame frame() const {
        return Frame{probe::Welcome{}, probe::Layers{.time = {}, .layers = layers}, grid.size()};
    }

    [[nodiscard]] mapgen::Image render(Overlay overlay, const std::string& species = "") const {
        jobs::JobSystem jobs{2};
        return viewer::render_overlay(grid, frame(), overlay, species, width, jobs);
    }

    // The cell under each pixel, row by row.
    [[nodiscard]] std::vector<std::uint32_t> cells() const {
        const mapgen::ImageSize size{.width = width, .height = width / 2};
        std::vector<std::uint32_t> cells;
        for (int y = 0; y < size.height; ++y) {
            for (int x = 0; x < size.width; ++x) {
                cells.push_back(grid.index(planet::cell_at(
                    mapgen::pixel_direction(mapgen::PlanetView{}, size, x, y, 1.0), level)));
            }
        }
        return cells;
    }
};

// Land 500 m up everywhere.
Planet land() {
    Planet planet;
    planet.set("planet/ocean", 0.0F);
    planet.set("planet/elevation", 500.0F);
    planet.set("planet/area", 100.0F);
    return planet;
}

std::vector<Rgb> pixels(const mapgen::Image& image) {
    std::vector<Rgb> out;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            out.push_back(image.at(x, y));
        }
    }
    return out;
}

bool all(const mapgen::Image& image, Rgb colour) {
    return std::ranges::all_of(pixels(image), [colour](Rgb pixel) { return pixel == colour; });
}

std::int64_t fixed(double value) {
    return std::llround(value * planet::fixed_one);
}

} // namespace

TEST_CASE("an overlay covers the planet twice as wide as tall", "[viewer][overlay]") {
    const mapgen::Image image = land().render(Overlay::Terrain);
    CHECK(image.width == width);
    CHECK(image.height == width / 2);
    CHECK(all(image, mapgen::height_colour(fixed(500.0))));
}

TEST_CASE("overlays without their layers show as missing", "[viewer][overlay]") {
    const Planet planet;
    for (const Overlay overlay : viewer::overlays) {
        INFO(viewer::to_string(overlay));
        CHECK(all(planet.render(overlay, "deer"), viewer::missing_colour));
    }
}

TEST_CASE("weather overlays colour by their fields", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("weather/temperature", 30.0F);
    planet.set("weather/front", 0.0F);
    planet.set("weather/pressure", 1'013.0F);
    CHECK(all(planet.render(Overlay::Temperature), mapgen::temperature_colour(fixed(30.0))));
    CHECK(all(planet.render(Overlay::Air), mapgen::air_colour(30.0, 0.0)));
    CHECK(all(planet.render(Overlay::Pressure), viewer::pressure_colour(1'013.0)));
    CHECK(all(planet.render(Overlay::Sky), viewer::missing_colour));

    for (const char* name :
         {"weather/cloud", "weather/rain", "weather/snow", "weather/hail", "weather/storm"}) {
        planet.set(name, 0.0F);
    }
    CHECK(all(planet.render(Overlay::Sky),
              mapgen::sky_colour(mapgen::height_colour(fixed(500.0)), 0.0, {}, 0.0)));
}

TEST_CASE("biomes are shown cell by cell", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("planet/biome", static_cast<float>(planet::Biome::Desert));
    planet.values("planet/biome")[7] = static_cast<float>(planet::Biome::Tundra);
    const auto cells = planet.cells();
    const auto shown = pixels(planet.render(Overlay::Biome));
    for (std::size_t pixel = 0; pixel < shown.size(); ++pixel) {
        CHECK(shown[pixel] == mapgen::biome_colour(cells[pixel] == 7 ? planet::Biome::Tundra
                                                                     : planet::Biome::Desert));
    }
    CHECK(std::ranges::count(cells, 7U) > 0);
}

TEST_CASE("plants show by how grown they are, and fires on top", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("forest/capacity", 20.0F);
    planet.set("forest/biomass", 5.0F);
    planet.set("forest/fire", 0.0F);
    planet.values("forest/fire")[11] = 0.6F;
    CHECK(all(planet.render(Overlay::Plants), viewer::plant_colour(0.25)));

    const auto cells = planet.cells();
    const auto shown = pixels(planet.render(Overlay::Fire));
    const Rgb plants = viewer::plant_colour(0.25);
    for (std::size_t pixel = 0; pixel < shown.size(); ++pixel) {
        CHECK(shown[pixel] == (cells[pixel] == 11 ? viewer::fire_colour(plants, 0.6) : plants));
    }

    planet.set("planet/ocean", 1.0F);
    CHECK_FALSE(all(planet.render(Overlay::Plants), viewer::plant_colour(0.25)));
}

TEST_CASE("wildlife is shown against its densest cell", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("wildlife/deer", 10.0F);
    planet.values("wildlife/deer")[3] = 40.0F;
    const auto cells = planet.cells();
    const auto shown = pixels(planet.render(Overlay::Wildlife, "deer"));
    const Rgb ground = mapgen::height_colour(fixed(500.0));
    for (std::size_t pixel = 0; pixel < shown.size(); ++pixel) {
        CHECK(shown[pixel] == mapgen::density_colour(ground, cells[pixel] == 3 ? 1.0 : 0.25));
    }
    CHECK(all(planet.render(Overlay::Wildlife, "elk"), viewer::missing_colour));
}

TEST_CASE("each overlay asks for the layers it draws", "[viewer][overlay]") {
    const auto has = [](const std::vector<std::string>& layers, const std::string& name) {
        return std::ranges::find(layers, name) != layers.end();
    };
    CHECK(has(viewer::layers_for(Overlay::Sky, ""), "weather/storm"));
    CHECK(has(viewer::layers_for(Overlay::Fire, ""), "forest/fire"));
    CHECK(has(viewer::layers_for(Overlay::Wildlife, "red fox"), "wildlife/red fox"));
    CHECK(has(viewer::layers_for(Overlay::Wildlife, "red fox"), "planet/area"));
    CHECK_FALSE(has(viewer::layers_for(Overlay::Terrain, ""), "weather/temperature"));
    for (const Overlay overlay : viewer::overlays) {
        CHECK(has(viewer::layers_for(overlay, "deer"), "planet/elevation"));
    }
}

TEST_CASE("colour ramps run as described", "[viewer][overlay]") {
    CHECK(viewer::pressure_colour(1'013.0) == Rgb{245, 245, 245});
    CHECK(viewer::pressure_colour(900.0) == viewer::pressure_colour(970.0));
    CHECK(viewer::plant_colour(1.0) == Rgb{25, 95, 40});
    CHECK(viewer::fire_colour(Rgb{0, 0, 0}, 0.0) == Rgb{0, 0, 0});
    CHECK(viewer::fire_colour(Rgb{0, 0, 0}, 1.0) == Rgb{255, 80, 10});
}

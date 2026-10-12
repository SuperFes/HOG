#include <hog/mapgen/weather_render.hpp>
#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/viewer/ground.hpp>
#include <hog/viewer/overlay.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
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
        return Frame{probe::Welcome{}, probe::Layers{.time = {}, .clock = {}, .layers = layers},
                     grid.size()};
    }

    [[nodiscard]] mapgen::Image render(Overlay overlay, const std::string& species = "",
                                       const viewer::MapRect& shown = {},
                                       const probe::Ground* ground = nullptr) const {
        jobs::JobSystem jobs{2};
        return viewer::render_overlay(grid, frame(), ground, overlay, species, shown, width, jobs);
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

TEST_CASE("a zoomed overlay draws only the shown part, at full size", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("planet/biome", static_cast<float>(planet::Biome::Desert));
    planet.values("planet/biome")[7] = static_cast<float>(planet::Biome::Tundra);
    const Rgb tundra = mapgen::biome_colour(planet::Biome::Tundra);
    const auto whole = pixels(planet.render(Overlay::Biome));
    const auto first = static_cast<int>(std::ranges::find(whole, tundra) - whole.begin());
    REQUIRE(first < static_cast<int>(whole.size()));

    viewer::MapView view;
    view.zoom_by(4.0, viewer::pixel_point(viewer::MapRect{}, width, width / 2, first % width,
                                          first / width));
    const mapgen::Image image = planet.render(Overlay::Biome, "", view.shown());
    CHECK(image.width == width);
    CHECK(image.height == width / 2);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const DVec3 direction = viewer::map_direction(
                viewer::pixel_point(view.shown(), image.width, image.height, x, y));
            const bool in_tundra = planet.grid.index(planet::cell_at(direction, level)) == 7;
            CHECK(image.at(x, y) ==
                  (in_tundra ? tundra : mapgen::biome_colour(planet::Biome::Desert)));
        }
    }
    // The cell spans more pixels than it did on the whole map.
    CHECK(std::ranges::count(pixels(image), tundra) > std::ranges::count(whole, tundra));
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
    const Rgb ground = mapgen::height_colour(fixed(500.0));
    CHECK(all(planet.render(Overlay::Wildlife, "deer"), mapgen::density_colour(ground, 1.0)));

    // Smoothed between cells, so only those beside the dense one see it.
    planet.values("wildlife/deer")[3] = 40.0F;
    const auto& beside = planet.grid.neighbors(3);
    const auto cells = planet.cells();
    const auto shown = pixels(planet.render(Overlay::Wildlife, "deer"));
    for (std::size_t pixel = 0; pixel < shown.size(); ++pixel) {
        if (cells[pixel] != 3 && std::ranges::find(beside, cells[pixel]) == beside.end()) {
            CHECK(shown[pixel] == mapgen::density_colour(ground, 0.25));
        }
    }
    CHECK(std::ranges::count(shown, mapgen::density_colour(ground, 0.25)) <
          static_cast<std::ptrdiff_t>(shown.size()));
    CHECK(all(planet.render(Overlay::Wildlife, "elk"), viewer::missing_colour));
}

namespace {

// Ground surveyed under every pixel of a shown area: the west half sea 30 m deep and the east half
// land 800 m up, tundra over sandstone with gold in the far east.
probe::Ground surveyed(const viewer::MapRect& shown) {
    const int height = width / 2;
    probe::Ground ground{.survey = viewer::survey_of(shown, width, height, true, 40.0F),
                         .elevation = {},
                         .biome = {},
                         .rock = {},
                         .deposit = {}};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool east = x >= width / 2;
            ground.elevation.push_back(east ? 800.0F : -30.0F);
            ground.biome.push_back(
                std::to_underlying(east ? planet::Biome::Tundra : planet::Biome::Ocean));
            ground.rock.push_back(std::to_underlying(planet::Rock::Sandstone));
            ground.deposit.push_back(x >= width - 4 ? std::to_underlying(planet::Resource::Gold) + 1
                                                    : 0);
        }
    }
    return ground;
}

} // namespace

TEST_CASE("surveyed ground draws terrain, coasts and biomes finely", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("planet/biome", static_cast<float>(planet::Biome::Desert));
    planet.set("forest/biomass", 1.0F);
    planet.set("forest/capacity", 4.0F);
    const viewer::MapRect shown{.low = {0.25, 0.25}, .high = {0.75, 0.5}};
    const probe::Ground ground = surveyed(shown);

    const mapgen::Image terrain = planet.render(Overlay::Terrain, "", shown, &ground);
    const mapgen::Image biomes = planet.render(Overlay::Biome, "", shown, &ground);
    const mapgen::Image plants = planet.render(Overlay::Plants, "", shown, &ground);
    for (int y = 0; y < terrain.height; ++y) {
        for (int x = 0; x < terrain.width; ++x) {
            const bool east = x >= width / 2;
            CHECK(terrain.at(x, y) == mapgen::height_colour(fixed(east ? 800.0 : -30.0)));
            CHECK(biomes.at(x, y) ==
                  mapgen::biome_colour(east ? planet::Biome::Tundra : planet::Biome::Ocean));
        }
    }
    // Overlays of the simulation keep its values, inside the surveyed coasts.
    CHECK(plants.at(0, 0) == Rgb{40, 90, 150});
    CHECK(plants.at(width - 1, 0) == viewer::plant_colour(0.25));

    // Off the surveyed part, the cells show as before.
    const mapgen::Image whole = planet.render(Overlay::Terrain, "", {}, &ground);
    CHECK(whole.at(0, 0) == mapgen::height_colour(fixed(500.0)));
}

TEST_CASE("minerals show the rock or rarest deposit at the surveyed depth", "[viewer][overlay]") {
    const Planet planet = land();
    CHECK(all(planet.render(Overlay::Minerals), viewer::missing_colour));
    const viewer::MapRect shown{.low = {0.25, 0.25}, .high = {0.75, 0.5}};
    probe::Ground ground = surveyed(shown);
    const mapgen::Image image = planet.render(Overlay::Minerals, "", shown, &ground);
    const Rgb sandstone = mapgen::rock_colour(planet::Rock::Sandstone);
    CHECK(image.at(width / 2, 0) == sandstone);
    CHECK(image.at(width - 1, 0) == mapgen::resource_colour(planet::Resource::Gold));
    CHECK(image.at(0, 0) == mapgen::mix(sandstone, Rgb{40, 90, 150}, 0.4));

    // Ground surveyed only at the surface has nothing to show below it.
    ground.rock.clear();
    ground.deposit.clear();
    CHECK(all(planet.render(Overlay::Minerals, "", shown, &ground), viewer::missing_colour));
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

TEST_CASE("a legend gives the range of what an overlay shows", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("weather/temperature", 10.0F);
    planet.values("weather/temperature")[2] = -5.0F;
    planet.values("weather/temperature")[4] = 25.0F;
    const probe::Welcome welcome{
        .version = probe::protocol_version,
        .world_id = 1,
        .level = level,
        .planet_radius = 1.0,
        .layers = {probe::LayerInfo{.name = "weather/temperature", .unit = "°C"}}};
    const Frame frame{welcome, probe::Layers{.time = {}, .clock = {}, .layers = planet.layers},
                      planet.grid.size()};
    const auto legend = viewer::legend_for(Overlay::Air, frame, "");
    REQUIRE(legend);
    const viewer::Legend air = legend.value_or(viewer::Legend{});
    CHECK(air.layer == "weather/temperature");
    CHECK(air.unit == "°C");
    CHECK(air.low == -5.0);
    CHECK(air.high == 25.0);

    CHECK_FALSE(viewer::legend_for(Overlay::Biome, frame, ""));
    CHECK_FALSE(viewer::legend_for(Overlay::Pressure, frame, ""));
    const auto terrain = viewer::legend_for(Overlay::Terrain, frame, "");
    REQUIRE(terrain);
    CHECK(terrain.value_or(viewer::Legend{.layer = "", .unit = "?", .low = 0.0, .high = 0.0})
              .unit.empty());
}

TEST_CASE("overlays are found by name", "[viewer][overlay]") {
    for (const Overlay overlay : viewer::overlays) {
        CHECK(viewer::overlay_named(viewer::to_string(overlay)) == overlay);
    }
    CHECK_FALSE(viewer::overlay_named("rainbows"));
}

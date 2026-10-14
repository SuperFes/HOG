#include <hog/mapgen/weather_render.hpp>
#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/natural.hpp>
#include <hog/viewer/ground.hpp>
#include <hog/viewer/overlay.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
                                       const probe::Ground* ground = nullptr,
                                       bool natural = false) const {
        jobs::JobSystem jobs{2};
        return viewer::render_overlay(grid, frame(), ground, overlay, species, shown, width,
                                      natural, jobs);
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

TEST_CASE("the planet's faces are drawn a pixel per cell, stacked", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("planet/biome", static_cast<float>(planet::Biome::Desert));
    planet.values("planet/biome")[7] = static_cast<float>(planet::Biome::Tundra);
    constexpr int across = 1 << level;
    jobs::JobSystem jobs{2};
    const mapgen::Image faces = viewer::render_faces(planet.grid, planet.frame(), nullptr,
                                                     Overlay::Biome, "", across, false, jobs);
    CHECK(faces.width == across);
    CHECK(faces.height == planet::face_count * across);
    int tundra = 0;
    for (int face = 0; face < planet::face_count; ++face) {
        for (int j = 0; j < across; ++j) {
            for (int i = 0; i < across; ++i) {
                const std::uint32_t cell = planet.grid.index(planet::CellId::from_face_ij(
                    face, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j), level));
                tundra += cell == 7 ? 1 : 0;
                CHECK(faces.at(i, (face * across) + j) ==
                      mapgen::biome_colour(cell == 7 ? planet::Biome::Tundra
                                                     : planet::Biome::Desert));
            }
        }
    }
    CHECK(tundra == 1);
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

    // Nothing lives at sea, whatever smoothing carries over the coast.
    planet.set("planet/ocean", 1.0F);
    CHECK(all(planet.render(Overlay::Wildlife, "deer"), mapgen::height_colour(fixed(-200.0))));
}

namespace {

// The cave and its decimetres of room along the top rows; none below.
constexpr std::array<std::pair<std::uint8_t, std::uint8_t>, 3> row_caves{{
    {std::to_underlying(planet::Cave::LavaTube) + 1, 30},
    {std::to_underlying(planet::Cave::Karst) + 1, 2},
    {0, 0},
}};

void add_sample(probe::Ground& ground, int x, int y) {
    const bool east = x >= width / 2;
    ground.elevation.push_back(east ? 800.0F : -30.0F);
    ground.biome.push_back(std::to_underlying(east ? planet::Biome::Tundra : planet::Biome::Ocean));
    ground.water_table.push_back(east ? 6.0F : 0.0F);
    ground.river.push_back(0);
    ground.rock.push_back(std::to_underlying(planet::Rock::Sandstone));
    ground.deposit.push_back(x >= width - 4 ? std::to_underlying(planet::Resource::Gold) + 1 : 0);
    ground.heat.push_back(east ? 650.0F : 20.0F);
    const auto& [cave, room] = row_caves.at(static_cast<std::size_t>(std::min(y, 2)));
    ground.cave.push_back(cave);
    ground.room.push_back(room);
}

// Ground surveyed under every pixel of a shown area: the west half sea 30 m deep and the east half
// land 800 m up, tundra over sandstone with gold in the far east, hot under the land, with a
// roomy lava tube along the top row and a karst squeeze along the next.
probe::Ground surveyed(const viewer::MapRect& shown) {
    const int height = width / 2;
    probe::Ground ground{.survey = viewer::survey_of(shown, width, height, true, 40.0F),
                         .elevation = {},
                         .biome = {},
                         .water_table = {},
                         .river = {},
                         .rock = {},
                         .deposit = {},
                         .heat = {},
                         .cave = {},
                         .room = {}};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            add_sample(ground, x, y);
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

TEST_CASE("lakes and rivers show over terrain and biomes", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("planet/biome", static_cast<float>(planet::Biome::Desert));
    const viewer::MapRect shown{.low = {0.25, 0.25}, .high = {0.75, 0.5}};
    probe::Ground ground = surveyed(shown);
    const auto sample = [](int x, int y) {
        return (static_cast<std::size_t>(y) * width) + static_cast<std::size_t>(x);
    };
    ground.river[sample(3 * width / 4, 2)] = 30;
    ground.river[sample(1, 2)] = 30;
    ground.biome[sample(width - 2, 4)] = std::to_underlying(planet::Biome::Lake);

    for (const Overlay overlay : {Overlay::Terrain, Overlay::Biome}) {
        const mapgen::Image image = planet.render(overlay, "", shown, &ground);
        CHECK(image.at(3 * width / 4, 2) == mapgen::river_colour(30.0));
        CHECK(image.at(width - 2, 4) == mapgen::biome_colour(planet::Biome::Lake));
        // Rivers run into the sea, which stays the sea's colour.
        CHECK(image.at(1, 2) == planet.render(overlay, "", shown, &ground).at(0, 2));
    }
    CHECK(planet.render(Overlay::Terrain, "", shown, &ground).at(width - 1, 2) ==
          mapgen::height_colour(fixed(800.0)));

    // Off the surveyed part, lakes come from the cells' biomes.
    planet.values("planet/biome")[5] = static_cast<float>(planet::Biome::Lake);
    const auto cells = planet.cells();
    const auto terrain = pixels(planet.render(Overlay::Terrain));
    std::size_t lakes = 0;
    for (std::size_t pixel = 0; pixel < cells.size(); ++pixel) {
        const bool lake = cells[pixel] == 5;
        lakes += lake ? 1U : 0U;
        CHECK(terrain[pixel] == (lake ? mapgen::biome_colour(planet::Biome::Lake)
                                      : mapgen::height_colour(fixed(500.0))));
    }
    CHECK(lakes > 0);
}

TEST_CASE("the water overlay shows how far down the water table is", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("planet/biome", static_cast<float>(planet::Biome::Grassland));
    planet.set("planet/water_table", 3.0F);
    CHECK(all(planet.render(Overlay::Water), mapgen::water_table_colour(fixed(3.0))));

    const viewer::MapRect shown{.low = {0.25, 0.25}, .high = {0.75, 0.5}};
    probe::Ground ground = surveyed(shown);
    ground.river[(2 * width) + (3 * width / 4)] = 30;
    const mapgen::Image image = planet.render(Overlay::Water, "", shown, &ground);
    CHECK(image.at(0, 0) == mapgen::height_colour(fixed(-30.0)));
    CHECK(image.at(width - 1, 0) == mapgen::water_table_colour(fixed(6.0)));
    CHECK(image.at(3 * width / 4, 2) == mapgen::river_colour(30.0));

    const Frame frame = planet.frame();
    const auto legend = viewer::legend_for(Overlay::Water, frame, "");
    REQUIRE(legend);
    CHECK(legend.value_or(viewer::Legend{}).layer == "planet/water_table");
    CHECK(legend.value_or(viewer::Legend{}).high == 3.0);
}

TEST_CASE("the water table rises and falls with the land's water", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("planet/biome", static_cast<float>(planet::Biome::Grassland));
    planet.set("planet/water_table", 3.0F);
    planet.set("land/water_table_rise", 1.0F);
    CHECK(all(planet.render(Overlay::Water), mapgen::water_table_colour(fixed(2.0))));
    // Risen to the surface, and no further.
    planet.set("land/water_table_rise", 5.0F);
    CHECK(all(planet.render(Overlay::Water), mapgen::water_table_colour(0)));
    planet.set("land/water_table_rise", -2.0F);
    CHECK(all(planet.render(Overlay::Water), mapgen::water_table_colour(fixed(5.0))));
}

TEST_CASE("the soil overlay shows how wet the ground is, white under snow", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("land/soil", 0.6F);
    CHECK(all(planet.render(Overlay::Soil), viewer::missing_colour));
    planet.set("land/snow", 0.0F);
    const Rgb damp = mapgen::moisture_colour(fixed(0.6));
    CHECK(all(planet.render(Overlay::Soil), damp));
    planet.set("land/snow", 20.0F);
    CHECK(all(planet.render(Overlay::Soil), viewer::snow_colour(damp, 20.0)));

    CHECK(viewer::snow_colour(damp, 0.0) == damp);
    const Rgb deep = viewer::snow_colour(damp, 1e6);
    CHECK(deep[0] > 240);
    CHECK(deep[2] > 245);
    const Rgb light = viewer::snow_colour(damp, 5.0);
    CHECK(light[0] > damp[0]);
    CHECK(light[0] < deep[0]);

    const auto legend = viewer::legend_for(Overlay::Soil, planet.frame(), "");
    CHECK(legend.value_or(viewer::Legend{}).layer == "land/soil");
}

TEST_CASE("minerals show the rock or rarest deposit at the surveyed depth", "[viewer][overlay]") {
    const Planet planet = land();
    CHECK(all(planet.render(Overlay::Minerals), viewer::missing_colour));
    const viewer::MapRect shown{.low = {0.25, 0.25}, .high = {0.75, 0.5}};
    probe::Ground ground = surveyed(shown);
    const mapgen::Image image = planet.render(Overlay::Minerals, "", shown, &ground);
    const Rgb sandstone = mapgen::rock_colour(planet::Rock::Sandstone);
    CHECK(image.at(width / 2, 2) == sandstone);
    CHECK(image.at(width - 1, 2) == mapgen::resource_colour(planet::Resource::Gold));
    CHECK(image.at(0, 2) == mapgen::mix(sandstone, Rgb{40, 90, 150}, 0.4));
    // Caves show over the rock and deposits; a squeeze lets the rock show through.
    CHECK(image.at(width - 1, 0) == mapgen::cave_colour(planet::Cave::LavaTube));
    CHECK(image.at(width / 2, 1) ==
          mapgen::mix(mapgen::cave_colour(planet::Cave::Karst), sandstone, 0.6));

    // Ground surveyed only at the surface has nothing to show below it.
    ground.rock.clear();
    ground.deposit.clear();
    CHECK(all(planet.render(Overlay::Minerals, "", shown, &ground), viewer::missing_colour));
}

TEST_CASE("heat shows the rock's temperature at the surveyed depth", "[viewer][overlay]") {
    const Planet planet = land();
    CHECK(all(planet.render(Overlay::Heat), viewer::missing_colour));
    const viewer::MapRect shown{.low = {0.25, 0.25}, .high = {0.75, 0.5}};
    probe::Ground ground = surveyed(shown);
    const mapgen::Image image = planet.render(Overlay::Heat, "", shown, &ground);
    CHECK(image.at(width - 1, 0) == mapgen::heat_colour(650 * planet::fixed_one));
    CHECK(image.at(0, 0) ==
          mapgen::mix(mapgen::heat_colour(20 * planet::fixed_one), Rgb{40, 90, 150}, 0.4));

    ground.heat.clear();
    CHECK(all(planet.render(Overlay::Heat, "", shown, &ground), viewer::missing_colour));
}

TEST_CASE("only the minerals and heat overlays look underground", "[viewer][overlay]") {
    for (const Overlay overlay : viewer::overlays) {
        CHECK(viewer::underground(overlay) ==
              (overlay == Overlay::Minerals || overlay == Overlay::Heat));
    }
}

TEST_CASE("natural edges wander, keeping each cell's value", "[viewer][overlay]") {
    Planet planet = land();
    planet.set("planet/biome", static_cast<float>(planet::Biome::Desert));
    planet.values("planet/biome")[7] = static_cast<float>(planet::Biome::Tundra);
    const auto plain = pixels(planet.render(Overlay::Biome));
    const auto natural = pixels(planet.render(Overlay::Biome, "", {}, nullptr, true));
    const Rgb tundra = mapgen::biome_colour(planet::Biome::Tundra);
    const double reach = sim::natural_reach(level);
    std::size_t moved = 0;
    for (int y = 0; y < width / 2; ++y) {
        for (int x = 0; x < width; ++x) {
            const DVec3 direction =
                viewer::map_direction(viewer::pixel_point({}, width, width / 2, x, y));
            // The welcome's world id seeds the wander.
            const DVec3 cells = sim::natural_point(direction, reach, 0);
            const bool in_tundra = planet.grid.index(planet::cell_at(cells, level)) == 7;
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * width) + static_cast<std::size_t>(x);
            CHECK(natural[pixel] ==
                  (in_tundra ? tundra : mapgen::biome_colour(planet::Biome::Desert)));
            moved += natural[pixel] != plain[pixel] ? 1U : 0U;
        }
    }
    CHECK(moved > 0);
}

namespace {

// Sea 20 °C everywhere, running at `east` and `north` m/s.
Planet sea(float east, float north) {
    Planet planet;
    planet.set("planet/ocean", 1.0F);
    planet.set("planet/elevation", 0.0F);
    planet.set("ocean/temperature", 20.0F);
    planet.set("ocean/current_east", east);
    planet.set("ocean/current_north", north);
    return planet;
}

// How far the arrow in the image's first square reaches across and down, in pixels.
std::pair<int, int> arrow_extent(const mapgen::Image& image, Rgb sea) {
    const int spacing = viewer::arrow_spacing(width);
    int left = spacing;
    int right = -1;
    int top = spacing;
    int bottom = -1;
    for (int y = 0; y < spacing; ++y) {
        for (int x = 0; x < spacing; ++x) {
            if (image.at(x, y) != sea) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    return {right - left + 1, bottom - top + 1};
}

} // namespace

TEST_CASE("the sea overlay shows its warmth, with arrows along its currents", "[viewer][overlay]") {
    const Rgb warm = mapgen::temperature_colour(fixed(20.0));
    CHECK(all(sea(0.0F, 0.0F).render(Overlay::Sea), warm));

    const mapgen::Image east = sea(0.4F, 0.0F).render(Overlay::Sea);
    const auto [east_wide, east_tall] = arrow_extent(east, warm);
    CHECK(east_wide > east_tall);
    // Every pixel is the sea's or leans towards the arrows'.
    for (const Rgb pixel : pixels(east)) {
        REQUIRE((pixel == warm || pixel[2] < warm[2] || pixel[0] < warm[0]));
    }
    const auto [north_wide, north_tall] = arrow_extent(sea(0.0F, 0.4F).render(Overlay::Sea), warm);
    CHECK(north_tall > north_wide);
    // Faster currents draw longer arrows.
    CHECK(arrow_extent(sea(0.05F, 0.0F).render(Overlay::Sea), warm).first < east_wide);

    // Land shows as itself, without arrows.
    Planet planet = land();
    planet.set("ocean/temperature", 20.0F);
    planet.set("ocean/current_east", 0.4F);
    planet.set("ocean/current_north", 0.0F);
    CHECK(all(planet.render(Overlay::Sea), mapgen::height_colour(fixed(500.0))));
}

TEST_CASE("the sea's legend ranges over the sea alone", "[viewer][overlay]") {
    Planet planet = sea(0.0F, 0.0F);
    planet.values("planet/ocean")[0] = 0.0F;
    planet.values("ocean/temperature")[0] = -30.0F;
    planet.values("ocean/temperature")[1] = 4.0F;
    const auto legend = viewer::legend_for(Overlay::Sea, planet.frame(), "");
    REQUIRE(legend);
    CHECK(legend.value_or(viewer::Legend{}).low == 4.0);
    CHECK(legend.value_or(viewer::Legend{}).high == 20.0);
}

TEST_CASE("each overlay asks for the layers it draws", "[viewer][overlay]") {
    const auto has = [](const std::vector<std::string>& layers, const std::string& name) {
        return std::ranges::find(layers, name) != layers.end();
    };
    CHECK(has(viewer::layers_for(Overlay::Sky, ""), "weather/storm"));
    CHECK(has(viewer::layers_for(Overlay::Fire, ""), "forest/fire"));
    CHECK(has(viewer::layers_for(Overlay::Water, ""), "planet/water_table"));
    CHECK(has(viewer::layers_for(Overlay::Water, ""), "land/water_table_rise"));
    CHECK(has(viewer::layers_for(Overlay::Soil, ""), "land/snow"));
    CHECK(has(viewer::layers_for(Overlay::Terrain, ""), "planet/biome"));
    CHECK(has(viewer::layers_for(Overlay::Sea, ""), "ocean/temperature"));
    CHECK(has(viewer::layers_for(Overlay::Sea, ""), "ocean/current_north"));
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

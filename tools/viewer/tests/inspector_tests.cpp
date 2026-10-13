#include <hog/viewer/inspector.hpp>
#include <hog/viewer/map_view.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec2;
using hog::viewer::Reading;
using hog::viewer::ReadingGroup;

TEST_CASE("an inspected point is the planet under the map point", "[viewer][inspector]") {
    const hog::probe::Inspect centre = hog::viewer::inspect_at(DVec2{0.5, 0.5});
    CHECK(centre.latitude == 0.0);
    CHECK(centre.longitude == 0.0);
    const hog::probe::Inspect corner = hog::viewer::inspect_at(DVec2{0.0, 0.0});
    CHECK(corner.latitude == 90.0);
    CHECK(corner.longitude == -180.0);

    for (const DVec2 point : {DVec2{0.1, 0.7}, DVec2{0.93, 0.21}, DVec2{1.0, 1.0}}) {
        const hog::probe::Inspect inspect = hog::viewer::inspect_at(point);
        // The same direction the map draws there.
        const hog::DVec3 direction = hog::viewer::map_direction(point);
        CHECK_THAT(std::asin(direction.z),
                   WithinAbs(inspect.latitude * std::numbers::pi / 180.0, 1e-12));
        const DVec2 back = hog::viewer::inspected_point(inspect);
        CHECK_THAT(back.x, WithinAbs(point.x, 1e-12));
        CHECK_THAT(back.y, WithinAbs(point.y, 1e-12));
    }
}

TEST_CASE("readings carry their units, and biomes their names", "[viewer][inspector]") {
    CHECK(hog::viewer::reading_text(12.5F, "°C") == "12.5 °C");
    CHECK(hog::viewer::reading_text(1013.25F, "hPa") == "1013 hPa");
    CHECK(hog::viewer::reading_text(0.000125F, "") == "0.000125");
    CHECK(hog::viewer::reading_text(250'000.0F, "kg/km²") == "250000 kg/km²");
    CHECK(hog::viewer::reading_text(-12'345.6F, "m") == "-12346 m");
    CHECK(hog::viewer::reading_text(5.0F, "biome") == "temperate forest");
    CHECK(hog::viewer::reading_text(99.0F, "biome") == "unknown 99");
    CHECK(hog::viewer::reading_text(2.5F, "biome") == "2.5 biome");
}

TEST_CASE("readings are grouped as the server offers its layers", "[viewer][inspector]") {
    const hog::probe::Welcome welcome{.layers = {{.name = "planet/area", .unit = "km²"},
                                                 {.name = "planet/biome", .unit = "biome"},
                                                 {.name = "weather/temperature", .unit = "°C"},
                                                 {.name = "wildlife/deer", .unit = "animals"},
                                                 {.name = "wildlife/deer/fed", .unit = ""}}};
    hog::probe::Inspection inspection{.inspect = {},
                                      .time = {},
                                      .cell = 0,
                                      .values = {36.5F, 6.0F, -3.25F, 120.0F, 0.75F},
                                      .column = {}};
    const std::vector<ReadingGroup> expected{
        {.name = "planet",
         .readings = {{.name = "area", .value = "36.5 km²"},
                      {.name = "biome", .value = "grassland"}}},
        {.name = "weather", .readings = {{.name = "temperature", .value = "-3.25 °C"}}},
        {.name = "wildlife",
         .readings = {{.name = "deer", .value = "120 animals"},
                      {.name = "deer/fed", .value = "0.75"}}},
    };
    CHECK(hog::viewer::readings(welcome, inspection) == expected);

    // From a server whose layers have since changed.
    inspection.values.pop_back();
    CHECK(hog::viewer::readings(welcome, inspection).empty());
}

TEST_CASE("the ground reads from the surface down", "[viewer][inspector]") {
    const hog::probe::Column column{.elevation = 240.5F,
                                    .biome = 5,
                                    .water_table = 12.25F,
                                    .river = 0.75F,
                                    .continental = true,
                                    .cover = 0,
                                    .cover_thickness = 1.5F,
                                    .sediment = 3,
                                    .sediment_thickness = 410.0F,
                                    .bedrock = 5,
                                    .seams = {{.resource = 1, .top = 12.0F, .thickness = 1.5F},
                                              {.resource = 6, .top = 300.0F, .thickness = 0.25F}}};
    const std::vector<Reading> expected{
        {.name = "elevation", .value = "240.5 m"},
        {.name = "biome", .value = "temperate forest"},
        {.name = "water table", .value = "12.25 m down"},
        {.name = "river", .value = "0.75 m deep"},
        {.name = "crust", .value = "continental"},
        {.name = "cover", .value = "soil, 0 m to 1.5 m"},
        {.name = "sediment", .value = "shale, 1.5 m to 411.5 m"},
        {.name = "bedrock", .value = "granite, from 411.5 m"},
        {.name = "coal", .value = "12 m to 13.5 m"},
        {.name = "gold", .value = "300 m to 300.2 m"},
    };
    CHECK(hog::viewer::column_readings(column) == expected);

    // Layers with no thickness are left out.
    const hog::probe::Column bare{.elevation = -3'000.0F,
                                  .biome = 0,
                                  .water_table = 0.0F,
                                  .river = 0.0F,
                                  .continental = false,
                                  .cover = 0,
                                  .cover_thickness = 0.0F,
                                  .sediment = 0,
                                  .sediment_thickness = 0.0F,
                                  .bedrock = 6,
                                  .seams = {}};
    const std::vector<Reading> floor{
        {.name = "elevation", .value = "-3000 m"},
        {.name = "biome", .value = "ocean"},
        {.name = "crust", .value = "oceanic"},
        {.name = "bedrock", .value = "basalt, from 0 m"},
    };
    CHECK(hog::viewer::column_readings(bare) == floor);
}

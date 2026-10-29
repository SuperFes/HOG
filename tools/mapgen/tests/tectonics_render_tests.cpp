#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/tectonics_render.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using hog::mapgen::crust_colour;
using hog::planet::Tectonics;

namespace {

constexpr double radius = 115'000.0;

int brightness(hog::mapgen::Rgb colour) {
    return colour[0] + colour[1] + colour[2];
}

} // namespace

TEST_CASE("young ocean floor is drawn paler than old", "[mapgen]") {
    const Tectonics::Column young{.crust = Tectonics::Crust::Oceanic, .age = 0.0F};
    const Tectonics::Column old{.crust = Tectonics::Crust::Oceanic, .age = 80.0F};
    CHECK(brightness(crust_colour(young)) > brightness(crust_colour(old)));
    CHECK(crust_colour(young)[2] > crust_colour(young)[0]);
}

TEST_CASE("continents are drawn whiter the thicker they pile up", "[mapgen]") {
    const Tectonics::Column settled{.crust = Tectonics::Crust::Continental, .thickness = 35.0F};
    const Tectonics::Column piled{.crust = Tectonics::Crust::Continental, .thickness = 90.0F};
    CHECK(brightness(crust_colour(piled)) > brightness(crust_colour(settled)));
    CHECK(crust_colour(settled)[1] > crust_colour(settled)[2]);
}

TEST_CASE("a tectonics map shows the crust with the plates' edges darkened", "[mapgen]") {
    const Tectonics tectonics{1, radius, {.level = 4, .plates = 6}};
    hog::jobs::JobSystem jobs{2};
    const auto image =
        hog::mapgen::render_tectonics(tectonics, radius, hog::mapgen::PlanetView{}, 128, jobs);
    REQUIRE(image.width == 128);
    REQUIRE(image.height == 64);
    REQUIRE(image.pixels.size() == std::size_t{3} * 128 * 64);
    // Some of it is continent, some ocean, and some an edge darker than the crust under it.
    const hog::mapgen::ImageSize size{.width = image.width, .height = image.height};
    int green = 0;
    int blue = 0;
    int edges = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const auto colour = image.at(x, y);
            green += colour[1] > colour[2] ? 1 : 0;
            blue += colour[2] > colour[1] ? 1 : 0;
            const auto direction =
                hog::mapgen::pixel_direction(hog::mapgen::PlanetView{}, size, x, y, radius);
            const auto cell =
                tectonics.grid().index(hog::planet::cell_at(direction, tectonics.grid().level()));
            const auto under = crust_colour(tectonics.columns()[cell]);
            if (colour != under) {
                CHECK(brightness(colour) < brightness(under));
                ++edges;
            }
        }
    }
    CHECK(green > 0);
    CHECK(blue > 0);
    CHECK(edges > 0);
}

TEST_CASE("tectonics maps are named by view and step", "[mapgen]") {
    CHECK(hog::mapgen::tectonics_file_name(hog::mapgen::PlanetView{}, 40) ==
          "planet-tectonics-0040.png");
}

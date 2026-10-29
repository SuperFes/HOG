#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/tectonics_render.hpp>
#include <hog/planet/isostasy.hpp>
#include <hog/planet/noise.hpp>
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

TEST_CASE("the sea is drawn darker the deeper, the land paler the higher", "[mapgen]") {
    using hog::mapgen::relief_colour;
    CHECK(brightness(relief_colour(-100.0)) > brightness(relief_colour(-4'000.0)));
    CHECK(relief_colour(-100.0)[2] > relief_colour(-100.0)[0]);
    CHECK(relief_colour(100.0)[1] > relief_colour(100.0)[2]);
    CHECK(brightness(relief_colour(5'000.0)) > brightness(relief_colour(100.0)));
}

TEST_CASE("a relief map shows land above the sea and sea below it", "[mapgen]") {
    const Tectonics tectonics{1, radius, {.level = 4, .plates = 6}};
    const auto heights = hog::planet::crust_heights(tectonics);
    hog::jobs::JobSystem jobs{2};
    const auto image =
        hog::mapgen::render_relief(tectonics, heights, 1'000 * hog::planet::fixed_one, radius,
                                   hog::mapgen::PlanetView{}, 128, jobs);
    REQUIRE(image.pixels.size() == std::size_t{3} * 128 * 64);
    int land = 0;
    int sea = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const auto colour = image.at(x, y);
            land += colour[1] > colour[2] ? 1 : 0;
            sea += colour[2] > colour[1] ? 1 : 0;
        }
    }
    CHECK(land > 0);
    CHECK(sea > land);
}

TEST_CASE("plates' ocean floor is drawn paler the younger", "[mapgen]") {
    using hog::mapgen::plate_crust_colour;
    const hog::planet::Crust young{.type = hog::planet::CrustType::Oceanic, .age = 0.0F};
    const hog::planet::Crust old{.type = hog::planet::CrustType::Oceanic, .age = 150.0F};
    CHECK(brightness(plate_crust_colour(young)) > brightness(plate_crust_colour(old)));
}

TEST_CASE("a plates map shows the mantle where they part and red where they overlap", "[mapgen]") {
    const Tectonics tectonics{1, radius, {.level = 4, .plates = 6}};
    hog::planet::Lithosphere lithosphere = hog::planet::lithosphere_from(tectonics, 10.0);
    hog::jobs::JobSystem jobs{2};
    struct Counts {
        int mantle = 0;
        int overlap = 0;
    };
    const auto count = [&] {
        const auto image =
            hog::mapgen::render_plates(lithosphere, radius, hog::mapgen::PlanetView{}, 128, jobs);
        REQUIRE(image.pixels.size() == std::size_t{3} * 128 * 64);
        Counts counts;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const auto colour = image.at(x, y);
                counts.mantle += colour == hog::mapgen::mantle_colour ? 1 : 0;
                counts.overlap += colour[0] > colour[1] && colour[0] > colour[2] ? 1 : 0;
            }
        }
        return counts;
    };
    const Counts start = count();
    CHECK(start.mantle == 0);
    CHECK(start.overlap == 0);

    lithosphere.advance(100.0);
    const Counts moved = count();
    CHECK(moved.mantle > 0);
    CHECK(moved.overlap > 0);
}

TEST_CASE("evolution maps are named by view, kind and step", "[mapgen]") {
    CHECK(hog::mapgen::evolution_file_name(hog::mapgen::PlanetView{}, "tectonics", 40) ==
          "planet-tectonics-0040.png");
    CHECK(hog::mapgen::evolution_file_name(hog::mapgen::PlanetView{}, "relief", 5) ==
          "planet-relief-0005.png");
}

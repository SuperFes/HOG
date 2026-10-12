#include <hog/viewer/ground.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>

using hog::DVec2;
using hog::viewer::MapRect;

TEST_CASE("the whole map surveys the whole planet", "[viewer][ground]") {
    const hog::probe::Survey survey = hog::viewer::survey_of(MapRect{}, 64, 32, true, 12.0F);
    CHECK(survey == hog::probe::Survey{.west = -180.0,
                                       .east = 180.0,
                                       .north = 90.0,
                                       .south = -90.0,
                                       .width = 64,
                                       .height = 32,
                                       .underground = true,
                                       .depth = 12.0F});
}

TEST_CASE("a zoomed map surveys only what's shown", "[viewer][ground]") {
    const MapRect shown{.low = {0.5, 0.25}, .high = {0.75, 0.5}};
    const hog::probe::Survey survey = hog::viewer::survey_of(shown, 8, 4, false, 0.0F);
    CHECK(survey.west == 0.0);
    CHECK(survey.east == 90.0);
    CHECK(survey.north == 45.0);
    CHECK(survey.south == 0.0);
}

TEST_CASE("each pixel's centre falls in its own sample", "[viewer][ground]") {
    const MapRect shown{.low = {0.25, 0.25}, .high = {0.75, 0.5}};
    const hog::probe::Ground ground{.survey = hog::viewer::survey_of(shown, 8, 4, false, 0.0F),
                                    .elevation = {},
                                    .biome = {},
                                    .rock = {},
                                    .deposit = {}};
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK(hog::viewer::sample_at(ground, hog::viewer::pixel_point(shown, 8, 4, x, y)) ==
                  std::optional{static_cast<std::size_t>((y * 8) + x)});
        }
    }
    CHECK_FALSE(hog::viewer::sample_at(ground, DVec2{0.2, 0.3}));
    CHECK_FALSE(hog::viewer::sample_at(ground, DVec2{0.5, 0.6}));
    CHECK_FALSE(hog::viewer::sample_at(ground, DVec2{0.75, 0.3}));
}

TEST_CASE("surveys wait for the view to settle and aren't repeated", "[viewer][ground]") {
    using namespace std::chrono_literals;
    hog::viewer::SurveyPacer pacer{150ms};
    const hog::MonotonicClock::time_point start{};
    hog::probe::Survey wanted = hog::viewer::survey_of(MapRect{}, 64, 32, false, 0.0F);
    CHECK_FALSE(pacer.poll(wanted, start));
    CHECK_FALSE(pacer.poll(wanted, start + 100ms));
    CHECK(pacer.poll(wanted, start + 150ms) == wanted);
    CHECK_FALSE(pacer.poll(wanted, start + 500ms));

    // Each change starts the wait again.
    wanted.depth = 10.0F;
    CHECK_FALSE(pacer.poll(wanted, start + 600ms));
    wanted.depth = 20.0F;
    CHECK_FALSE(pacer.poll(wanted, start + 700ms));
    CHECK(pacer.poll(wanted, start + 850ms) == wanted);

    pacer.forget();
    CHECK(pacer.poll(wanted, start + 851ms) == wanted);
}

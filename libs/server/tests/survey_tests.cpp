#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/server/survey.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>

using namespace hog;
using hog::probe::Survey;

namespace {

constexpr double radius = 115'000.0;

const planet::Geology& geology() {
    static const planet::Geology made{
        planet::PlanetConfig{.seed = 1, .ocean_fraction = 0.7, .radius = radius}};
    return made;
}

Survey whole(std::uint16_t width) {
    return Survey{.west = -180.0,
                  .east = 180.0,
                  .north = 90.0,
                  .south = -90.0,
                  .width = width,
                  .height = static_cast<std::uint16_t>(width / 2),
                  .underground = false,
                  .depth = 0.0F};
}

} // namespace

TEST_CASE("surveys must lie on the planet and be a sensible size", "[server][survey]") {
    CHECK(server::check_survey(whole(1'024)));
    const auto refused = [](Survey survey) {
        const auto checked = server::check_survey(survey);
        return !checked && checked.error().code == ErrorCode::InvalidArgument;
    };
    Survey survey = whole(64);
    survey.west = 20.0;
    survey.east = 10.0;
    CHECK(refused(survey));
    survey = whole(64);
    survey.north = 95.0;
    CHECK(refused(survey));
    survey = whole(64);
    survey.south = std::numeric_limits<double>::quiet_NaN();
    CHECK(refused(survey));
    survey = whole(64);
    survey.height = 0;
    CHECK(refused(survey));
    CHECK(refused(whole(server::most_survey_side + 2)));
    survey = whole(64);
    survey.depth = -1.0F;
    CHECK(refused(survey));
    survey.depth = std::numeric_limits<float>::infinity();
    CHECK(refused(survey));
}

TEST_CASE("surveys sample cells as small as their samples are apart", "[server][survey]") {
    const int coarse = server::survey_level(whole(128), radius);
    CHECK(server::survey_level(whole(2'048), radius) == coarse + 4);
    // The same samples over a sixteenth of the width look four levels deeper.
    Survey zoomed = whole(128);
    zoomed.west = 0.0;
    zoomed.east = 22.5;
    zoomed.north = 11.25;
    zoomed.south = 0.0;
    CHECK(server::survey_level(zoomed, radius) == coarse + 4);
    // Cells no larger than the samples are apart.
    const double apart = std::numbers::pi * radius / 64.0;
    CHECK(planet::level_for(apart, radius) == coarse);
}

TEST_CASE("a survey reports the ground under each sample", "[server][survey]") {
    const Survey survey{.west = -40.0,
                        .east = 40.0,
                        .north = 30.0,
                        .south = -30.0,
                        .width = 16,
                        .height = 12,
                        .underground = GENERATE(false, true),
                        .depth = 25.0F};
    jobs::JobSystem jobs{2};
    const probe::Ground ground = server::survey_ground(geology(), radius, survey, jobs);
    CHECK(ground.survey == survey);
    const std::size_t samples = std::size_t{16} * 12;
    REQUIRE(ground.elevation.size() == samples);
    REQUIRE(ground.biome.size() == samples);
    REQUIRE(ground.rock.size() == (survey.underground ? samples : 0));
    REQUIRE(ground.deposit.size() == ground.rock.size());

    const int level = server::survey_level(survey, radius);
    const auto depth = std::llround(25.0 * planet::fixed_one);
    for (std::size_t row = 0; row < survey.height; ++row) {
        for (std::size_t column = 0; column < survey.width; ++column) {
            // Sample centres, from the north-west.
            const double latitude =
                (30.0 - (5.0 * (static_cast<double>(row) + 0.5))) * std::numbers::pi / 180.0;
            const double longitude =
                (-40.0 + (5.0 * (static_cast<double>(column) + 0.5))) * std::numbers::pi / 180.0;
            const planet::CellId cell =
                planet::cell_at(planet::direction_at(latitude, longitude), level);
            const std::size_t sample = (row * survey.width) + column;
            CHECK(ground.elevation[sample] ==
                  static_cast<float>(planet::fixed_to_double(geology().terrain().elevation(cell))));
            CHECK(ground.biome[sample] == std::to_underlying(geology().climate().biome(cell)));
            if (survey.underground) {
                CHECK(ground.rock[sample] ==
                      std::to_underlying(geology().strata(cell).rock_at(depth)));
                const auto resource = planet::resource_at(geology().deposits(cell), depth);
                CHECK(ground.deposit[sample] == (resource ? std::to_underlying(*resource) + 1 : 0));
            }
        }
    }
}

TEST_CASE("the whole planet has land and sea", "[server][survey]") {
    jobs::JobSystem jobs{2};
    const probe::Ground ground = server::survey_ground(geology(), radius, whole(128), jobs);
    CHECK(std::ranges::any_of(ground.elevation, [](float metres) { return metres > 0.0F; }));
    CHECK(std::ranges::any_of(ground.elevation, [](float metres) { return metres < 0.0F; }));
}

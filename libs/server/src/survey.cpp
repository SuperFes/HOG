#include <hog/core/assert.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/server/survey.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
#include <utility>

namespace hog::server {

namespace {

constexpr double radians(double degrees) noexcept {
    return degrees * std::numbers::pi / 180.0;
}

// Fills one row of samples, from the west.
void survey_row(const planet::Geology& geology, const probe::Survey& survey, int level,
                std::uint32_t row, probe::Ground& ground) {
    const double latitude =
        radians(survey.north - ((row + 0.5) / survey.height * (survey.north - survey.south)));
    const std::int64_t depth = std::llround(static_cast<double>(survey.depth) * planet::fixed_one);
    const std::size_t first = static_cast<std::size_t>(row) * survey.width;
    for (std::uint32_t column = 0; column < survey.width; ++column) {
        const double longitude =
            radians(survey.west + ((column + 0.5) / survey.width * (survey.east - survey.west)));
        const planet::CellId cell =
            planet::cell_at(planet::direction_at(latitude, longitude), level);
        const std::size_t sample = first + column;
        ground.elevation[sample] =
            static_cast<float>(planet::fixed_to_double(geology.terrain().elevation(cell)));
        ground.biome[sample] = std::to_underlying(geology.climate().biome(cell));
        if (!survey.underground) {
            continue;
        }
        ground.rock[sample] = std::to_underlying(geology.strata(cell).rock_at(depth));
        const auto resource = planet::resource_at(geology.deposits(cell), depth);
        ground.deposit[sample] =
            resource ? static_cast<std::uint8_t>(std::to_underlying(*resource) + 1) : 0;
    }
}

} // namespace

Result<void> check_survey(const probe::Survey& survey) {
    const bool across = survey.west >= -180.0 && survey.west < survey.east && survey.east <= 180.0;
    const bool down = survey.south >= -90.0 && survey.south < survey.north && survey.north <= 90.0;
    if (!across || !down) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("survey edges west {} east {} north {} south {} aren't in "
                                      "order on the planet",
                                      survey.west, survey.east, survey.north, survey.south));
    }
    if (survey.width == 0 || survey.height == 0 || survey.width > most_survey_side ||
        survey.height > most_survey_side) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("a survey takes 1 to {} samples a side, not {} by {}",
                                      most_survey_side, survey.width, survey.height));
    }
    if (!(survey.depth >= 0.0F) || std::isinf(survey.depth)) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("a survey can't look {} m down", survey.depth));
    }
    return {};
}

int survey_level(const probe::Survey& survey, double planet_radius) {
    const double nearest = std::clamp(0.0, survey.south, survey.north);
    const double down = radians(survey.north - survey.south) / survey.height;
    const double across =
        radians(survey.east - survey.west) / survey.width * std::cos(radians(nearest));
    return planet::level_for(std::max(std::min(down, across), 1e-12) * planet_radius,
                             planet_radius);
}

probe::Ground survey_ground(const planet::Geology& geology, double planet_radius,
                            const probe::Survey& survey, jobs::JobSystem& jobs) {
    HOG_ASSERT(check_survey(survey).has_value(), "survey not checked");
    const std::size_t samples = static_cast<std::size_t>(survey.width) * survey.height;
    const std::size_t below = survey.underground ? samples : 0;
    probe::Ground ground{.survey = survey,
                         .elevation = std::vector<float>(samples),
                         .biome = std::vector<std::uint8_t>(samples),
                         .rock = std::vector<std::uint8_t>(below),
                         .deposit = std::vector<std::uint8_t>(below)};
    const int level = survey_level(survey, planet_radius);
    jobs.parallel_for(survey.height, 1, [&](jobs::Range rows, std::uint32_t /*thread*/) {
        for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
            survey_row(geology, survey, level, row, ground);
        }
    });
    return ground;
}

} // namespace hog::server

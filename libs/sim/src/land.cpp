#include "weather_stages.hpp"

#include <hog/core/assert.hpp>
#include <hog/sim/land.hpp>
#include <hog/sim/weather.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace hog::sim {

LandFields::LandFields(std::size_t size)
    : snow(size), soil(size), groundwater(size), runoff(size), usual_groundwater(size) {}

Land::Land(const CellGrid& grid, std::span<const Ground> ground, const LandTuning& tuning)
    : tuning_(tuning), fields_(grid.size()) {
    HOG_ASSERT(ground.size() == grid.size(), "need ground for every cell");
    land_.reserve(grid.size());
    for (const Ground& cell : ground) {
        land_.push_back(!cell.ocean);
    }
}

void Land::set_fields(const CellGrid& grid, LandFields fields) {
    HOG_ASSERT(fields.size() == grid.size() && fields.soil.size() == grid.size() &&
                   fields.groundwater.size() == grid.size() &&
                   fields.runoff.size() == grid.size() &&
                   fields.usual_groundwater.size() == grid.size(),
               "need water for every cell");
    fields_ = std::move(fields);
}

double Land::snow_cover(std::uint32_t index) const {
    const auto snow = static_cast<double>(fields_.snow[index]);
    return snow / (snow + tuning_.covering_snow);
}

double Land::wetness(std::uint32_t index) const {
    const double cover = snow_cover(index);
    return ((1.0 - cover) * static_cast<double>(fields_.soil[index])) +
           (cover * tuning_.snow_wetness);
}

double Land::water_table_rise(std::uint32_t index) const {
    const double above = fields_.groundwater[index] - fields_.usual_groundwater[index];
    return above / 1'000.0 / tuning_.specific_yield;
}

double Land::water(std::uint32_t index) const {
    return static_cast<double>(fields_.snow[index]) +
           (static_cast<double>(fields_.soil[index]) * tuning_.soil_capacity) +
           fields_.groundwater[index];
}

void Land::step(const CellGrid& grid, const LandWeather& weather, jobs::JobSystem* jobs) {
    HOG_ASSERT(weather.rain.size() == grid.size() && weather.snow.size() == grid.size() &&
                   weather.evaporation.size() == grid.size() &&
                   weather.temperature.size() == grid.size(),
               "need weather for every cell");
    detail::for_cells(jobs, grid.size(), [&](std::uint32_t index) {
        if (land_[index]) {
            step(index, weather);
        }
    });
}

void Land::step(std::uint32_t index, const LandWeather& weather) {
    const double days = weather.seconds / 86'400.0;
    const double capacity = tuning_.soil_capacity;
    auto snow = static_cast<double>(fields_.snow[index]) + static_cast<double>(weather.snow[index]);
    const double warmth = static_cast<double>(weather.temperature[index]) - tuning_.melting;
    const double melt = std::min(snow, tuning_.melt_per_degree * std::max(warmth, 0.0) * days);
    snow -= melt;

    // What the sun draws out comes from the snow where it lies, and the soil elsewhere.
    const double cover = snow / (snow + tuning_.covering_snow);
    const double drawn = std::max(static_cast<double>(weather.evaporation[index]), 0.0);
    const double sublimed = std::min(snow, drawn * cover);
    snow -= sublimed;

    double soil = static_cast<double>(fields_.soil[index]) * capacity;
    const double arriving = static_cast<double>(weather.rain[index]) + melt;
    double runoff = arriving * std::pow(soil / capacity, tuning_.shedding);
    soil += arriving - runoff;
    runoff += std::max(soil - capacity, 0.0);
    soil = std::min(soil, capacity);
    soil = std::max(soil - (drawn - sublimed), 0.0);

    const double fullness = soil / capacity;
    const double percolating =
        soil * fullness * fullness * detail::approach(weather.seconds, tuning_.percolation);
    soil -= percolating;
    double ground = fields_.groundwater[index] + percolating;
    const double seeping = ground * detail::approach(weather.seconds, tuning_.seepage);
    ground -= seeping;
    runoff += seeping;

    const double usual = fields_.usual_groundwater[index];
    fields_.snow[index] = static_cast<float>(snow);
    fields_.soil[index] = static_cast<float>(soil / capacity);
    fields_.groundwater[index] = ground;
    fields_.runoff[index] = static_cast<float>(runoff);
    fields_.usual_groundwater[index] =
        usual + ((ground - usual) * detail::approach(weather.seconds, tuning_.usual));
}

} // namespace hog::sim

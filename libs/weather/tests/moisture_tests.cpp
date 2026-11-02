#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/weather/field.hpp>
#include <hog/weather/moisture.hpp>
#include <hog/weather/wind.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

using Catch::Approx;
using hog::globe::Planet;
using hog::globe::Volatile;
using namespace hog::weather;

namespace {

constexpr double pressure = 90'000.0;

double latent() {
    return hog::globe::latent_heat(Volatile::Water).value_or(0.0);
}

// What heat and water a parcel holds: its dry air's and vapour's warmth, and the heat its
// vapour would give up.
double moist_energy(const Parcel& parcel) {
    return (air_heat_capacity() * parcel.temperature) + (latent() * parcel.vapour);
}

double water(const Parcel& parcel) {
    return parcel.vapour + parcel.cloud;
}

} // namespace

TEST_CASE("air holding more than it can condenses to cloud, warming", "[weather][moisture]") {
    Parcel parcel{.temperature = 285.0, .vapour = 0.02, .cloud = 0.0};
    const double energy = moist_energy(parcel);
    const double total = water(parcel);
    saturate(parcel, Volatile::Water, latent(), pressure);
    CHECK(parcel.cloud > 0.0);
    CHECK(parcel.temperature > 285.0);
    CHECK(water(parcel) == Approx(total));
    CHECK(moist_energy(parcel) == Approx(energy).epsilon(1e-5));
    // Left holding as much as it can at the temperature it came to.
    CHECK(parcel.vapour ==
          Approx(saturation_ratio(Volatile::Water, parcel.temperature, pressure)).epsilon(1e-3));
}

TEST_CASE("cloud in dry air evaporates, cooling it, until the air is full or the cloud gone",
          "[weather][moisture]") {
    Parcel thin{.temperature = 285.0, .vapour = 0.002, .cloud = 0.001};
    saturate(thin, Volatile::Water, latent(), pressure);
    CHECK(thin.cloud == Approx(0.0).margin(1e-12));
    CHECK(thin.vapour == Approx(0.003).epsilon(1e-6));
    CHECK(thin.temperature < 285.0);

    Parcel thick{.temperature = 285.0, .vapour = 0.002, .cloud = 0.05};
    const double energy = moist_energy(thick);
    saturate(thick, Volatile::Water, latent(), pressure);
    CHECK(thick.cloud > 0.0);
    CHECK(thick.cloud < 0.05);
    CHECK(moist_energy(thick) == Approx(energy).epsilon(1e-5));
}

TEST_CASE("air that holds less than it can is left alone", "[weather][moisture]") {
    Parcel parcel{.temperature = 285.0, .vapour = 0.004, .cloud = 0.0};
    saturate(parcel, Volatile::Water, latent(), pressure);
    CHECK(parcel.temperature == 285.0);
    CHECK(parcel.vapour == 0.004);
    CHECK(parcel.cloud == 0.0);
}

TEST_CASE("a parcel lifted from the ground makes cloud above a base that rises with dry air",
          "[weather][moisture]") {
    const Planet planet = hog::globe::preset("earth").value_or(Planet{});
    const double surface = 295.0;
    const auto base = [&](double humidity) {
        const double held0 = saturation_ratio(Volatile::Water, surface, planet.pressure);
        for (int metres = 0; metres < 8'000; metres += 25) {
            const double height = metres;
            Parcel parcel{.temperature = surface - (dry_lapse_rate(planet) * height),
                          .vapour = humidity * held0,
                          .cloud = 0.0};
            saturate(parcel, Volatile::Water, latent(), pressure_at(planet, surface, height));
            if (parcel.cloud > 1e-6) {
                return height;
            }
        }
        return 8'000.0;
    };
    // Cloud base is about 125 m for each degree the dew point lies under the temperature.
    CHECK(base(0.8) > 300.0);
    CHECK(base(0.8) < 1'000.0);
    CHECK(base(0.4) > 1.5 * base(0.8));
}

TEST_CASE("a world with no volatile makes no cloud", "[weather][moisture]") {
    Planet moon = hog::globe::preset("moon").value_or(Planet{});
    moon.radius = 20'000.0;
    const hog::globe::Generator generator{moon};
    Field field = start_field(generator, 2, 4);
    field.air[0].temperature = 150.0F;
    condense(field, moon);
    evaporate(field, moon, 3'600.0);
    for (const auto& air : field.air) {
        REQUIRE(air.cloud == 0.0F);
        REQUIRE(air.vapour == 0.0F);
    }
}

TEST_CASE("the sea feeds the air above it more than the land does", "[weather][moisture]") {
    Planet planet = hog::globe::preset("earth").value_or(Planet{});
    planet.radius = 20'000.0;
    const hog::globe::Generator generator{planet};
    Field field = start_field(generator, 3, 6);
    for (auto& air : field.air) {
        air.vapour = 0.0F;
    }
    evaporate(field, planet, 6.0 * 3'600.0);
    double sea = 0.0;
    double land = 0.0;
    int seas = 0;
    int lands = 0;
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        int layer = 0;
        while (layer < field.grid.layers && field.solid(column, layer)) {
            ++layer;
        }
        if (layer == field.grid.layers) {
            continue;
        }
        const auto gained = static_cast<double>(field.at(column, layer).vapour);
        const double held =
            saturation_ratio(planet.volatiles,
                             air_temperature(planet, static_cast<double>(field.surface[column]),
                                             field.grid.radius_of(layer) - planet.radius),
                             pressure_at(planet, static_cast<double>(field.surface[column]),
                                         field.grid.radius_of(layer) - planet.radius));
        if (held <= 0.0) {
            continue;
        }
        (field.over_sea[column] != 0 ? sea : land) += gained / held;
        (field.over_sea[column] != 0 ? seas : lands) += 1;
    }
    REQUIRE(seas > 0);
    REQUIRE(lands > 0);
    CHECK(sea / seas > 2.0 * (land / lands));
    CHECK(sea / seas < 1.0);
}

TEST_CASE("moist air rises further than dry, as condensing warms it: a storm",
          "[weather][moisture]") {
    Planet planet = hog::globe::preset("earth").value_or(Planet{});
    planet.radius = 20'000.0;
    const hog::globe::Generator generator{planet};
    const Field start = start_field(generator, 3, 8);
    const Stencil stencil{start.grid};

    std::size_t column = 0;
    while (start.over_sea[column] == 0 || std::abs(stencil.up(column).z) > 0.4F) {
        ++column;
    }
    const auto strongest_updraft = [&](bool moist) {
        Field field = start;
        for (int layer = 0; layer < 3; ++layer) {
            Air& air = field.at(column, layer);
            air.temperature += 6.0F;
            const double radius = field.grid.radius_of(layer);
            air.vapour = moist ? static_cast<float>(saturation_ratio(
                                     planet.volatiles, static_cast<double>(air.temperature),
                                     pressure_at(planet, static_cast<double>(field.surface[column]),
                                                 radius - planet.radius)))
                               : 0.0F;
        }
        float best = 0.0F;
        for (int i = 0; i < 20; ++i) {
            step(field, stencil, planet, 60.0);
            for (int layer = 0; layer < field.grid.layers; ++layer) {
                best = std::max(best, glm::dot(field.at(column, layer).wind, stencil.up(column)));
            }
        }
        return best;
    };
    const float dry = strongest_updraft(false);
    const float wet = strongest_updraft(true);
    CHECK(dry > 0.0F);
    CHECK(wet > 1.2F * dry);
}

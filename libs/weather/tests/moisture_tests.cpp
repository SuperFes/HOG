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
#include <vector>

using Catch::Approx;
using hog::globe::Planet;
using namespace hog::weather;

namespace {

constexpr double pressure = 90'000.0;

double latent() {
    return hog::globe::latent_heat;
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
    saturate(parcel, latent(), pressure);
    CHECK(parcel.cloud > 0.0);
    CHECK(parcel.temperature > 285.0);
    CHECK(water(parcel) == Approx(total));
    CHECK(moist_energy(parcel) == Approx(energy).epsilon(1e-5));
    // Left holding as much as it can at the temperature it came to.
    CHECK(parcel.vapour == Approx(saturation_ratio(parcel.temperature, pressure)).epsilon(1e-3));
}

TEST_CASE("cloud in dry air evaporates, cooling it, until the air is full or the cloud gone",
          "[weather][moisture]") {
    Parcel thin{.temperature = 285.0, .vapour = 0.002, .cloud = 0.001};
    saturate(thin, latent(), pressure);
    CHECK(thin.cloud == Approx(0.0).margin(1e-12));
    CHECK(thin.vapour == Approx(0.003).epsilon(1e-6));
    CHECK(thin.temperature < 285.0);

    Parcel thick{.temperature = 285.0, .vapour = 0.002, .cloud = 0.05};
    const double energy = moist_energy(thick);
    saturate(thick, latent(), pressure);
    CHECK(thick.cloud > 0.0);
    CHECK(thick.cloud < 0.05);
    CHECK(moist_energy(thick) == Approx(energy).epsilon(1e-5));
}

TEST_CASE("air that holds less than it can is left alone", "[weather][moisture]") {
    Parcel parcel{.temperature = 285.0, .vapour = 0.004, .cloud = 0.0};
    saturate(parcel, latent(), pressure);
    CHECK(parcel.temperature == 285.0);
    CHECK(parcel.vapour == 0.004);
    CHECK(parcel.cloud == 0.0);
}

TEST_CASE("a parcel lifted from the ground makes cloud above a base that rises with dry air",
          "[weather][moisture]") {
    const Planet planet;
    const double surface = 295.0;
    const auto base = [&](double humidity) {
        const double held0 = saturation_ratio(surface, planet.system.air.pressure);
        for (int metres = 0; metres < 8'000; metres += 25) {
            const double height = metres;
            Parcel parcel{.temperature = surface - (dry_lapse_rate(planet) * height),
                          .vapour = humidity * held0,
                          .cloud = 0.0};
            saturate(parcel, latent(), pressure_at(planet, surface, height));
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
    Planet moon;
    moon.system.air.pressure = 0.0;
    moon.body.radius = 20'000.0;
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
    Planet planet;
    planet.body.radius = 20'000.0;
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
            saturation_ratio(air_temperature(planet, static_cast<double>(field.surface[column]),
                                             field.grid.radius_of(layer) - planet.body.radius),
                             pressure_at(planet, static_cast<double>(field.surface[column]),
                                         field.grid.radius_of(layer) - planet.body.radius));
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
    Planet planet;
    planet.body.radius = 20'000.0;
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
                                     static_cast<double>(air.temperature),
                                     pressure_at(planet, static_cast<double>(field.surface[column]),
                                                 radius - planet.body.radius)))
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

namespace {

struct Small {
    Planet planet = [] {
        Planet p;
        p.body.radius = 20'000.0;
        return p;
    }();
    hog::globe::Generator generator{planet};
    Field field = start_field(generator, 3, 8);
    Stencil stencil{field.grid};

    // A sea column away from the poles.
    [[nodiscard]] std::size_t sea_column() const {
        std::size_t column = 0;
        while (field.over_sea[column] == 0 || std::abs(stencil.up(column).z) > 0.4F) {
            ++column;
        }
        return column;
    }
    [[nodiscard]] double total_water(std::size_t column) const {
        double total = 0.0;
        for (int layer = 0; layer < field.grid.layers; ++layer) {
            const Air& air = field.at(column, layer);
            total += static_cast<double>(air.cloud) + static_cast<double>(air.rain);
        }
        return total;
    }
};

} // namespace

TEST_CASE("cloud thicker than a limit grows drops, and thin cloud doesn't", "[weather][rain]") {
    Small world;
    const std::size_t column = world.sea_column();
    Air& air = world.field.at(column, 3);
    air.cloud = 0.0005F;
    precipitate(world.field, world.planet, 600.0);
    CHECK(world.field.at(column, 3).rain == 0.0F);

    world.field.at(column, 3).cloud = 0.004F;
    precipitate(world.field, world.planet, 600.0);
    const Air& thick = world.field.at(column, 3);
    CHECK(thick.rain + world.field.at(column, 2).rain > 0.0F);
    CHECK(thick.cloud < 0.004F);
}

TEST_CASE("rain falls to the ground and is recorded, and no water is lost on the way",
          "[weather][rain]") {
    Small world;
    const std::size_t column = world.sea_column();
    // Saturated air, so none evaporates on the way.
    for (int layer = 0; layer < world.field.grid.layers; ++layer) {
        Air& air = world.field.at(column, layer);
        air.vapour = static_cast<float>(saturation_ratio(
            static_cast<double>(air.temperature),
            pressure_at(world.planet, static_cast<double>(world.field.surface[column]),
                        world.field.grid.radius_of(layer) - world.planet.body.radius)));
    }
    world.field.at(column, 6).rain = 0.002F;
    double reached = 0.0;
    for (int i = 0; i < 60; ++i) {
        precipitate(world.field, world.planet, 30.0);
        reached += static_cast<double>(world.field.precipitation[column]) * 30.0 / 3'600.0;
    }
    CHECK(reached > 0.0);
    CHECK(world.total_water(column) < 0.002);
}

TEST_CASE("rain in dry air evaporates before it lands, cooling the air", "[weather][rain]") {
    Small world;
    const std::size_t column = world.sea_column();
    for (int layer = 0; layer < world.field.grid.layers; ++layer) {
        world.field.at(column, layer).vapour = 0.0F;
    }
    const float before = world.field.at(column, 3).temperature;
    world.field.at(column, 6).rain = 0.0005F;
    double reached = 0.0;
    for (int i = 0; i < 60; ++i) {
        precipitate(world.field, world.planet, 30.0);
        reached += static_cast<double>(world.field.precipitation[column]) * 30.0 / 3'600.0;
    }
    // The cell holds about 0.0005 kg/kg of air a kilogram a cubic metre or less over its depth, and
    // a fifth of it or less reaches the ground.
    CHECK(reached < 0.25 * 0.0005 * world.field.grid.depth_of(6));
    CHECK(world.field.at(column, 3).temperature < before);
}

TEST_CASE("air heavy with rain sinks", "[weather][rain]") {
    Small world;
    const std::size_t column = world.sea_column();
    Field clear = world.field;
    world.field.at(column, 4).rain = 0.005F;
    accelerate(world.field, world.stencil, world.planet, 60.0);
    accelerate(clear, world.stencil, world.planet, 60.0);
    const auto rise = [&](const Field& field) {
        return glm::dot(field.at(column, 4).wind, world.stencil.up(column));
    };
    CHECK(rise(world.field) < rise(clear) - 1.0F);
}

TEST_CASE("a heated, moist column makes a storm that rains where a dry one barely does",
          "[weather][rain]") {
    Small world;
    const std::size_t column = world.sea_column();
    const Field start = world.field;
    const auto rained = [&](bool moist) {
        Field field = start;
        for (int layer = 0; layer < 3; ++layer) {
            Air& air = field.at(column, layer);
            air.temperature += 6.0F;
            air.vapour =
                moist ? static_cast<float>(saturation_ratio(
                            static_cast<double>(air.temperature),
                            pressure_at(world.planet, static_cast<double>(field.surface[column]),
                                        field.grid.radius_of(layer) - world.planet.body.radius)))
                      : 0.0F;
        }
        double total = 0.0;
        for (int i = 0; i < 90; ++i) {
            step(field, world.stencil, world.planet, 60.0);
            total += static_cast<double>(field.precipitation[column]) * 60.0 / 3'600.0;
        }
        return total;
    };
    const double wet = rained(true);
    CHECK(wet > 0.0);
    CHECK(wet > 3.0 * rained(false));
}

TEST_CASE("the sea gives up the heat of the water it evaporates", "[weather][moisture]") {
    Planet planet;
    planet.body.radius = 20'000.0;
    const hog::globe::Generator generator{planet};
    Field field = start_field(generator, 3, 6);
    for (auto& air : field.air) {
        air.vapour = 0.0F;
    }
    const std::vector<float> before = field.surface;
    evaporate(field, planet, 6.0 * 3'600.0);
    int cooled = 0;
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        CHECK(field.surface[column] <= before[column]);
        cooled += field.surface[column] < before[column] ? 1 : 0;
    }
    CHECK(cooled > 0);
}

TEST_CASE("heat the air takes from the ground, the ground loses", "[weather][moisture]") {
    Planet planet;
    planet.body.radius = 20'000.0;
    const hog::globe::Generator generator{planet};
    Field field = start_field(generator, 3, 6);
    // Air a good deal colder than the ground under it.
    for (auto& air : field.air) {
        air.temperature -= 15.0F;
    }
    const Field before = field;
    exchange_heat(field, planet, 600.0);
    double taken = 0.0;
    double lost = 0.0;
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        int layer = 0;
        while (layer < field.grid.layers && field.solid(column, layer)) {
            ++layer;
        }
        if (layer == field.grid.layers) {
            continue;
        }
        const double height = field.grid.radius_of(layer) - planet.body.radius;
        const double mass =
            air_density(static_cast<double>(field.at(column, layer).temperature),
                        pressure_at(planet, static_cast<double>(field.surface[column]), height)) *
            field.grid.depth_of(layer);
        taken += mass * air_heat_capacity() *
                 static_cast<double>(field.at(column, layer).temperature -
                                     before.at(column, layer).temperature);
        lost += surface_heat(field, column) *
                static_cast<double>(before.surface[column] - field.surface[column]);
    }
    CHECK(taken > 0.0);
    CHECK(lost == Approx(taken).epsilon(1e-2));
}

TEST_CASE("rain keeps its water falling through cells that grow with height", "[weather][rain]") {
    Small world;
    const std::size_t column = world.sea_column();
    const Field& field = world.field;
    const auto mass = [&](int layer) {
        const double height = field.grid.radius_of(layer) - world.planet.body.radius;
        const auto surface = static_cast<double>(field.surface[column]);
        const double radius = field.grid.radius_of(layer);
        return air_density(air_temperature(world.planet, surface, height),
                           pressure_at(world.planet, surface, height)) *
               field.grid.depth_of(layer) * radius * radius;
    };
    for (int layer = 0; layer < field.grid.layers; ++layer) {
        Air& air = world.field.at(column, layer);
        air.vapour = static_cast<float>(
            saturation_ratio(static_cast<double>(air.temperature),
                             pressure_at(world.planet, static_cast<double>(field.surface[column]),
                                         field.grid.radius_of(layer) - world.planet.body.radius)));
    }
    world.field.at(column, 6).rain = 0.002F;
    const double start = 0.002 * mass(6);
    const double ground = field.ground[column];
    double reached = 0.0;
    for (int i = 0; i < 40; ++i) {
        precipitate(world.field, world.planet, 30.0);
        reached +=
            static_cast<double>(field.precipitation[column]) * 30.0 / 3'600.0 * ground * ground;
    }
    double held = 0.0;
    for (int layer = 0; layer < field.grid.layers; ++layer) {
        const Air& air = field.at(column, layer);
        held += (static_cast<double>(air.rain) + static_cast<double>(air.cloud)) * mass(layer);
    }
    CHECK(held + reached == Approx(start).epsilon(0.02));
}

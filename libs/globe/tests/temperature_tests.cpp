#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace hog::globe;
using Catch::Matchers::WithinAbs;

TEST_CASE("the baseline temperature is what the generator says at every point", "[globe][temperature]") {
    const Generator generator{Planet{}};
    const Planet& planet = generator.planet();
    for (int k = 0; k < 600; ++k) {
        const double z = 1.0 - ((2.0 * k + 1.0) / 600.0);
        const double r = std::sqrt(1.0 - (z * z));
        const hog::DVec3 d{r * std::cos(2.399963 * k), r * std::sin(2.399963 * k), z};
        const auto column = generator.column(d, 2.0);
        const double latitude = std::asin(z);
        for (const double offset : {-300.0, -20.0, -1.0, 1.0, 20.0, 400.0}) {
            const double radius = column.ground() + offset;
            const Sample sample = column.sample(radius);
            Surroundings surroundings = Surroundings::Open;
            if (sample.distance < 0.0) {
                surroundings = Surroundings::Inside;
            } else if (sample.water > 0.0) {
                surroundings = Surroundings::Water;
            }
            const double baseline = baseline_temperature(
                planet, generator.derived(), temperature_at(planet, latitude), radius,
                column.ground(), surroundings);
            CHECK_THAT(baseline, WithinAbs(sample.temperature, 1e-6));
        }
    }
}

TEST_CASE("it is colder the higher, and warmer the deeper", "[globe][temperature]") {
    const Generator generator{Planet{}};
    const Planet& planet = generator.planet();
    const double at_sea = temperature_at(planet, 0.3);
    const double ground = planet.radius + 100.0;
    const auto at = [&](double radius, Surroundings in) {
        return baseline_temperature(planet, generator.derived(), at_sea, radius, ground, in);
    };
    CHECK(at(ground + 5'000.0, Surroundings::Open) < at(ground + 50.0, Surroundings::Open));
    CHECK(at(ground - 1'000.0, Surroundings::Inside) > at(ground - 10.0, Surroundings::Inside));
    CHECK(at(ground - 10.0, Surroundings::Water) == at(ground - 1.0, Surroundings::Water));
    CHECK(climate_lapse_rate(planet) > 0.0);
}

#include <hog/atmosphere/model.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::atmosphere::Air;
using hog::atmosphere::Model;
using hog::atmosphere::Scattered;

namespace {

constexpr double degree = std::numbers::pi / 180.0;

const Model& earth() {
    static const Model model;
    return model;
}

// Unit, `elevation` above the horizon and `azimuth` round from +X.
DVec3 towards(double elevation, double azimuth = 0.0) {
    return {std::cos(elevation * degree) * std::cos(azimuth * degree),
            std::cos(elevation * degree) * std::sin(azimuth * degree),
            std::sin(elevation * degree)};
}

double brightness(DVec3 light) {
    return light.r + light.g + light.b;
}

} // namespace

TEST_CASE("straight up, the air's optical depth is its density summed", "[atmosphere]") {
    const Air air;
    const double thickness = air.top - air.bottom;
    const DVec3 expected =
        (air.rayleigh_scattering * air.rayleigh_height *
         (1.0 - std::exp(-thickness / air.rayleigh_height))) +
        DVec3{air.mie_extinction * air.mie_height * (1.0 - std::exp(-thickness / air.mie_height))} +
        (air.ozone_absorption * air.ozone_half_width);
    const DVec3 depth = hog::atmosphere::optical_depth(air, 0.0, 1.0);
    for (glm::length_t channel = 0; channel < 3; ++channel) {
        CHECK_THAT(depth[channel], WithinRel(expected[channel], 1e-4));
    }
}

TEST_CASE("tabulated transmittance matches the air marched finely", "[atmosphere]") {
    const Model& model = earth();
    for (const double height : {0.0, 400.0, 3'000.0, 20'000.0, 70'000.0}) {
        const double horizon =
            -std::sqrt(1.0 - std::pow(model.air().bottom / (model.air().bottom + height), 2.0));
        for (const double mu : {1.0, 0.6, 0.2, 0.05, 0.01, horizon + 1e-3}) {
            const DVec3 depth = hog::atmosphere::optical_depth(model.air(), height, mu);
            const DVec3 tabulated = model.transmittance(height, mu);
            INFO("height " << height << ", mu " << mu);
            for (glm::length_t channel = 0; channel < 3; ++channel) {
                CHECK_THAT(tabulated[channel], WithinAbs(std::exp(-depth[channel]), 5e-3));
            }
        }
    }
}

TEST_CASE("no light comes through the ground", "[atmosphere]") {
    CHECK(earth().transmittance(1'000.0, -0.5) == DVec3{0.0});
    CHECK(earth().transmittance(0.0, -0.01) == DVec3{0.0});
    CHECK(earth().transmittance(0.0, 0.01).r > 0.0);
}

TEST_CASE("the setting sun is reddened", "[atmosphere]") {
    const DVec3 noon = earth().transmittance(0.0, 1.0);
    const DVec3 setting = earth().transmittance(0.0, std::sin(1.0 * degree));
    CHECK(noon.b > 0.6);
    CHECK(setting.r > setting.g);
    CHECK(setting.g > setting.b);
    CHECK(setting.r / setting.b > 10.0 * noon.r / noon.b);
}

TEST_CASE("the noon sky is blue, and paler towards the horizon", "[atmosphere]") {
    const DVec3 sun = towards(90.0);
    const DVec3 zenith = earth().march(2.0, towards(90.0), sun).light;
    const DVec3 horizon = earth().march(2.0, towards(2.0), sun).light;
    CHECK(zenith.b > zenith.g);
    CHECK(zenith.g > zenith.r);
    CHECK(zenith.r > 0.0);
    CHECK(brightness(horizon) > brightness(zenith));
    CHECK(horizon.b / horizon.r < zenith.b / zenith.r);
}

TEST_CASE("the sky glows red round the setting sun", "[atmosphere]") {
    const DVec3 sun = towards(1.0);
    const DVec3 near_sun = earth().march(2.0, towards(3.0), sun).light;
    const DVec3 opposite = earth().march(2.0, towards(3.0, 180.0), sun).light;
    CHECK(near_sun.r > near_sun.b);
    CHECK(brightness(near_sun) > brightness(opposite));
}

TEST_CASE("after twilight the sky is dark", "[atmosphere]") {
    const DVec3 noon = earth().march(2.0, towards(90.0), towards(90.0)).light;
    const DVec3 night = earth().march(2.0, towards(90.0), towards(-12.0)).light;
    CHECK(brightness(night) < 1e-3 * brightness(noon));
}

TEST_CASE("multiple scattering lights the air most with the sun high", "[atmosphere]") {
    const DVec3 high = earth().multiple_scattering(1'000.0, 1.0);
    const DVec3 low = earth().multiple_scattering(1'000.0, 0.0);
    const DVec3 below = earth().multiple_scattering(1'000.0, -0.5);
    CHECK(low.b > 0.0);
    CHECK(brightness(high) > brightness(low));
    CHECK(brightness(low) > brightness(below));
}

TEST_CASE("nearby air hardly scatters or dims", "[atmosphere]") {
    const Scattered near = earth().march(2.0, towards(0.0), towards(45.0), 10.0);
    const Scattered far = earth().march(2.0, towards(0.0), towards(45.0), 50'000.0);
    CHECK(near.transmittance.b > 0.999);
    CHECK(brightness(near.light) < 1e-3 * brightness(far.light));
    CHECK(far.transmittance.b < 0.5);
    CHECK(far.transmittance.r > far.transmittance.b);
}

TEST_CASE("from above the air, only looking through it shows light", "[atmosphere]") {
    const DVec3 sun = towards(30.0);
    CHECK(earth().march(400'000.0, towards(90.0), sun).light == DVec3{0.0});
    const Scattered limb = earth().march(400'000.0, towards(-19.0), sun);
    CHECK(limb.light.b > 0.0);
    CHECK(limb.transmittance.b < 1.0);
}

TEST_CASE("the same air scaled to a smaller planet looks the same", "[atmosphere]") {
    const double radius = 115'000.0;
    const double scale = radius / earth().air().bottom;
    const Model small{earth().air().scaled_to(radius)};
    for (const double height : {2.0, 1'500.0, 30'000.0}) {
        for (const double sun : {60.0, 5.0, -2.0}) {
            for (const double view : {80.0, 10.0, 0.0, -20.0}) {
                const DVec3 big = earth().march(height, towards(view), towards(sun, 30.0)).light;
                const DVec3 scaled =
                    small.march(height * scale, towards(view), towards(sun, 30.0)).light;
                INFO("height " << height << ", sun " << sun << ", view " << view);
                for (glm::length_t channel = 0; channel < 3; ++channel) {
                    CHECK_THAT(scaled[channel], WithinAbs(big[channel], 1e-3 * brightness(big)));
                }
            }
        }
    }
}

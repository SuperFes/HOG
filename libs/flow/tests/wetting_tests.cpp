#include <hog/flow/wetting.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

using hog::flow::Sky;
using hog::flow::Soaking;
using hog::flow::Wetting;
using hog::globe::Material;
using hog::globe::Voxel;

namespace {

constexpr std::uint32_t top = 5; // the first open layer: layers under it are ground
constexpr double thickness = 1.0;
constexpr double step = 10.0;

// One column: `ground` below layer `top`, air over it, at a temperature.
struct Ground {
    std::array<Voxel, 12> voxels{};

    explicit Ground(Material ground, double kelvin = 293.15) {
        for (std::size_t layer = 0; layer < voxels.size(); ++layer) {
            if (layer < top) {
                voxels[layer].solid = ground;
            }
            voxels[layer].temperature = hog::globe::temperature_in_eighths(kelvin);
        }
    }
    [[nodiscard]] Voxel& bed() { return voxels[top - 1]; }
    [[nodiscard]] Voxel& film() { return voxels[top]; }

    // Runs `seconds` of `sky` in steps, applying what the ground is told.
    void weather(Sky sky, double seconds) {
        std::uint64_t draw = 0;
        const auto steps = static_cast<int>(std::ceil(seconds / step));
        for (int taken = 0; taken < steps; ++taken) {
            const auto read = [&](std::uint32_t layer) -> std::optional<Voxel> {
                return layer < voxels.size() ? std::optional{voxels[layer]} : std::nullopt;
            };
            for (const Soaking& change :
                 Wetting::column(read, {.face = 0, .i = 7, .j = 9, .layer = 0}, top + 2, thickness,
                                 sky, step, ++draw)) {
                voxels[change.site.layer].liquid_amount = change.amount;
                voxels[change.site.layer].liquid = change.liquid;
            }
        }
    }
};

// A depth of water in millimetres, as units of a voxel's volume.
double units(double millimetres) {
    return std::floor(millimetres / 1000.0 / thickness * Voxel::full);
}

} // namespace

TEST_CASE("light rain soaks into the ground and none lies on it", "[flow][wetting]") {
    Ground ground{Material::Soil};
    ground.weather({.rain = 5.0}, 3'600.0);
    CHECK(ground.film().liquid_amount == 0);
    CHECK(ground.bed().liquid == Material::Water);
    // An hour of 5 mm/h, to the dither's few units.
    CHECK(ground.bed().liquid_amount > units(5.0) - 12.0);
    CHECK(ground.bed().liquid_amount < units(5.0) + 12.0);
}

TEST_CASE("rain faster than the ground lets through lies on it as a film", "[flow][wetting]") {
    Ground ground{Material::Sediment};
    ground.weather({.rain = 20.0}, 3'600.0);
    // Clay takes about a third of a millimetre an hour; the rest stays on it.
    CHECK(ground.bed().liquid_amount < units(1.0));
    CHECK(ground.film().liquid_amount > units(15.0));
    CHECK(ground.film().liquid == Material::Water);
}

TEST_CASE("water in the ground stops at its pores, and what is over runs on as film",
          "[flow][wetting]") {
    Ground ground{Material::Soil};
    ground.bed().liquid = Material::Water;
    ground.bed().liquid_amount =
        hog::globe::amount_of(hog::globe::properties(Material::Soil).porosity);
    const std::uint16_t soaked = ground.bed().liquid_amount;
    ground.weather({.rain = 30.0}, 600.0);
    CHECK(ground.bed().liquid_amount == soaked);
    CHECK(ground.film().liquid_amount > 0);
}

TEST_CASE("warm dry air takes the film and then the ground's water; cold or damp air doesn't",
          "[flow][wetting]") {
    const auto wet = [](double kelvin) {
        Ground ground{Material::Soil, kelvin};
        ground.film().liquid = Material::Water;
        ground.film().liquid_amount = static_cast<std::uint16_t>(units(0.1));
        ground.bed().liquid = Material::Water;
        ground.bed().liquid_amount = static_cast<std::uint16_t>(units(10.0));
        return ground;
    };
    Ground warm = wet(293.15);
    warm.weather({.dryness = 1.0}, 3'600.0);
    CHECK(warm.film().liquid_amount == 0);
    CHECK(warm.film().liquid == Material::Air);
    CHECK(warm.bed().liquid_amount < units(10.0));
    CHECK(warm.bed().liquid_amount > units(10.0) - units(Wetting::drying_peak * 4.0));

    Ground cold = wet(268.15);
    cold.weather({.dryness = 1.0}, 3'600.0);
    CHECK(cold.bed().liquid_amount == units(10.0));
    CHECK(cold.film().liquid_amount == static_cast<std::uint16_t>(units(0.1)));

    Ground damp = wet(293.15);
    damp.weather({.dryness = 0.0}, 3'600.0);
    CHECK(damp.bed().liquid_amount == units(10.0));
}

TEST_CASE("ground dries right out and no further", "[flow][wetting]") {
    Ground ground{Material::Soil};
    ground.bed().liquid = Material::Water;
    ground.bed().liquid_amount = static_cast<std::uint16_t>(units(3.0));
    ground.weather({.dryness = 1.0}, 24.0 * 3'600.0);
    CHECK(ground.bed().liquid_amount == 0);
    CHECK(ground.bed().liquid == Material::Air);
}

TEST_CASE("standing water and other liquids are left alone", "[flow][wetting]") {
    Ground pond{Material::Soil};
    pond.film().liquid = Material::Water;
    pond.film().liquid_amount = Voxel::full / 2;
    pond.weather({.dryness = 1.0}, 3'600.0);
    CHECK(pond.film().liquid_amount == Voxel::full / 2);

    Ground melt{Material::Basalt};
    melt.bed().liquid = Material::Basalt;
    melt.bed().liquid_amount = 100;
    melt.weather({.rain = 50.0, .dryness = 1.0}, 3'600.0);
    CHECK(melt.bed().liquid == Material::Basalt);
    CHECK(melt.bed().liquid_amount == 100);
}

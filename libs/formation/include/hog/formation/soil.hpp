#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/drifts.hpp>
#include <hog/formation/ice.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sediment.hpp>
#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <vector>

namespace hog::formation {

// What soil forms from.
enum class Parent : std::uint8_t {
    None,     // under ice, standing water or salt: no soil forms
    Rock,     // the rock itself
    Till,     // what ice laid down
    Alluvium, // what rivers laid down
    LakeBed,  // what settled in a hollow, wet or dry
    Loess,    // silt the wind laid
    Sand,     // sand the wind laid
};

// How deep soil has grown over the time it has had (Heimsath's soil production: what it forms from
// turns to soil fastest when bare and slower under deeper soil, while the ground wears away).
// It forms down from the top of what was laid: through what the wind laid last, loess over sand,
// then the rivers' alluvium or the lake beds, then the till ice laid before them, then the rock.
// Plants darken its top with humus from their dying roots, and what dies above ground lies on it
// as litter, or as peat where waterlogged ground keeps the air from rotting it.
struct Soil {
    std::vector<double> depth;     // metres of soil, topsoil and subsoil
    std::vector<double> topsoil;   // metres of its top darkened with humus
    std::vector<double> organic;   // metres of litter or peat lying on it
    std::vector<double> loess;     // metres of silt the wind laid, left under the soil
    std::vector<double> sand;      // metres of sand the wind laid, left under the loess
    std::vector<double> alluvium;  // metres of what rivers laid, left under the soil
    std::vector<double> lake_beds; // metres of what settled in hollows, left under the soil
    std::vector<double> till;      // metres of what ice laid, left under those
    std::vector<double> making;    // metres a year its bare parent turns to soil there
    std::vector<double> years;     // years it has had to grow
    std::vector<Parent> parent;
};

// What lies at a depth under the ground, from the top down.
enum class Layer : std::uint8_t {
    Organic,
    Topsoil,
    Subsoil,
    Loess,
    Sand,
    Alluvium,
    LakeBed,
    Till,
    Rock
};

// How thick each layer is anywhere on the sphere, read between the points that hold them.
struct Profile {
    double organic = 0.0;
    double topsoil = 0.0;
    double subsoil = 0.0;
    double loess = 0.0;
    double sand = 0.0;
    double alluvium = 0.0;
    double lake_beds = 0.0;
    double till = 0.0;
};

// Metres a year bare rock turns to soil: rotting faster where it's warm and wet, shattered by frost
// a little under freezing, neither where it's frozen through.
[[nodiscard]] double soil_making(double celsius, double rain, const Body& body);
// The depth at which soil made under it matches `wear`; none where wear outpaces making.
[[nodiscard]] double balanced_depth(double making, double wear, const Body& body);
// The depth soil grows to from bare ground over `years`, tending to the balanced depth.
[[nodiscard]] double grown_depth(double making, double wear, double years, const Body& body);

// Kilograms a square metre plants grow a year where they stand, by the lesser of what warmth and
// rain allow (Lieth's Miami model), only while the ground is thawed.
[[nodiscard]] double plant_growth(double celsius, double rain, const Body& body);
// Rain against what the air would take off open water: over 1, more falls than can dry.
[[nodiscard]] double wetness(double celsius, double rain, const Body& body);
// Share of the ground waterlogged, so that air can't reach what dies in it.
[[nodiscard]] double waterlogged(double wetness);
// Per year humus rots in the soil: faster warm, slower dry, frozen or waterlogged.
[[nodiscard]] double rotting_rate(double celsius, double wetness, const Body& body);
// What a store fed `gain` a year and losing `rate` of itself a year holds after `years`, from none.
[[nodiscard]] double gathered(double gain, double rate, double years);

// Soil on rock alone, as though nothing had been laid down and no ice had ever lain.
[[nodiscard]] Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate,
                           const Body& body);
// Soil on rock or on what ice and rivers laid over it, none under present ice or standing water.
[[nodiscard]] Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate,
                           const Ice& ice, const Lakes& lakes, const Sediment& sediment,
                           const Body& body);
// The same with what the wind laid over all that, and took off it.
[[nodiscard]] Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate,
                           const Ice& ice, const Lakes& lakes, const Sediment& sediment,
                           const Drifts& drifts, const Body& body);

[[nodiscard]] Profile profile_at(const Sphere& sphere, const Soil& soil, const Sphere::Spot& spot);
// The layer `depth` metres under the top of the ground; rock under everything laid.
[[nodiscard]] Layer layer_at(const Profile& profile, double depth);

} // namespace hog::formation

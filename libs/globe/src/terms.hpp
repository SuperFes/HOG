#pragma once

#include <hog/globe/generator.hpp>
#include <hog/globe/noise.hpp>

#include <cstdint>

// The generator's terms, one file each. Heights are metres over the planet's raw radius, as
// functions of direction, their gradients taken against the point on the sphere at that radius.
namespace hog::globe::terms {

// Seeds for each term's noise, fixed forever.
enum class Term : std::uint8_t {
    Province = 1,
    OneSide,
    Belts,
    Peaks,
    Hills,
    Ridges,
    Gullies,
    Craters,
};

struct Context {
    const Planet* planet;
    const Derived* derived;
    const Generator::Shape* shape;

    [[nodiscard]] std::uint64_t seed(Term term) const noexcept;
};

[[nodiscard]] DVec3 tangential(DVec3 gradient, DVec3 direction) noexcept;

[[nodiscard]] double smoothstep(double x) noexcept; // 0 below 0, 1 above 1
[[nodiscard]] double smoothstep_slope(double x) noexcept;

// The crustal province field, before its threshold: the high crust stands where it's above.
[[nodiscard]] Noise province_field(const Context& context, DVec3 direction) noexcept;

// Metres across the margin between the provinces, as wide as the isostatic step's slope lets it.
[[nodiscard]] double margin(const Context& context) noexcept;

// How much of the high crust is here, from 0 to 1.
struct Province {
    double high = 0.0;
    DVec3 gradient{0.0};
};

[[nodiscard]] Province province(const Context& context, DVec3 direction) noexcept;

// Mountains, hills and ocean ridges, by the regime and the erosion at work.
[[nodiscard]] Noise relief(const Context& context, DVec3 direction, const Province& crust,
                           double lod) noexcept;

// One width of gullies, cut straight down `slope` (the ground's, coarser than them), as deep as
// `share` of the fall across their width, faded in by `weight`.
[[nodiscard]] Noise gullies(const Context& context, DVec3 direction, DVec3 slope, double wavelength,
                            int octave, double share, double weight) noexcept;

// A fresh crater's height over the ground it struck, `x` radii from its middle, and its slope per
// radius: a bowl below `transition` across, a flat floor and a central peak above it.
struct CraterProfile {
    double height = 0.0;
    double slope = 0.0;
};

[[nodiscard]] CraterProfile crater_profile(double x, double diameter, double transition) noexcept;

// Craters over `diameter` metres across per km² that the surface keeps, before wear.
[[nodiscard]] double craters_over(const Planet& planet, double diameter) noexcept;

// Every crater's shape over the ground, worn by its age.
[[nodiscard]] Noise craters(const Context& context, DVec3 direction, double lod) noexcept;

// The ground's raw height along a direction, the sum of every column term.
struct Column {
    Noise height;
    double high = 0.0; // share of high crust
};

[[nodiscard]] Column column(const Context& context, DVec3 direction, double lod) noexcept;

} // namespace hog::globe::terms

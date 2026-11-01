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

// The ground's raw height along a direction, the sum of every column term.
struct Column {
    Noise height;
    double high = 0.0; // share of high crust
};

[[nodiscard]] Column column(const Context& context, DVec3 direction, double lod) noexcept;

} // namespace hog::globe::terms

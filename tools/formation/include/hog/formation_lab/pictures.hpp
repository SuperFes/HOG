#pragma once

#include <hog/core/image.hpp>
#include <hog/core/math.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <functional>
#include <span>

namespace hog::formation_lab {

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// Precondition: each channel within 0 to 255.
[[nodiscard]] constexpr Rgb rgb(int r, int g, int b) noexcept {
    return {.r = static_cast<std::uint8_t>(r),
            .g = static_cast<std::uint8_t>(g),
            .b = static_cast<std::uint8_t>(b)};
}

// The direction from the planet's middle through a map's pixel: longitude across from -180° at
// the left, latitude down from 90° at the top.
[[nodiscard]] DVec3 map_direction(int x, int y, int width, int height);

// A whole-planet map `width` across and half as high, each pixel coloured by `colour` of its
// direction. Pixels are coloured in reading order, so `colour` can walk on from the last one.
[[nodiscard]] Image map_picture(int width, const std::function<Rgb(const DVec3&)>& colour);

// Draws the great-circle stretch from `from` to `to` onto a whole-planet map, `width` pixels wide,
// wrapping round its sides.
void draw_line(Image& image, const DVec3& from, const DVec3& to, Rgb colour, int width);

// Along `stops`, evenly spaced over 0 to 1.
[[nodiscard]] Rgb ramp(double t, std::span<const Rgb> stops);
// Sea blues by depth, land from lowland green through brown to snowy white by height.
[[nodiscard]] Rgb height_colour(double metres);
[[nodiscard]] Rgb plate_colour(std::uint16_t plate);
[[nodiscard]] Rgb meeting_colour(formation::Meeting meeting);
[[nodiscard]] Rgb scaled(Rgb colour, double by);

// Light falling from the north-west on a height field read smoothly over the sphere: 1 on flat
// ground, more on slopes facing the light, less facing away. `step` is radians between samples.
[[nodiscard]] double shade(const formation::Sphere& sphere, std::span<const double> heights,
                           const DVec3& direction, double radius, double step,
                           double exaggeration, std::uint32_t& start);

} // namespace hog::formation_lab

#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/pockets.hpp>
#include <hog/render/camera.hpp>
#include <hog/terrain/bend.hpp>

#include <span>
#include <string>
#include <vector>

namespace hog::client {

// How far a ping looks, and how far apart its casts are, in metres.
inline constexpr double ping_range = 300.0;
inline constexpr double ping_spacing = 6.0;
// The nearest pockets a ping shows.
inline constexpr std::size_t ping_shown = 8;

// A pocket marked over the picture: where, and what it says of it.
struct Blip {
    std::string text;
    DVec2 at;        // over the middle of the text, from the top left of the picture
    bool on_screen;  // else it's held to the picture's edge, towards where the pocket lies
    double distance; // metres from the eye
};

// Where the nearest `ping_shown` of the pockets are marked over a picture of `size` seen by
// `camera`, bent as drawn, furthest first so the nearest are drawn over them. Those out of the
// picture or behind the eye are held `margin` pixels in from its edge, towards where they lie,
// with an arrow showing which way.
[[nodiscard]] std::vector<Blip> blips(std::span<const globe::Pocket> pockets,
                                      const render::Camera& camera, const terrain::Bend& bend,
                                      DVec2 size, double margin);

} // namespace hog::client

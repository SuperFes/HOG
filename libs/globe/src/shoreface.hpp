#pragma once

#include <hog/globe/material.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

namespace hog::globe {

// A beach's shape across the shore once the waves have worked it to their liking: a face rising
// from the water at the slope the swash leaves (Sunamura 1984), a berm on top as high as an
// ordinary day's waves run up (Stockdon et al. 2006), and under the water Dean's shoreface (Dean
// 1991, with the face's slope at the waterline) out to where storms stop moving the bottom
// (Hallermeier 1981), past which the sand lies at the steepest it holds soaked. Bars stand where
// the waves break (Short and Aagaard 1993), cut crescent where the beach is between steep and flat
// (Wright and Short 1984).

// What the sea sends in on a coast: storms' and an ordinary day's significant wave heights,
// metres, and the swell's period, seconds.
struct Sea {
    double storm = 0.0;
    double usual = 0.0;
    double period = 0.0;
};

// What of a loose material's grains shapes the beach they make.
struct Grains {
    double size = 0.0;     // metres across
    double settling = 0.0; // metres a second they sink through still water
    double scale = 0.0;    // Dean's shoreface scale, metres to the third
    double steepest = 0.0; // rise over run they lie at soaked
    double dry = 0.0;      // and dry
};

// The grains of `material`, their size as sinks as fast as its own (Ferguson and Church 2004).
[[nodiscard]] Grains grains_of(Material material, double gravity);
// A mix of grains by shares summing to 1.
[[nodiscard]] Grains mixed(std::span<const Grains> grains, std::span<const double> shares);

// The rise over run of the face the swash leaves on a beach of `grains`, the flatter the bigger
// an ordinary day's waves and the finer the grains.
[[nodiscard]] double face_slope(const Sea& sea, const Grains& grains, double gravity);
// Metres over the sea an ordinary day's waves run up a face rising `face` over run, as high as
// they build the berm; storms cut it back rather than build it.
[[nodiscard]] double runup(const Sea& sea, double face, double gravity);
// Metres down storms' waves stop moving the bottom.
[[nodiscard]] double closure_depth(const Sea& sea, double gravity);

// A beach across the shore, distances out to sea from its waterline.
struct Profile {
    double scale = 0.0;   // Dean's, metres to the third
    double face = 0.0;    // rise over run at the waterline
    double berm = 0.0;    // metres its top stands over the sea
    double closure = 0.0; // metres down its shoreface reaches
    double toe = 0.0;     // rise over run past that
    double back = 0.0;    // rise over run its berm falls away inland where the ground lies low
};
[[nodiscard]] Profile profile_for(const Sea& sea, const Grains& grains, double gravity);

// A surface's height over the sea, metres, and its rise per metre further out.
struct Across {
    double height = 0.0;
    double slope = 0.0;
};
[[nodiscard]] Across across(const Profile& profile, double out);
// Metres out from the waterline its shoreface is `depth` metres down, from none to its closure.
[[nodiscard]] double out_to(const Profile& profile, double depth);

// Ground under the beach as a plane falling `slope` metres a metre out to sea, until it lies
// `floor` metres down.
struct Ground {
    double slope = 0.0;
    double floor = 0.0;
};
// Cubic metres a metre of shore holds between the ground and a profile whose waterline is
// `ahead` metres out from the ground's, no higher than `top`.
[[nodiscard]] double held(const Profile& profile, const Ground& ground, double ahead,
                          double top = std::numeric_limits<double>::infinity());
// Where a beach of `volume` cubic metres a metre of shore lies on the ground; none without any,
// or without sea floor to lie on. What there is gathers at the ground's foot and fills up to
// `top`, until it comes up to the beach's berm. Then the waves build its face out from the
// waterline `ahead` metres until it stands whole, take its shoreface `down` as far as there's
// sand for before its toe falls to the ground, and with plenty build the whole of it out.
struct Built {
    double ahead = 0.0;
    double down = 0.0;
    double top = std::numeric_limits<double>::infinity();
};
[[nodiscard]] std::optional<Built> built_on(const Profile& profile, const Ground& ground,
                                            double volume);
// The profile as built, its shoreface going only as far down as there was sand for.
[[nodiscard]] Profile as_built(const Profile& profile, const Built& built);
// The top of the beach `out` metres out from the ground's waterline, its toe running on down to
// whatever ground it meets. Inland of where the plane rises over its berm, its berm falls away to
// ground lying lower.
[[nodiscard]] std::optional<Across> beach_top(const Profile& profile, const Ground& ground,
                                              const Built& built, double out);

// The bars along a beach, outermost last, metres out from its waterline.
struct Bars {
    static constexpr int most = 4;
    int count = 0;
    std::array<double, most> crest{};  // how far out each crest is
    std::array<double, most> width{};  // and how far its trough lies inshore of it
    std::array<double, most> relief{}; // metres from trough to crest
    double rhythm = 0.0;               // 0 to 1: how far along the shore they're cut crescent
};
[[nodiscard]] Bars bars_on(const Profile& profile, const Sea& sea, const Grains& grains,
                           double gravity);
// What the bars add to the profile `out` metres out from the waterline, crests and troughs
// shifted `shift` of their width out to sea.
[[nodiscard]] Across bars_at(const Bars& bars, double out, double shift);

// The ground across a shore: its heights over the sea at distances out from its waterline,
// never rising seaward, and past its ends falling on as its last stretch does.
struct Section {
    static constexpr std::size_t samples = 33;
    std::array<double, samples> out{};
    std::array<double, samples> height{};
};
// How far out along it the ground is `height` metres high, and how much further out a metre lower.
[[nodiscard]] Across out_at(const Section& section, double height);
// Where a beach of `volume` lies on a shore's ground itself, as `built_on` a plane; none without
// any, or without sea off it.
[[nodiscard]] std::optional<Built> built_along(const Profile& profile, const Section& section,
                                               double volume);
// The top of a beach built along a shore's ground `out` metres out from its waterline.
[[nodiscard]] Across top_along(const Profile& profile, const Section& section, const Built& built,
                               double out);

// A beach built along its shore's ground and read by the height of that: so beaches built about
// a shore blend wherever the ground is that high, each keeping its own slopes across it.
struct Laid {
    Profile profile;
    Built built;
    Bars bars;
    Section section;
};
// How thick a beach lies over ground `height` metres over the sea, and how much thicker a metre
// higher, its bars shifted `shift` of their width out to sea; none where it doesn't reach.
[[nodiscard]] Across lying_over(const Laid& laid, double height, double shift);

} // namespace hog::globe

#pragma once

#include <hog/flora/habitat.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace hog::flora {

// Kinds of tree by how they live; each species belongs to one.
enum class Kind : std::uint8_t {
    Needleleaf, // evergreen conifers, hardy to long cold
    Deciduous,  // broadleaves dropping their leaves for a cold season
    Evergreen,  // broadleaves of warm, wet woods with no winter
    Dry,        // small-leaved, thorny trees standing apart in dry heat
    Palm,       // warm and frost-free, most of all where water's near
    Wetland,    // standing with wet feet: willow, alder, swamp cypress, mangrove
};
inline constexpr std::size_t kinds = 6;

// The woods at a place: the share of the ground under crowns, and each kind's share of them,
// summing to one where any stand.
struct Woods {
    double cover = 0.0;
    std::array<double, kinds> shares{};

    [[nodiscard]] double share(Kind kind) const noexcept {
        return shares.at(static_cast<std::size_t>(kind));
    }
};

// The year's warmth as plants feel it, from a habitat's yearly mean and swing, its months taken
// as a cosine through the year: Holdridge's biotemperature, the mean of the months' temperatures
// held between 0 and 30 °C; the share of the year warm enough to grow in, the months over 0.9 °C,
// and their mean, against which Körner found treelines stand at about 6.4 °C; and the coldest
// month.
struct Warmth {
    double biotemperature = 0.0;
    double growing = 0.0; // share of the year
    double growing_mean = 0.0;
    double coldest = 0.0;
};
[[nodiscard]] Warmth warmth_of(const Habitat& habitat) noexcept;

// How far the water under the ground lies within the roots' reach, 0 to 1: where the ground
// stands a few metres over a drainage, and rain or a river running the year round keeps the
// water up. A dry wash's lies far deeper.
[[nodiscard]] double groundwater_of(const Habitat& habitat) noexcept;
// What water the habitat gives a tree in a year against what the warmth would take: its rain over
// Holdridge's potential evapotranspiration, 58.93 mm a year per degree of biotemperature, or as
// much as the groundwater in reach gives.
[[nodiscard]] double moisture_of(const Habitat& habitat, const Warmth& warmth) noexcept;

// The woods a habitat grows. Cover is the least that each limit lets stand: too little warmth to
// grow in (under the treeline), too little water, thin soil and bare rock, steep ground, ice, and
// open water too deep to stand in. Each kind's share is how well the habitat suits it against how
// well it suits the others, every range fading rather than stopping.
[[nodiscard]] Woods woods_of(const Habitat& habitat) noexcept;

} // namespace hog::flora

#pragma once

#include <hog/core/math.hpp>

namespace hog::terrain {

// What a body of water carries, which decides how clear it is and what colour it turns.
struct Carried {
    double organics = 0.0; // dissolved organic matter, by what it absorbs at 440 nm, per metre
    double sediment = 0.0; // suspended, grams to the cubic metre
    double plankton = 0.0; // by its chlorophyll, milligrams to the cubic metre
    double flour = 0.0;    // the share of the sediment that's fine rock flour, not silt
};

// The open sea, far from any river: a little plankton and what it leaves dissolved. As
// shaders/water.slang has it; change both together.
inline constexpr Carried open_sea{.organics = 0.01, .sediment = 0.05, .plankton = 0.07};

// Of water, by the metre and by red, green and blue light: the share lost of the light falling
// through it, and of what's seen through it, which its scattering also blurs away; and of the
// light falling on it, what deep water of it scatters back out.
struct Optics {
    DVec3 dimming{0.0};
    DVec3 hiding{0.0};
    DVec3 colour{0.0};
};

// From what's absorbed and scattered by water itself and by what it carries, each as measured
// on Earth, as shaders/water.slang has it; change both together.
[[nodiscard]] Optics optics(const Carried& carried);

} // namespace hog::terrain

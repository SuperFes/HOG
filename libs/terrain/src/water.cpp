#include <hog/terrain/water.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::terrain {

namespace {

// Each by red, green and blue light, taken at 610, 550 and 470 nm, per metre: water itself
// (Pope and Fry; Morel), which scatters as much back as on.
constexpr DVec3 pure_absorbing{0.25, 0.0565, 0.0106};
constexpr DVec3 pure_scattering{0.00122, 0.00191, 0.00376};
// Dissolved organics, per unit absorbed at 440 nm, falling off exponentially into the red.
constexpr DVec3 organics_absorbing{0.0781, 0.192, 0.6376};
// Plankton, per mg of chlorophyll to the power (Bricaud; Gordon and Morel).
constexpr DVec3 plankton_absorbing{0.0065, 0.0060, 0.045};
constexpr DVec3 plankton_absorbing_power{0.85, 0.85, 0.75};
constexpr DVec3 plankton_scattering{0.2705, 0.3, 0.3511};
constexpr double plankton_scattering_power = 0.62;
constexpr double plankton_backward = 0.01;
// Per gram, brown silt washed off the land and grey rock flour ground fine by glaciers, which
// scatters more, and more of the blue, and absorbs less (Babin).
constexpr DVec3 silt_absorbing{0.0051, 0.0107, 0.0287};
constexpr DVec3 silt_scattering{0.4793, 0.51, 0.5604};
constexpr double silt_backward = 0.019;
constexpr DVec3 flour_absorbing{0.0019, 0.0040, 0.0108};
constexpr DVec3 flour_scattering{0.8832, 1.0, 1.2076};
constexpr double flour_backward = 0.03;
// Of the light scattered forwards, the share that blurs what's seen through the water out of a
// pixel; the rest only moves it within one.
constexpr double blurring = 0.2;
// How much more clear water scatters back than water does, brightening the deep clear blue the
// sea's drawn darker than it looks; the light scattered by what water carries outweighs it.
constexpr double clear_brightening = 16.0;

// Of light scattered back up out of deep water, the share leaving it, by how much the water
// scatters back against all it takes out of the light (Gordon; Lee).
double leaving(double backward) {
    const double under = (0.0949 * backward) + (0.0794 * backward * backward);
    return std::numbers::pi * 0.52 * under / (1.0 - (1.7 * under));
}

} // namespace

Optics optics(const Carried& carried) {
    const double plankton = std::max(carried.plankton, 0.0);
    const double flour = std::clamp(carried.flour, 0.0, 1.0);
    const DVec3 sediment_scattering =
        carried.sediment * ((silt_scattering * (1.0 - flour)) + (flour_scattering * flour));
    const DVec3 by_plankton = plankton_scattering * std::pow(plankton, plankton_scattering_power);
    const DVec3 absorbing =
        pure_absorbing + (carried.organics * organics_absorbing) +
        (plankton_absorbing * glm::pow(DVec3{plankton}, plankton_absorbing_power)) +
        (carried.sediment * ((silt_absorbing * (1.0 - flour)) + (flour_absorbing * flour)));
    const DVec3 scattering = pure_scattering + by_plankton + sediment_scattering;
    const DVec3 backward = (pure_scattering / 2.0) + (plankton_backward * by_plankton) +
                           (carried.sediment * ((silt_scattering * silt_backward * (1.0 - flour)) +
                                                (flour_scattering * flour_backward * flour)));
    const DVec3 brightened = backward + ((clear_brightening - 1.0) * pure_scattering / 2.0);
    const DVec3 share = brightened / (absorbing + brightened);
    return {.dimming = absorbing + backward,
            .hiding = absorbing + backward + (blurring * (scattering - backward)),
            .colour = {leaving(share.x), leaving(share.y), leaving(share.z)}};
}

} // namespace hog::terrain

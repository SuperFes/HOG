#include <hog/globe/weathering.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace hog::globe {
namespace {

// How a rock weathers: how deep beside a middling rock, and what its fines come to.
struct Rotting {
    double deep = 1.0;
    Material fines = Material::Clay;
};

std::optional<Rotting> rotting(Material rock) {
    switch (rock) {
    // Feldspar rots and the quartz grains stay: grus.
    case Material::Granite:
    case Material::Anorthosite:
        return Rotting{.deep = 1.0, .fines = Material::Sand};
    case Material::Sandstone:
        return Rotting{.deep = 0.8, .fines = Material::Sand};
    case Material::Shale:
        return Rotting{.deep = 1.3, .fines = Material::Clay};
    case Material::Basalt:
        return Rotting{.deep = 1.2, .fines = Material::Clay};
    case Material::Mantle:
        return Rotting{.deep = 1.0, .fines = Material::Clay};
    // Most of it dissolves away, leaving its few impurities as red clay.
    case Material::Limestone:
        return Rotting{.deep = 0.5, .fines = Material::Clay};
    default:
        return std::nullopt;
    }
}

} // namespace

Material Horizons::at(double below) const noexcept {
    if (below < topsoil) {
        return top;
    }
    if (below < subsoil) {
        return under;
    }
    if (below < depth) {
        return broken;
    }
    return Material::Air;
}

Horizons horizons(Material rock, const Climate& climate, double depth) {
    const std::optional<Rotting> how = rotting(rock);
    if (!how || depth <= 0.0) {
        return {};
    }
    if (!climate.liquid) {
        return {.depth = depth, .broken = Material::Regolith};
    }
    // Under this the ground never thaws deep enough for rock to rot.
    constexpr double frozen = 263.0;
    constexpr double temperate = 288.0;
    constexpr double middling_rain = 0.8;
    // Too dry to grow enough to make topsoil.
    constexpr double desert = 0.25;
    const double wet = std::clamp(climate.rain / middling_rain, 0.25, 2.0);
    const double rot = climate.kelvin < frozen
                           ? 0.0
                           : std::min(3.0, wet * std::exp2((climate.kelvin - temperate) / 10.0));
    const double scaled = depth * how->deep;
    Horizons made{.under = how->fines};
    made.top = climate.rain < desert ? Material::Sand : Material::Soil;
    made.topsoil = scaled * 0.15 * std::min(rot, 1.5);
    made.subsoil = made.topsoil + (scaled * 0.35 * rot);
    made.depth = made.subsoil + (scaled * 0.5);
    return made;
}

} // namespace hog::globe

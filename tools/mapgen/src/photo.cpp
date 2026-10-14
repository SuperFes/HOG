#include <hog/mapgen/photo.hpp>
#include <hog/sim/weather_tuning.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace hog::mapgen {

namespace {

// How a land biome looks with nothing growing, and fully grown.
struct Look {
    Rgb bare{};
    Rgb grown{};
    double cover = 0.0;  // share of the ground its plants hide when grown
    double canopy = 0.0; // share of its plants standing above lying snow
};

// Indexed by biome; the sea and lakes have their own colours.
constexpr std::array<Look, planet::biome_count> looks{{
    {.bare = {}, .grown = {}},                                                    // ocean
    {.bare = {215, 200, 160}, .grown = {150, 150, 100}, .cover = 0.1},            // beach
    {.bare = {225, 232, 240}, .grown = {225, 232, 240}},                          // ice
    {.bare = {125, 115, 95}, .grown = {115, 120, 80}, .cover = 0.6},              // tundra
    {.bare = {95, 85, 65}, .grown = {30, 55, 35}, .cover = 0.95, .canopy = 0.5},  // boreal
    {.bare = {110, 95, 70}, .grown = {45, 85, 40}, .cover = 0.95, .canopy = 0.4}, // temperate
    {.bare = {150, 130, 95}, .grown = {105, 130, 60}, .cover = 0.85},             // grassland
    {.bare = {170, 145, 105}, .grown = {110, 115, 65}, .cover = 0.55},            // shrubland
    {.bare = {205, 175, 125}, .grown = {150, 140, 90}, .cover = 0.1},             // desert
    {.bare = {180, 150, 100}, .grown = {140, 135, 65}, .cover = 0.6},             // savanna
    {.bare = {110, 85, 60}, .grown = {25, 70, 30}, .cover = 1.0, .canopy = 0.5},  // rainforest
    {.bare = {125, 120, 115}, .grown = {100, 110, 80}, .cover = 0.35},            // alpine
    {.bare = {85, 80, 60}, .grown = {55, 85, 55}, .cover = 0.85, .canopy = 0.2},  // wetland
    {.bare = {}, .grown = {}},                                                    // lake
}};

constexpr std::array sea_stops{
    ColourStop{.at = -4'000.0, .colour = {6, 18, 45}},
    ColourStop{.at = -1'000.0, .colour = {12, 35, 75}},
    ColourStop{.at = -100.0, .colour = {25, 75, 110}},
    ColourStop{.at = -10.0, .colour = {60, 130, 140}},
    ColourStop{.at = 0.0, .colour = {95, 160, 155}},
};

constexpr Rgb sea_ice{215, 225, 235};
constexpr Rgb lake_water{25, 55, 75};
constexpr Rgb river_water{30, 60, 80};
constexpr Rgb flood_brown{120, 100, 70};
constexpr Rgb snow_white{240, 244, 250};
constexpr Rgb glacier_ice{195, 212, 230};
constexpr Rgb char_black{35, 30, 28};
constexpr Rgb ember_orange{230, 90, 20};

// Wet soil reflects this much less than dry.
constexpr double wet_darkening = 0.4;
// Flow over its usual at which a river runs fully brown.
constexpr double full_flood = 5.0;

LinearRgb blend(LinearRgb from, LinearRgb to, double share) {
    return glm::mix(from, to, std::clamp(share, 0.0, 1.0));
}

LinearRgb flowing(LinearRgb ground, const Surface& surface) {
    if (surface.river <= 0.0) {
        return ground;
    }
    const double flow = std::max(surface.flow, 0.0);
    // A river runs narrower through a drought, and muddier in flood.
    const double share = surface.river * std::min(0.3 + (0.7 * flow), 1.0);
    const LinearRgb water =
        blend(to_linear(river_water), to_linear(flood_brown), (flow - 1.0) / (full_flood - 1.0));
    return blend(ground, water, share);
}

LinearRgb snowed(LinearRgb ground, double snow, double standing) {
    const sim::LandTuning tuning{};
    const double cover = snow / (snow + tuning.covering_snow) * (1.0 - standing);
    const LinearRgb white =
        blend(to_linear(snow_white), to_linear(glacier_ice), snow / tuning.glacier);
    return blend(ground, white, cover);
}

} // namespace

LinearRgb true_colour(const Surface& surface) noexcept {
    const double snow = std::max(surface.snow, 0.0);
    switch (surface.biome) {
    case planet::Biome::Ocean:
        return blend(to_linear(ramp(sea_stops, -surface.depth)), to_linear(sea_ice), surface.ice);
    case planet::Biome::Lake:
        return snowed(to_linear(lake_water), snow, 0.0);
    default:
        break;
    }
    const Look& look = looks.at(static_cast<std::size_t>(surface.biome));
    const double plants = std::clamp(surface.plants, 0.0, 1.0);
    const LinearRgb earth =
        to_linear(look.bare) * (1.0 - (wet_darkening * std::clamp(surface.soil, 0.0, 1.0)));
    const LinearRgb grown = blend(earth, to_linear(look.grown), look.cover * plants);
    const double fire = std::clamp(surface.fire, 0.0, 1.0);
    const LinearRgb burning = blend(blend(grown, to_linear(char_black), 2.0 * fire),
                                    to_linear(ember_orange), 0.5 * std::sqrt(fire));
    return snowed(flowing(burning, surface), snow, look.canopy * plants);
}

DVec3 sun_towards(const Lighting& lighting, const planet::TangentFrame& frame) noexcept {
    const double elevation = glm::radians(lighting.sun_elevation);
    const double azimuth = glm::radians(lighting.sun_azimuth);
    return frame.to_world({std::cos(elevation) * std::sin(azimuth),
                           std::cos(elevation) * std::cos(azimuth), std::sin(elevation)});
}

DVec3 surface_normal(DVec3 left, DVec3 right, DVec3 below, DVec3 above, DVec3 up) noexcept {
    const DVec3 normal = glm::cross(right - left, above - below);
    const double length = glm::length(normal);
    if (length <= 0.0) {
        return up;
    }
    return glm::dot(normal, up) < 0.0 ? -normal / length : normal / length;
}

double light(DVec3 normal, DVec3 up, DVec3 sun, double sky, bool shadowed) noexcept {
    const double direct = shadowed ? 0.0 : std::max(glm::dot(normal, sun), 0.0);
    return direct + (sky * 0.5 * (1.0 + glm::dot(normal, up)));
}

double exposure(const Lighting& lighting) noexcept {
    return std::max(std::sin(glm::radians(lighting.sun_elevation)), 0.0) + lighting.sky;
}

bool in_shadow(const HeightField& field, int x, int y, DVec2 step, double rise,
               int steps) noexcept {
    const auto at = [&](int column, int row) {
        return field
            .heights[(static_cast<std::size_t>(row) * static_cast<std::size_t>(field.width)) +
                     static_cast<std::size_t>(column)];
    };
    const double ground = at(x, y);
    for (int taken = 1; taken <= steps; ++taken) {
        const DVec2 along = DVec2{x + 0.5, y + 0.5} + (step * static_cast<double>(taken));
        const auto column = static_cast<int>(std::floor(along.x));
        const auto row = static_cast<int>(std::floor(along.y));
        if (column < 0 || row < 0 || column >= field.width || row >= field.height) {
            return false;
        }
        if (at(column, row) > ground + (rise * static_cast<double>(taken))) {
            return true;
        }
    }
    return false;
}

} // namespace hog::mapgen

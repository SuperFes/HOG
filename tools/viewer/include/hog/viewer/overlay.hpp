#pragma once

#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/colour.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/viewer/frame.hpp>
#include <hog/viewer/map_view.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hog::viewer {

enum class Overlay : std::uint8_t {
    Terrain,     // elevation, with the ocean, lakes and rivers
    Photo,       // the ground in true colour, as it would look from above at noon
    Biome,       // with lakes and rivers
    Water,       // the water table's depth, with lakes and rivers
    Minerals,    // rock, or the rarest deposit, at a depth; needs the ground surveyed
    Heat,        // the rock's temperature at a depth; likewise
    Temperature, // at the surface
    Air,         // temperature, reddened along warm fronts and blued along cold ones
    Sky,         // cloud, rain, snow, hail and storms over the ground
    Pressure,    // at sea level
    Sea,         // its surface temperature, with arrows along the currents
    Soil,        // how wet it is, white under snow
    Plants,      // biomass as a share of what the ground holds
    Fire,        // plants, with what's burning
    Wildlife,    // one species, by density
};

inline constexpr std::array overlays{
    Overlay::Terrain, Overlay::Photo,       Overlay::Biome,  Overlay::Water, Overlay::Minerals,
    Overlay::Heat,    Overlay::Temperature, Overlay::Air,    Overlay::Sky,   Overlay::Pressure,
    Overlay::Sea,     Overlay::Soil,        Overlay::Plants, Overlay::Fire,  Overlay::Wildlife,
};

// Whether an overlay shows what lies underground, which needs the ground surveyed at a depth.
[[nodiscard]] constexpr bool underground(Overlay overlay) noexcept {
    return overlay == Overlay::Minerals || overlay == Overlay::Heat;
}

[[nodiscard]] std::string_view to_string(Overlay overlay) noexcept;
[[nodiscard]] std::optional<Overlay> overlay_named(std::string_view name) noexcept;

// The layers an overlay draws, so a tool fetches only those. `species` names the wildlife shown.
[[nodiscard]] std::vector<std::string> layers_for(Overlay overlay, std::string_view species);

// The range of the layer an overlay mostly shows, as of a frame.
struct Legend {
    std::string layer;
    std::string unit;
    double low = 0.0;
    double high = 0.0;
};

// None for overlays of categories, such as biomes, or when the layer is missing.
[[nodiscard]] std::optional<Legend> legend_for(Overlay overlay, const Frame& frame,
                                               std::string_view species);

// What the map shows where its layers are missing.
inline constexpr mapgen::Rgb missing_colour{90, 90, 90};
inline constexpr mapgen::Rgb arrow_colour{15, 25, 45};

// Arrows are this many pixels apart, one centred in each square of the image.
[[nodiscard]] int arrow_spacing(int width) noexcept;

// The shown part of the map, latitude down and longitude across, twice as wide as it is tall.
// Fields are smoothed between cells; biomes and fires are shown cell by cell. Where `ground`, if
// any, was surveyed, the terrain, coasts and biomes come from it instead, as finely as it was
// sampled, and rivers show, as wide as they are or a sample if narrower. `natural` gives the cells'
// edges a natural wander. Precondition: the grid is at the frame's level.
[[nodiscard]] mapgen::Image render_overlay(const sim::CellGrid& grid, const Frame& frame,
                                           const probe::Ground* ground, Overlay overlay,
                                           std::string_view species, const MapRect& shown,
                                           int width, bool natural, jobs::JobSystem& jobs);

// The planet on the six faces of its cube, `size` pixels across each, stacked from face 0 down.
// Pixels are evenly spaced along each face's s and t, so at the level with `size` cells across a
// face each pixel is one cell. Drawn as render_overlay draws, without the sea's arrows.
[[nodiscard]] mapgen::Image render_faces(const sim::CellGrid& grid, const Frame& frame,
                                         const probe::Ground* ground, Overlay overlay,
                                         std::string_view species, int size, bool natural,
                                         jobs::JobSystem& jobs);

// Metres the land stands above the sea, and none for the sea or where unknown, at (steps + 1)²
// points per face with s and t of 0, 1 / steps, ... 1, stacked from face 0. Taken by direction, so
// neighbouring faces agree where they meet.
[[nodiscard]] std::vector<float> render_heights(const sim::CellGrid& grid, const Frame& frame,
                                                const probe::Ground* ground, int steps,
                                                bool natural, jobs::JobSystem& jobs);

// Pale lows through white to dark red highs, in hPa.
[[nodiscard]] mapgen::Rgb pressure_colour(double pressure) noexcept;

// Bare tan through grass and shrubs to dark mature forest, by biomass as a share of capacity.
[[nodiscard]] mapgen::Rgb plant_colour(double share) noexcept;

// Plants burning, brighter as the fire is stronger, from 0 to 1.
[[nodiscard]] mapgen::Rgb fire_colour(mapgen::Rgb plants, double fire) noexcept;

// The ground under mm of snow, whiter as more of it is hidden.
[[nodiscard]] mapgen::Rgb snow_colour(mapgen::Rgb ground, double snow) noexcept;

} // namespace hog::viewer

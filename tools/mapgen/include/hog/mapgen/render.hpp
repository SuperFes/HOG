#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/colour.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/planet/geology.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hog::mapgen {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // RGB, row by row from the top

    [[nodiscard]] Rgb at(int x, int y) const;
};

struct ImageSize {
    int width = 0;
    int height = 0;

    friend bool operator==(const ImageSize&, const ImageSize&) = default;
};

// The planet view is twice as wide as it is tall; the others are square.
[[nodiscard]] ImageSize image_size(const View& view, int width) noexcept;

// The view with any place named by the planet, such as the starting islands, turned into a region.
[[nodiscard]] View resolve(const View& view, const planet::Geology& geology);

// The unit direction under the centre of a pixel. Precondition: the view is resolved.
[[nodiscard]] DVec3 pixel_direction(const View& view, ImageSize size, int x, int y,
                                    double planet_radius);

// Blue deepening with the ocean; green, tan, brown and white climbing on land. Elevation is in
// fixed-point metres.
[[nodiscard]] Rgb height_colour(std::int64_t elevation) noexcept;

[[nodiscard]] Rgb biome_colour(planet::Biome biome) noexcept;

// Blue below freezing, white at 0 °C, red when hot; temperature is fixed-point °C.
[[nodiscard]] Rgb temperature_colour(std::int64_t temperature) noexcept;

// Brown when arid through green to blue when saturated; moisture is fixed point from 0 to 1.
[[nodiscard]] Rgb moisture_colour(std::int64_t moisture) noexcept;

// The rarest deposit under a cell, over grey land shaded by height and dark ocean.
[[nodiscard]] Rgb
deposit_colour(const std::array<planet::Deposit, planet::resource_count>& deposits,
               std::int64_t elevation) noexcept;

[[nodiscard]] Image render(const planet::Geology& geology, double planet_radius, const View& view,
                           Layer layer, int width, jobs::JobSystem& jobs);

// The file name for a view and layer, such as face2-height.png.
[[nodiscard]] std::string file_name(const View& view, Layer layer);

[[nodiscard]] Result<void> write_png(const std::filesystem::path& path, const Image& image);

} // namespace hog::mapgen

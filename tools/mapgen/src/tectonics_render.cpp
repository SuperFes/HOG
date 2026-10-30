#include <hog/mapgen/tectonics_render.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace hog::mapgen {

namespace {

void put(Image& image, int x, int y, Rgb colour) {
    const std::size_t at =
        3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)) +
             static_cast<std::size_t>(x));
    image.pixels[at] = colour[0];
    image.pixels[at + 1] = colour[1];
    image.pixels[at + 2] = colour[2];
}

// Ocean floor by its age in steps, continents by kilometres of crust.
constexpr std::array floor_stops{ColourStop{.at = 0.0, .colour = {150, 210, 240}},
                                 ColourStop{.at = 40.0, .colour = {30, 80, 160}},
                                 ColourStop{.at = 120.0, .colour = {10, 30, 80}}};
constexpr std::array continent_stops{ColourStop{.at = 25.0, .colour = {90, 150, 70}},
                                     ColourStop{.at = 35.0, .colour = {120, 170, 80}},
                                     ColourStop{.at = 50.0, .colour = {190, 160, 100}},
                                     ColourStop{.at = 70.0, .colour = {150, 100, 70}},
                                     ColourStop{.at = 90.0, .colour = {250, 250, 250}}};
// The sea by metres deep, the land by metres high.
constexpr std::array depth_stops{ColourStop{.at = 0.0, .colour = {120, 200, 225}},
                                 ColourStop{.at = 300.0, .colour = {60, 140, 200}},
                                 ColourStop{.at = 2'500.0, .colour = {30, 80, 160}},
                                 ColourStop{.at = 6'000.0, .colour = {10, 25, 70}}};
constexpr std::array height_stops{ColourStop{.at = 0.0, .colour = {80, 150, 70}},
                                  ColourStop{.at = 500.0, .colour = {130, 170, 80}},
                                  ColourStop{.at = 1'500.0, .colour = {190, 160, 100}},
                                  ColourStop{.at = 3'000.0, .colour = {150, 100, 70}},
                                  ColourStop{.at = 5'000.0, .colour = {250, 250, 250}}};
// Ocean floor by its age in millions of years; the oldest on Earth is under 200.
constexpr std::array floor_age_stops{ColourStop{.at = 0.0, .colour = {150, 210, 240}},
                                     ColourStop{.at = 60.0, .colour = {30, 80, 160}},
                                     ColourStop{.at = 180.0, .colour = {10, 30, 80}}};
// Plate edges are drawn this much darker.
constexpr double edge_darkening = 0.6;

// Each pixel of the view coloured by `colour` of the grid cell under it, plate edges darkened.
template <typename Colour>
Image paint(const planet::LevelGrid& grid, std::span<const std::uint32_t> plates,
            double planet_radius, const View& view, int width, jobs::JobSystem& jobs,
            const Colour& colour) {
    const ImageSize size = image_size(view, width);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                    static_cast<std::size_t>(size.height))};
    const auto colour_at = [&](DVec3 direction) {
        const std::uint32_t cell = grid.index(planet::cell_at(direction, grid.level()));
        const Rgb here = colour(cell);
        for (const std::uint32_t next : grid.neighbors(cell)) {
            if (plates[next] != plates[cell]) {
                return mix(here, Rgb{0, 0, 0}, edge_darkening);
            }
        }
        return here;
    };
    jobs.parallel_for(static_cast<std::uint32_t>(size.height), 1,
                      [&](jobs::Range rows, std::uint32_t /*thread*/) {
                          for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                              const auto y = static_cast<int>(row);
                              for (int x = 0; x < size.width; ++x) {
                                  put(image, x, y,
                                      colour_at(pixel_direction(view, size, x, y, planet_radius)));
                              }
                          }
                      });
    return image;
}

constexpr auto parted = std::numeric_limits<std::uint32_t>::max();

std::size_t pixel_index(const ImageSize& size, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width)) +
           static_cast<std::size_t>(x);
}

// The crust on top over a place, the plate carrying it, and how many plates carry crust there.
struct PlatesOver {
    std::optional<planet::Crust> top;
    std::uint32_t plate = parted;
    int carrying = 0;

    [[nodiscard]] Rgb colour() const noexcept {
        const Rgb crust = top ? plate_crust_colour(*top) : mantle_colour;
        return carrying > 1 ? mix(crust, overlap_colour, overlap_tint) : crust;
    }
};

PlatesOver plates_over(const planet::Lithosphere& lithosphere, DVec3 at) {
    PlatesOver over;
    for (std::uint32_t plate = 0; plate < lithosphere.plates().size(); ++plate) {
        const std::optional<planet::Crust> crust = lithosphere.crust_of(plate, at);
        if (!crust) {
            continue;
        }
        ++over.carrying;
        if (!over.top || planet::stays_over(*crust, *over.top)) {
            over.top = crust;
            over.plate = plate;
        }
    }
    return over;
}

// Darkens each pixel where the plate on top changes to the right or below; not along gaps.
void darken_edges(Image& image, const std::vector<std::uint32_t>& on_top) {
    const ImageSize size{.width = image.width, .height = image.height};
    const auto differs = [&](std::uint32_t here, int x, int y) {
        const std::uint32_t there = on_top[pixel_index(size, x, y)];
        return there != here && there != parted;
    };
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            const std::uint32_t here = on_top[pixel_index(size, x, y)];
            if (here != parted && ((x + 1 < size.width && differs(here, x + 1, y)) ||
                                   (y + 1 < size.height && differs(here, x, y + 1)))) {
                put(image, x, y, mix(image.at(x, y), Rgb{0, 0, 0}, edge_darkening));
            }
        }
    }
}

} // namespace

Rgb crust_colour(const planet::Tectonics::Column& column) noexcept {
    if (column.crust == planet::Tectonics::Crust::Oceanic) {
        return ramp(floor_stops, static_cast<double>(column.age));
    }
    return ramp(continent_stops, static_cast<double>(column.thickness));
}

Rgb relief_colour(double above_sea) noexcept {
    return above_sea < 0.0 ? ramp(depth_stops, -above_sea) : ramp(height_stops, above_sea);
}

Image render_tectonics(const planet::Tectonics& tectonics, double planet_radius, const View& view,
                       int width, jobs::JobSystem& jobs) {
    const auto columns = tectonics.columns();
    std::vector<std::uint32_t> plates(columns.size());
    std::ranges::transform(columns, plates.begin(),
                           [](const planet::Tectonics::Column& column) { return column.plate; });
    return paint(tectonics.grid(), plates, planet_radius, view, width, jobs,
                 [&](std::uint32_t cell) { return crust_colour(columns[cell]); });
}

Image render_relief(const planet::Lithosphere& lithosphere, std::span<const std::int64_t> heights,
                    std::int64_t sea_level, double planet_radius, const View& view, int width,
                    jobs::JobSystem& jobs) {
    const planet::LevelGrid& grid = lithosphere.grid();
    std::vector<std::uint32_t> plates(grid.size(), parted);
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (const auto found = lithosphere.top(planet::cell_center(grid.cell(cell)))) {
            plates[cell] = found->plate;
        }
    }
    return paint(grid, plates, planet_radius, view, width, jobs, [&](std::uint32_t cell) {
        return relief_colour(planet::fixed_to_double(heights[cell] - sea_level));
    });
}

Rgb plate_crust_colour(const planet::Crust& crust) noexcept {
    if (crust.type == planet::CrustType::Oceanic) {
        return ramp(floor_age_stops, static_cast<double>(crust.age));
    }
    return ramp(continent_stops, static_cast<double>(crust.thickness));
}

Image render_plates(const planet::Lithosphere& lithosphere, double planet_radius, const View& view,
                    int width, jobs::JobSystem& jobs) {
    const ImageSize size = image_size(view, width);
    const auto pixels =
        static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * pixels)};
    // The plate on top at each pixel, to find the edges between them after.
    std::vector<std::uint32_t> on_top(pixels, parted);
    jobs.parallel_for(static_cast<std::uint32_t>(size.height), 1,
                      [&](jobs::Range rows, std::uint32_t /*thread*/) {
                          for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                              const auto y = static_cast<int>(row);
                              for (int x = 0; x < size.width; ++x) {
                                  const PlatesOver over =
                                      plates_over(lithosphere,
                                                  pixel_direction(view, size, x, y, planet_radius));
                                  on_top[pixel_index(size, x, y)] = over.plate;
                                  put(image, x, y, over.colour());
                              }
                          }
                      });
    darken_edges(image, on_top);
    return image;
}

std::string evolution_file_name(const View& view, std::string_view kind, int step) {
    // Same prefix as the layer maps, so a view's files sort together.
    const std::string layer_name = file_name(view, Layer::Height);
    const std::string prefix = layer_name.substr(0, layer_name.rfind('-'));
    return std::format("{}-{}-{:04}.png", prefix, kind, step);
}

} // namespace hog::mapgen

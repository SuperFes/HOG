#include <hog/mapgen/tectonics_render.hpp>
#include <hog/planet/projection.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
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
// Plate edges are drawn this much darker.
constexpr double edge_darkening = 0.6;

} // namespace

Rgb crust_colour(const planet::Tectonics::Column& column) noexcept {
    if (column.crust == planet::Tectonics::Crust::Oceanic) {
        return ramp(floor_stops, static_cast<double>(column.age));
    }
    return ramp(continent_stops, static_cast<double>(column.thickness));
}

Image render_tectonics(const planet::Tectonics& tectonics, double planet_radius, const View& view,
                       int width, jobs::JobSystem& jobs) {
    const ImageSize size = image_size(view, width);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                    static_cast<std::size_t>(size.height))};
    const planet::LevelGrid& grid = tectonics.grid();
    const auto columns = tectonics.columns();
    const auto colour_at = [&](DVec3 direction) {
        const std::uint32_t cell = grid.index(planet::cell_at(direction, grid.level()));
        const Rgb colour = crust_colour(columns[cell]);
        for (const std::uint32_t next : grid.neighbors(cell)) {
            if (columns[next].plate != columns[cell].plate) {
                return mix(colour, Rgb{0, 0, 0}, edge_darkening);
            }
        }
        return colour;
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

std::string tectonics_file_name(const View& view, int step) {
    // Same prefix as the layer maps, so a view's files sort together.
    const std::string layer_name = file_name(view, Layer::Height);
    const std::string prefix = layer_name.substr(0, layer_name.rfind('-'));
    return std::format("{}-tectonics-{:04}.png", prefix, step);
}

} // namespace hog::mapgen

#include <hog/core/assert.hpp>
#include <hog/mapgen/season_render.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <vector>

namespace hog::mapgen {

namespace {

constexpr std::array rain_stops{
    ColourStop{.at = 0.0, .colour = {235, 220, 180}},
    ColourStop{.at = 1.0, .colour = {170, 200, 140}},
    ColourStop{.at = 3.0, .colour = {80, 160, 170}},
    ColourStop{.at = 8.0, .colour = {30, 80, 190}},
    ColourStop{.at = 20.0, .colour = {60, 20, 120}},
};

constexpr Rgb snow_white{245, 248, 252};
constexpr Rgb flood_brown{140, 110, 70};
constexpr Rgb bare_earth{165, 140, 105};
constexpr Rgb fire_orange{250, 110, 20};

// Flow over its usual at which a river is drawn fully in flood, and as wide as it's drawn.
constexpr double full_flood = 5.0;
constexpr double widest = 3.0;

double at(std::span<const float> field, std::uint32_t index) {
    return static_cast<double>(field[index]);
}

// The land cell for a point: the cell under it, or a land neighbour where that's sea, so a land
// pixel on a coast reads the land beside it. The sea when there's no land about.
std::uint32_t land_cell(const sim::CellGrid& grid, const std::vector<sim::Ground>& ground,
                        DVec3 point) {
    const std::uint32_t under = grid.index(planet::cell_at(point, grid.level()));
    if (!ground[under].ocean) {
        return under;
    }
    for (const std::uint32_t next : grid.neighbors(under)) {
        if (!ground[next].ocean) {
            return next;
        }
    }
    return under;
}

// A land field interpolated between land cells only, so the coasts aren't drawn like the sea.
template <typename Value>
double on_land(const sim::CellGrid& grid, const std::vector<sim::Ground>& ground, DVec3 point,
               Value value) {
    const std::uint32_t centre = land_cell(grid, ground, point);
    return grid.interpolate(
        point, [&](std::uint32_t index) { return value(ground[index].ocean ? centre : index); });
}

// Same prefix as the layer maps, so a view's files sort together.
std::string view_prefix(const View& view) {
    const std::string layer_name = file_name(view, Layer::Height);
    return layer_name.substr(0, layer_name.rfind('-'));
}

} // namespace

std::string_view to_string(SeasonMap map) noexcept {
    switch (map) {
    case SeasonMap::Temperature:
        return "temperature";
    case SeasonMap::Rain:
        return "rain";
    case SeasonMap::Land:
        return "land";
    case SeasonMap::Forest:
        return "forest";
    }
    return "unknown";
}

Rgb rain_colour(double per_day) noexcept {
    return ramp(rain_stops, per_day);
}

Rgb soil_colour(double soil, double snow) noexcept {
    const double cover = snow / (snow + sim::LandTuning{}.covering_snow);
    return mix(moisture_colour(std::llround(soil * static_cast<double>(planet::fixed_one))),
               snow_white, cover);
}

Rgb flowing_colour(Rgb ground, Rgb river, double ratio) noexcept {
    if (ratio < 1.0) {
        return mix(ground, river, 0.3 + (0.7 * std::max(ratio, 0.0)));
    }
    return mix(river, flood_brown, (ratio - 1.0) / (full_flood - 1.0));
}

Rgb forest_colour(Rgb grown, double share, double fire) noexcept {
    return mix(mix(bare_earth, grown, share), fire_orange, std::sqrt(std::max(fire, 0.0)));
}

Image render_season(const planet::Geology& geology, const sim::Weather& weather,
                    const sim::Forest& forest, double planet_radius, const View& view,
                    SeasonMap map, int width, jobs::JobSystem& jobs) {
    HOG_ASSERT(forest.grid().level() == weather.grid().level(), "forest off the weather's grid");
    const View resolved = resolve(view, geology);
    const ImageSize size = image_size(resolved, width);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                    static_cast<std::size_t>(size.height))};
    const sim::CellGrid& grid = weather.grid();
    const std::vector<sim::Ground>& ground = weather.ground();
    const sim::WeatherFields& fields = weather.fields();
    const sim::LandFields& land = weather.land().fields();
    const sim::ForestFields& plants = forest.fields();
    const planet::Elevation& terrain = geology.terrain();
    // Half a pixel, in metres, at a pixel's centre.
    const auto margin = [&](DVec3 here, int x, int y) {
        const int other = y + 1 < size.height ? y + 1 : y - 1;
        return 0.5 * glm::length(here - pixel_direction(resolved, size, x, other, planet_radius)) *
               planet_radius;
    };
    const auto land_colour = [&](planet::CellId cell, DVec3 point, int x, int y) {
        const Rgb soil = soil_colour(
            on_land(grid, ground, point, [&](std::uint32_t index) { return at(land.soil, index); }),
            on_land(grid, ground, point,
                    [&](std::uint32_t index) { return at(land.snow, index); }));
        if (terrain.lake(cell)) {
            return biome_colour(planet::Biome::Lake);
        }
        const double ratio = weather.land().flow_ratio(land_cell(grid, ground, point));
        // Floods spread wider than the river's own bed.
        const planet::Reach* river =
            terrain.rivers().near(cell, margin(point, x, y) * std::clamp(ratio, 1.0, widest));
        return river == nullptr ? soil : flowing_colour(soil, river_colour(river->width), ratio);
    };
    const auto forest_at = [&](planet::CellId cell, DVec3 point) {
        const auto share = [&](std::uint32_t index) {
            const double capacity = forest.capacity(index);
            return capacity > 0.0 ? at(plants.biomass, index) / capacity : 1.0;
        };
        return forest_colour(biome_colour(geology.climate().biome(cell)),
                             on_land(grid, ground, point, share),
                             on_land(grid, ground, point,
                                     [&](std::uint32_t index) { return at(plants.fire, index); }));
    };
    const auto colour_at = [&](DVec3 point, int x, int y) -> Rgb {
        const auto field = [&](const std::vector<float>& values) {
            return grid.interpolate(point, [&](std::uint32_t index) { return at(values, index); });
        };
        switch (map) {
        case SeasonMap::Temperature:
            return temperature_colour(std::llround(field(fields.recent_temperature) *
                                                   static_cast<double>(planet::fixed_one)));
        case SeasonMap::Rain:
            return rain_colour(field(fields.recent_precipitation));
        case SeasonMap::Land:
        case SeasonMap::Forest: {
            const planet::CellId cell = planet::cell_at(point);
            if (const std::int64_t elevation = terrain.elevation(cell); elevation <= 0) {
                return height_colour(elevation);
            }
            return map == SeasonMap::Land ? land_colour(cell, point, x, y) : forest_at(cell, point);
        }
        }
        return {};
    };
    jobs.parallel_for(
        static_cast<std::uint32_t>(size.height), 1,
        [&](jobs::Range rows, std::uint32_t /*thread*/) {
            for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                const auto y = static_cast<int>(row);
                for (int x = 0; x < size.width; ++x) {
                    const Rgb colour =
                        colour_at(pixel_direction(resolved, size, x, y, planet_radius), x, y);
                    const auto offset =
                        3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(size.width)) +
                             static_cast<std::size_t>(x));
                    std::ranges::copy(colour,
                                      image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
                }
            }
        });
    return image;
}

Image contact_sheet(std::span<const Image> frames, int columns) {
    HOG_ASSERT(!frames.empty() && columns > 0, "a sheet needs frames and columns");
    const int frame_width = frames.front().width;
    const int frame_height = frames.front().height;
    const auto count = static_cast<int>(frames.size());
    const int across = std::min(columns, count);
    const int down = (count + columns - 1) / columns;
    Image sheet{.width = across * frame_width,
                .height = down * frame_height,
                .pixels = std::vector<std::uint8_t>(
                    3 * static_cast<std::size_t>(across) * static_cast<std::size_t>(frame_width) *
                    static_cast<std::size_t>(down) * static_cast<std::size_t>(frame_height))};
    const auto row_bytes = 3 * static_cast<std::size_t>(frame_width);
    for (int frame = 0; frame < count; ++frame) {
        const Image& image = frames[static_cast<std::size_t>(frame)];
        HOG_ASSERT(image.width == frame_width && image.height == frame_height,
                   "sheet frames differ in size");
        const int left = (frame % columns) * frame_width;
        const int top = (frame / columns) * frame_height;
        for (int y = 0; y < frame_height; ++y) {
            const auto from = static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * row_bytes);
            const auto to = static_cast<std::ptrdiff_t>(
                3 * ((static_cast<std::size_t>(top + y) * static_cast<std::size_t>(sheet.width)) +
                     static_cast<std::size_t>(left)));
            std::copy_n(image.pixels.begin() + from, row_bytes, sheet.pixels.begin() + to);
        }
    }
    return sheet;
}

std::string season_file_name(const View& view, SeasonMap map, std::chrono::year_month month) {
    return std::format("{}-seasons-{:04}{:02}-{}.png", view_prefix(view),
                       static_cast<int>(month.year()), static_cast<unsigned>(month.month()),
                       to_string(map));
}

std::string season_sheet_name(const View& view, SeasonMap map) {
    return std::format("{}-seasons-{}.png", view_prefix(view), to_string(map));
}

} // namespace hog::mapgen

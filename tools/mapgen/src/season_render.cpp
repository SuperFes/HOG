#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/mapgen/season_render.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
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

// The cell for a point on land or at sea: the cell under it, or a neighbour on that side where it's
// the other, so a pixel on a coast reads its own side. The cell under it when there's none about.
std::uint32_t side_cell(const sim::CellGrid& grid, const std::vector<sim::Ground>& ground,
                        DVec3 point, bool sea) {
    const std::uint32_t under = grid.index(planet::cell_at(point, grid.level()));
    if (ground[under].ocean == sea) {
        return under;
    }
    for (const std::uint32_t next : grid.neighbors(under)) {
        if (ground[next].ocean == sea) {
            return next;
        }
    }
    return under;
}

// A field interpolated between cells on one side of the coast only, so the land isn't drawn like
// the sea, nor the sea like the land.
template <typename Value>
double on_side(const sim::CellGrid& grid, const std::vector<sim::Ground>& ground, DVec3 point,
               bool sea, Value value) {
    const std::uint32_t centre = side_cell(grid, ground, point, sea);
    return grid.interpolate(point, [&](std::uint32_t index) {
        return value(ground[index].ocean == sea ? index : centre);
    });
}

template <typename Value>
double on_land(const sim::CellGrid& grid, const std::vector<sim::Ground>& ground, DVec3 point,
               Value value) {
    return on_side(grid, ground, point, false, value);
}

std::uint32_t land_cell(const sim::CellGrid& grid, const std::vector<sim::Ground>& ground,
                        DVec3 point) {
    return side_cell(grid, ground, point, false);
}

// Same prefix as the layer maps, so a view's files sort together.
std::string view_prefix(const View& view) {
    const std::string layer_name = file_name(view, Layer::Height);
    return layer_name.substr(0, layer_name.rfind('-'));
}

// Whether higher ground hides the sun, followed across the image along the sun's bearing, given
// the metres a pixel spans across and down.
bool casts_shadow(const HeightField& field, int x, int y, DVec3 sun, DVec3 up, DVec3 across,
                  DVec3 down, int reach) {
    const double rising = glm::dot(sun, up);
    const DVec3 bearing = sun - (up * rising);
    // The pixels across and down that head along the bearing.
    const double aa = glm::dot(across, across);
    const double ad = glm::dot(across, down);
    const double dd = glm::dot(down, down);
    const double determinant = (aa * dd) - (ad * ad);
    if (rising <= 0.0 || glm::length(bearing) <= 0.0 || determinant <= 0.0) {
        return false;
    }
    const double ba = glm::dot(bearing, across);
    const double bd = glm::dot(bearing, down);
    const DVec2 heading{((dd * ba) - (ad * bd)) / determinant,
                        ((aa * bd) - (ad * ba)) / determinant};
    // A pixel's step along the bearing, and the metres it covers.
    const double pixels = glm::length(heading);
    const double metres = glm::length(bearing) / pixels;
    return in_shadow(field, x, y, heading / pixels, metres * rising / glm::length(bearing), reach);
}

// Clouds over the weather's grid, broken up finer than its cells.
class Sky {
public:
    Sky(const sim::Weather& weather, double planet_radius)
        : weather_{&weather}, planet_radius_{planet_radius},
          seed_{hash_values(weather.config().seed,
                            static_cast<std::uint64_t>(weather.time().time_since_epoch().count()))},
          // Two of the grid's cells a noise lattice cell.
          across_{2.0 * planet_radius *
                  std::sqrt(4.0 * std::numbers::pi / static_cast<double>(weather.grid().size()))} {}

    [[nodiscard]] Cloud cloud(DVec3 point) const {
        const sim::WeatherFields& fields = weather_->fields();
        const double cover = weather_->grid().interpolate(point, [&](std::uint32_t index) {
            return static_cast<double>(fields.cloud_cover(index));
        });
        return cloud_over(cover, cloud_noise(seed_, point * planet_radius_, across_));
    }

    [[nodiscard]] double storm(DVec3 point) const {
        return weather_->grid().interpolate(
            point, [&](std::uint32_t index) { return at(weather_->fields().storm, index); });
    }

private:
    const sim::Weather* weather_;
    double planet_radius_;
    std::uint64_t seed_;
    double across_;
};

// Pixels are coloured, then lit by the slope of the ground around them and any higher ground
// between them and the sun, under the clouds.
Image render_photo(const planet::Geology& geology, const sim::Weather& weather,
                   const sim::Forest& forest, double planet_radius, const View& resolved,
                   ImageSize size, const Lighting& lighting, jobs::JobSystem& jobs) {
    const auto width = static_cast<std::size_t>(size.width);
    const auto count = width * static_cast<std::size_t>(size.height);
    std::vector<DVec3> points(count);
    std::vector<double> heights(count);
    std::vector<LinearRgb> colours(count);
    std::vector<Cloud> clouds(count);
    std::vector<double> storms(count);
    const Sky sky{weather, planet_radius};
    const auto pixel = [&](int x, int y) {
        return (static_cast<std::size_t>(y) * width) + static_cast<std::size_t>(x);
    };
    const auto rows = static_cast<std::uint32_t>(size.height);
    jobs.parallel_for(rows, 1, [&](jobs::Range range, std::uint32_t /*thread*/) {
        for (std::uint32_t row = range.begin; row < range.end; ++row) {
            const auto y = static_cast<int>(row);
            const int other = y + 1 < size.height ? y + 1 : y - 1;
            for (int x = 0; x < size.width; ++x) {
                const DVec3 point = pixel_direction(resolved, size, x, y, planet_radius);
                const double across =
                    glm::length(point - pixel_direction(resolved, size, x, other, planet_radius)) *
                    planet_radius;
                const Surface surface = sample_surface(geology, weather, forest, point, across);
                points[pixel(x, y)] = point;
                heights[pixel(x, y)] = lighting.relief * surface.height;
                colours[pixel(x, y)] = true_colour(surface);
                clouds[pixel(x, y)] = sky.cloud(point);
                storms[pixel(x, y)] = sky.storm(point);
            }
        }
    });
    Image image{
        .width = size.width, .height = size.height, .pixels = std::vector<std::uint8_t>(3 * count)};
    const HeightField field{.width = size.width, .height = size.height, .heights = heights};
    const double exposed = exposure(lighting);
    jobs.parallel_for(rows, 1, [&](jobs::Range range, std::uint32_t /*thread*/) {
        for (std::uint32_t row = range.begin; row < range.end; ++row) {
            const auto y = static_cast<int>(row);
            const int below = std::max(y - 1, 0);
            const int above = std::min(y + 1, size.height - 1);
            for (int x = 0; x < size.width; ++x) {
                const int left = std::max(x - 1, 0);
                const int right = std::min(x + 1, size.width - 1);
                const auto ground = [&](int column, int line) {
                    const std::size_t at = pixel(column, line);
                    return points[at] * (planet_radius + heights[at]);
                };
                const auto flat = [&](int column, int line) {
                    return points[pixel(column, line)] * planet_radius;
                };
                const planet::TangentFrame frame = planet::tangent_frame(points[pixel(x, y)]);
                const DVec3 normal = surface_normal(ground(left, y), ground(right, y),
                                                    ground(x, below), ground(x, above), frame.up);
                const DVec3 sun = sun_towards(lighting, frame);
                const bool shadowed = casts_shadow(
                    field, x, y, sun, frame.up,
                    (flat(right, y) - flat(left, y)) / static_cast<double>(right - left),
                    (flat(x, above) - flat(x, below)) / static_cast<double>(above - below),
                    lighting.shadow_reach);
                const LinearRgb ground_lit =
                    colours[pixel(x, y)] *
                    (light(normal, frame.up, sun, lighting.sky, shadowed) / exposed);
                const Cloud& cloud = clouds[pixel(x, y)];
                const LinearRgb cloud_lit =
                    cloud_colour(cloud, storms[pixel(x, y)]) *
                    (light(frame.up, frame.up, sun, lighting.sky, false) / exposed);
                const LinearRgb lit = glm::mix(ground_lit, cloud_lit, cloud.opacity());
                std::ranges::copy(to_srgb(lit), image.pixels.begin() +
                                                    static_cast<std::ptrdiff_t>(3 * pixel(x, y)));
            }
        }
    });
    return image;
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
    case SeasonMap::Photo:
        return "photo";
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

Surface sample_surface(const planet::Geology& geology, const sim::Weather& weather,
                       const sim::Forest& forest, DVec3 point, double across) {
    HOG_ASSERT(forest.grid().level() == weather.grid().level(), "forest off the weather's grid");
    const sim::CellGrid& grid = weather.grid();
    const std::vector<sim::Ground>& ground = weather.ground();
    const planet::Elevation& terrain = geology.terrain();
    const planet::CellId cell = planet::cell_at(point);
    const std::int64_t elevation = terrain.elevation(cell);
    if (elevation <= 0) {
        const std::vector<float>& sea = weather.ocean().temperature;
        const double freezing = sim::OceanTuning{}.freezing;
        return Surface{.biome = planet::Biome::Ocean,
                       .depth = -planet::fixed_to_double(elevation),
                       .ice = on_side(grid, ground, point, true, [&](std::uint32_t index) {
                           return at(sea, index) <= freezing ? 1.0 : 0.0;
                       })};
    }
    const sim::LandFields& land = weather.land().fields();
    const sim::ForestFields& plants = forest.fields();
    const double height = planet::fixed_to_double(elevation);
    const auto field = [&](const std::vector<float>& values) {
        return on_land(grid, ground, point, [&](std::uint32_t index) { return at(values, index); });
    };
    Surface surface{.height = height,
                    .plants = on_land(grid, ground, point,
                                      [&](std::uint32_t index) {
                                          const double capacity = forest.capacity(index);
                                          return capacity > 0.0
                                                     ? at(plants.biomass, index) / capacity
                                                     : 1.0;
                                      }),
                    .fire = field(plants.fire),
                    .soil = field(land.soil),
                    .snow = field(land.snow)};
    if (terrain.lake(cell)) {
        surface.biome = planet::Biome::Lake;
        return surface;
    }
    const planet::Biome biome = geology.climate().biome(cell);
    surface.biome = biome == planet::Biome::Ocean ? planet::Biome::Beach : biome;
    surface.flow = weather.land().flow_ratio(land_cell(grid, ground, point));
    // Floods spread wider than the river's own bed.
    const double spread = std::clamp(surface.flow, 1.0, widest);
    if (const planet::Reach* river = terrain.rivers().near(cell, 0.5 * across * spread)) {
        // Drawn at least this thick, so a creek still shows as a line.
        constexpr double thinnest = 0.4;
        surface.river = std::clamp(river->width * spread / across, thinnest, 1.0);
    }
    return surface;
}

Image render_season(const planet::Geology& geology, const sim::Weather& weather,
                    const sim::Forest& forest, double planet_radius, const View& view,
                    SeasonMap map, int width, jobs::JobSystem& jobs, const Lighting& lighting) {
    HOG_ASSERT(forest.grid().level() == weather.grid().level(), "forest off the weather's grid");
    const View resolved = resolve(view, geology);
    const ImageSize size = image_size(resolved, width);
    if (map == SeasonMap::Photo) {
        return render_photo(geology, weather, forest, planet_radius, resolved, size, lighting,
                            jobs);
    }
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
        case SeasonMap::Photo: // lit by render_photo
            break;
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

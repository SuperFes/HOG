#include <hog/mapgen/weather_render.hpp>
#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/weather.hpp>
#include <hog/viewer/overlay.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace hog::viewer {

namespace {

using mapgen::ColourStop;
using mapgen::Rgb;

constexpr std::array pressure_stops{
    ColourStop{.at = 970.0, .colour = {120, 60, 170}},
    ColourStop{.at = 995.0, .colour = {90, 150, 230}},
    ColourStop{.at = 1'013.0, .colour = {245, 245, 245}},
    ColourStop{.at = 1'030.0, .colour = {240, 160, 70}},
    ColourStop{.at = 1'045.0, .colour = {150, 30, 30}},
};

constexpr std::array plant_stops{
    ColourStop{.at = 0.0, .colour = {175, 150, 105}},
    ColourStop{.at = 0.25, .colour = {190, 195, 95}},
    ColourStop{.at = 0.5, .colour = {120, 170, 65}},
    ColourStop{.at = 1.0, .colour = {25, 95, 40}},
};

constexpr Rgb ocean_blue{40, 90, 150};
constexpr Rgb burning{255, 80, 10};
// Shallow sea, which the elevation layer doesn't distinguish from deep.
constexpr double ocean_depth = -200.0;

std::int64_t fixed(double value) {
    return std::llround(value * planet::fixed_one);
}

// Reads a frame's layers at points on the planet.
class Sampler {
public:
    Sampler(const sim::CellGrid& grid, const Frame& frame) : grid_(&grid), frame_(&frame) {}

    [[nodiscard]] std::span<const float> layer(std::string_view name) const {
        return frame_->layer(name);
    }

    // Smoothed between cells. Precondition: the layer isn't empty.
    [[nodiscard]] double smooth(std::span<const float> values, DVec3 direction) const {
        return grid_->interpolate(
            direction, [values](std::uint32_t cell) { return static_cast<double>(values[cell]); });
    }

    // The cell under the point. Precondition: the layer isn't empty.
    [[nodiscard]] double nearest(std::span<const float> values, DVec3 direction) const {
        return static_cast<double>(values[cell(direction)]);
    }

    [[nodiscard]] std::uint32_t cell(DVec3 direction) const {
        return grid_->index(planet::cell_at(direction, grid_->level()));
    }

    [[nodiscard]] bool ocean(DVec3 direction) const {
        const std::span<const float> ocean = layer("planet/ocean");
        // Smoothed, so coasts run between cells rather than around them.
        return !ocean.empty() && smooth(ocean, direction) > 0.5;
    }

    [[nodiscard]] Rgb ground(DVec3 direction) const {
        const std::span<const float> elevation = layer("planet/elevation");
        if (elevation.empty()) {
            return missing_colour;
        }
        if (ocean(direction)) {
            return mapgen::height_colour(fixed(ocean_depth));
        }
        // Land at sea level would take the shallows' colour.
        return mapgen::height_colour(fixed(std::max(smooth(elevation, direction), 1.0)));
    }

private:
    const sim::CellGrid* grid_;
    const Frame* frame_;
};

// Animals per km² in each cell; empty without the counts or the areas.
std::vector<float> densities(const Frame& frame, std::span<const float> count) {
    const std::span<const float> area = frame.layer("planet/area");
    if (count.empty() || area.size() != count.size()) {
        return {};
    }
    std::vector<float> density(count.size());
    for (std::size_t cell = 0; cell < count.size(); ++cell) {
        density[cell] = area[cell] > 0.0F ? count[cell] / area[cell] : 0.0F;
    }
    return density;
}

Rgb plants_at(const Sampler& sampler, DVec3 direction) {
    const std::span<const float> biomass = sampler.layer("forest/biomass");
    const std::span<const float> capacity = sampler.layer("forest/capacity");
    if (biomass.empty() || capacity.empty()) {
        return missing_colour;
    }
    if (sampler.ocean(direction)) {
        return ocean_blue;
    }
    const std::uint32_t cell = sampler.cell(direction);
    if (!(capacity[cell] > 0.0F)) {
        return mapgen::mix(sampler.ground(direction), {0, 0, 0}, 0.5);
    }
    return plant_colour(static_cast<double>(biomass[cell] / capacity[cell]));
}

Rgb weather_at(const Sampler& sampler, Overlay overlay, DVec3 direction) {
    const auto field = [&](std::string_view name, double& value) {
        const std::span<const float> values = sampler.layer(name);
        if (values.empty()) {
            return false;
        }
        value = sampler.smooth(values, direction);
        return true;
    };
    double temperature = 0.0;
    double other = 0.0;
    switch (overlay) {
    case Overlay::Temperature:
        return field("weather/temperature", temperature)
                   ? mapgen::temperature_colour(fixed(temperature))
                   : missing_colour;
    case Overlay::Air:
        return field("weather/temperature", temperature) && field("weather/front", other)
                   ? mapgen::air_colour(temperature, other)
                   : missing_colour;
    case Overlay::Pressure:
        return field("weather/pressure", other) ? pressure_colour(other) : missing_colour;
    default:
        break;
    }
    double cloud = 0.0;
    mapgen::Falling falling;
    double storm = 0.0;
    if (!field("weather/cloud", cloud) || !field("weather/rain", falling.rain) ||
        !field("weather/snow", falling.snow) || !field("weather/hail", falling.hail) ||
        !field("weather/storm", storm)) {
        return missing_colour;
    }
    return mapgen::sky_colour(sampler.ground(direction),
                              static_cast<double>(sim::cloud_cover(static_cast<float>(cloud))),
                              falling, storm);
}

Rgb colour_at(const Sampler& sampler, Overlay overlay, std::span<const float> species, double most,
              DVec3 direction) {
    switch (overlay) {
    case Overlay::Terrain:
        return sampler.ground(direction);
    case Overlay::Biome: {
        const std::span<const float> biome = sampler.layer("planet/biome");
        if (biome.empty()) {
            return missing_colour;
        }
        const auto index = std::clamp(static_cast<int>(sampler.nearest(biome, direction)), 0,
                                      planet::biome_count - 1);
        return mapgen::biome_colour(static_cast<planet::Biome>(index));
    }
    case Overlay::Temperature:
    case Overlay::Air:
    case Overlay::Sky:
    case Overlay::Pressure:
        return weather_at(sampler, overlay, direction);
    case Overlay::Dryness: {
        const std::span<const float> dryness = sampler.layer("forest/dryness");
        if (dryness.empty()) {
            return missing_colour;
        }
        if (sampler.ocean(direction)) {
            return ocean_blue;
        }
        return mapgen::moisture_colour(fixed(1.0 - sampler.smooth(dryness, direction)));
    }
    case Overlay::Plants:
        return plants_at(sampler, direction);
    case Overlay::Fire: {
        const std::span<const float> fire = sampler.layer("forest/fire");
        const Rgb plants = plants_at(sampler, direction);
        return fire.empty() ? missing_colour
                            : fire_colour(plants, sampler.nearest(fire, direction));
    }
    case Overlay::Wildlife: {
        if (species.empty()) {
            return missing_colour;
        }
        const double density = sampler.smooth(species, direction);
        return mapgen::density_colour(sampler.ground(direction), most > 0.0 ? density / most : 0.0);
    }
    }
    return missing_colour;
}

} // namespace

std::optional<Overlay> overlay_named(std::string_view name) noexcept {
    for (const Overlay overlay : overlays) {
        if (to_string(overlay) == name) {
            return overlay;
        }
    }
    return std::nullopt;
}

std::string_view to_string(Overlay overlay) noexcept {
    switch (overlay) {
    case Overlay::Terrain:
        return "terrain";
    case Overlay::Biome:
        return "biome";
    case Overlay::Temperature:
        return "temperature";
    case Overlay::Air:
        return "air";
    case Overlay::Sky:
        return "sky";
    case Overlay::Pressure:
        return "pressure";
    case Overlay::Dryness:
        return "dryness";
    case Overlay::Plants:
        return "plants";
    case Overlay::Fire:
        return "fire";
    case Overlay::Wildlife:
        return "wildlife";
    }
    return "unknown";
}

std::vector<std::string> layers_for(Overlay overlay, std::string_view species) {
    std::vector<std::string> layers{"planet/ocean", "planet/elevation"};
    const auto add = [&layers](std::initializer_list<std::string_view> names) {
        layers.insert(layers.end(), names.begin(), names.end());
    };
    switch (overlay) {
    case Overlay::Terrain:
        break;
    case Overlay::Biome:
        add({"planet/biome"});
        break;
    case Overlay::Temperature:
        add({"weather/temperature"});
        break;
    case Overlay::Air:
        add({"weather/temperature", "weather/front"});
        break;
    case Overlay::Sky:
        add({"weather/cloud", "weather/rain", "weather/snow", "weather/hail", "weather/storm"});
        break;
    case Overlay::Pressure:
        add({"weather/pressure"});
        break;
    case Overlay::Dryness:
        add({"forest/dryness"});
        break;
    case Overlay::Plants:
        add({"forest/biomass", "forest/capacity"});
        break;
    case Overlay::Fire:
        add({"forest/biomass", "forest/capacity", "forest/fire"});
        break;
    case Overlay::Wildlife:
        layers.emplace_back("planet/area");
        layers.push_back("wildlife/" + std::string{species});
        break;
    }
    return layers;
}

std::optional<Legend> legend_for(Overlay overlay, const Frame& frame, std::string_view species) {
    std::string layer;
    switch (overlay) {
    case Overlay::Terrain:
        layer = "planet/elevation";
        break;
    case Overlay::Biome:
        return std::nullopt;
    case Overlay::Temperature:
    case Overlay::Air:
        layer = "weather/temperature";
        break;
    case Overlay::Sky:
        layer = "weather/rain";
        break;
    case Overlay::Pressure:
        layer = "weather/pressure";
        break;
    case Overlay::Dryness:
        layer = "forest/dryness";
        break;
    case Overlay::Plants:
        layer = "forest/biomass";
        break;
    case Overlay::Fire:
        layer = "forest/fire";
        break;
    case Overlay::Wildlife:
        layer = "wildlife/" + std::string{species};
        break;
    }
    const std::span<const float> values = frame.layer(layer);
    if (values.empty()) {
        return std::nullopt;
    }
    const auto [low, high] = std::ranges::minmax(values);
    const auto& offered = frame.welcome().layers;
    const auto info = std::ranges::find(offered, layer, &probe::LayerInfo::name);
    return Legend{.layer = layer,
                  .unit = info != offered.end() ? info->unit : std::string{},
                  .low = static_cast<double>(low),
                  .high = static_cast<double>(high)};
}

Rgb pressure_colour(double pressure) noexcept {
    return mapgen::ramp(pressure_stops, pressure);
}

Rgb plant_colour(double share) noexcept {
    return mapgen::ramp(plant_stops, share);
}

Rgb fire_colour(Rgb plants, double fire) noexcept {
    if (!(fire > 0.0)) {
        return plants;
    }
    return mapgen::mix(plants, burning, 0.5 + (0.5 * std::min(fire, 1.0)));
}

mapgen::Image render_overlay(const sim::CellGrid& grid, const Frame& frame, Overlay overlay,
                             std::string_view species, int width, jobs::JobSystem& jobs) {
    const mapgen::View view = mapgen::PlanetView{};
    const mapgen::ImageSize size = mapgen::image_size(view, width);
    mapgen::Image image{.width = size.width,
                        .height = size.height,
                        .pixels =
                            std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                      static_cast<std::size_t>(size.height))};
    const Sampler sampler{grid, frame};
    // Densities rather than counts, since cells differ in size.
    const std::vector<float> animals =
        overlay == Overlay::Wildlife
            ? densities(frame, frame.layer("wildlife/" + std::string{species}))
            : std::vector<float>{};
    const double most = animals.empty() ? 0.0 : static_cast<double>(std::ranges::max(animals));
    jobs.parallel_for(
        static_cast<std::uint32_t>(size.height), 1,
        [&](jobs::Range rows, std::uint32_t /*thread*/) {
            for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                const auto y = static_cast<int>(row);
                for (int x = 0; x < size.width; ++x) {
                    // The radius only matters for regional views.
                    const Rgb colour = colour_at(sampler, overlay, animals, most,
                                                 mapgen::pixel_direction(view, size, x, y, 1.0));
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

} // namespace hog::viewer

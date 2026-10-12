#include <hog/mapgen/weather_render.hpp>
#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/weather.hpp>
#include <hog/viewer/ground.hpp>
#include <hog/viewer/overlay.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
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

// Where on the map a pixel samples.
struct Spot {
    DVec3 direction;
    std::optional<std::size_t> sample; // of the surveyed ground, if it reaches here
};

// Reads a frame's layers, and the ground surveyed where there is some, at points on the planet.
class Sampler {
public:
    Sampler(const sim::CellGrid& grid, const Frame& frame, const probe::Ground* ground)
        : grid_(&grid), frame_(&frame), ground_(ground) {}

    [[nodiscard]] Spot spot(DVec2 point) const {
        return Spot{.direction = map_direction(point),
                    .sample = ground_ != nullptr ? sample_at(*ground_, point) : std::nullopt};
    }

    [[nodiscard]] std::span<const float> layer(std::string_view name) const {
        return frame_->layer(name);
    }

    // Smoothed between cells. Precondition: the layer isn't empty.
    [[nodiscard]] double smooth(std::span<const float> values, const Spot& spot) const {
        return grid_->interpolate(spot.direction, [values](std::uint32_t cell) {
            return static_cast<double>(values[cell]);
        });
    }

    // The cell under the point. Precondition: the layer isn't empty.
    [[nodiscard]] double nearest(std::span<const float> values, const Spot& spot) const {
        return static_cast<double>(values[cell(spot)]);
    }

    [[nodiscard]] std::uint32_t cell(const Spot& spot) const {
        return grid_->index(planet::cell_at(spot.direction, grid_->level()));
    }

    // Metres above sea level, where surveyed.
    [[nodiscard]] std::optional<double> elevation(const Spot& spot) const {
        if (!spot.sample) {
            return std::nullopt;
        }
        return static_cast<double>(ground_->elevation[*spot.sample]);
    }

    [[nodiscard]] bool ocean(const Spot& spot) const {
        if (const auto metres = elevation(spot)) {
            return *metres <= 0.0;
        }
        const std::span<const float> ocean = layer("planet/ocean");
        // Smoothed, so coasts run between cells rather than around them.
        return !ocean.empty() && smooth(ocean, spot) > 0.5;
    }

    [[nodiscard]] Rgb ground(const Spot& spot) const {
        if (const auto metres = elevation(spot)) {
            // Land at sea level would take the shallows' colour.
            return mapgen::height_colour(fixed(*metres <= 0.0 ? *metres : std::max(*metres, 1.0)));
        }
        const std::span<const float> elevation = layer("planet/elevation");
        if (elevation.empty()) {
            return missing_colour;
        }
        if (ocean(spot)) {
            return mapgen::height_colour(fixed(ocean_depth));
        }
        return mapgen::height_colour(fixed(std::max(smooth(elevation, spot), 1.0)));
    }

    [[nodiscard]] std::optional<planet::Biome> biome(const Spot& spot) const {
        int index = 0;
        if (spot.sample) {
            index = ground_->biome[*spot.sample];
        } else if (const std::span<const float> biome = layer("planet/biome"); !biome.empty()) {
            index = static_cast<int>(nearest(biome, spot));
        } else {
            return std::nullopt;
        }
        return static_cast<planet::Biome>(std::clamp(index, 0, planet::biome_count - 1));
    }

    // Rock, or the rarest deposit, at the surveyed depth; none where that wasn't surveyed.
    [[nodiscard]] std::optional<Rgb> underground(const Spot& spot) const {
        if (!spot.sample || ground_->rock.empty()) {
            return std::nullopt;
        }
        const int rock = ground_->rock[*spot.sample];
        const int deposit = ground_->deposit[*spot.sample];
        if (rock >= planet::rock_count || deposit > planet::resource_count) {
            return std::nullopt;
        }
        return deposit > 0 ? mapgen::resource_colour(static_cast<planet::Resource>(deposit - 1))
                           : mapgen::rock_colour(static_cast<planet::Rock>(rock));
    }

private:
    const sim::CellGrid* grid_;
    const Frame* frame_;
    const probe::Ground* ground_;
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

Rgb plants_at(const Sampler& sampler, const Spot& spot) {
    const std::span<const float> biomass = sampler.layer("forest/biomass");
    const std::span<const float> capacity = sampler.layer("forest/capacity");
    if (biomass.empty() || capacity.empty()) {
        return missing_colour;
    }
    if (sampler.ocean(spot)) {
        return ocean_blue;
    }
    const std::uint32_t cell = sampler.cell(spot);
    if (!(capacity[cell] > 0.0F)) {
        return mapgen::mix(sampler.ground(spot), {0, 0, 0}, 0.5);
    }
    return plant_colour(static_cast<double>(biomass[cell] / capacity[cell]));
}

Rgb weather_at(const Sampler& sampler, Overlay overlay, const Spot& spot) {
    const auto field = [&](std::string_view name, double& value) {
        const std::span<const float> values = sampler.layer(name);
        if (values.empty()) {
            return false;
        }
        value = sampler.smooth(values, spot);
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
    return mapgen::sky_colour(sampler.ground(spot),
                              static_cast<double>(sim::cloud_cover(static_cast<float>(cloud))),
                              falling, storm);
}

Rgb colour_at(const Sampler& sampler, Overlay overlay, std::span<const float> species, double most,
              const Spot& spot) {
    switch (overlay) {
    case Overlay::Terrain:
        return sampler.ground(spot);
    case Overlay::Biome: {
        const std::optional<planet::Biome> biome = sampler.biome(spot);
        return biome ? mapgen::biome_colour(*biome) : missing_colour;
    }
    case Overlay::Minerals: {
        const std::optional<Rgb> below = sampler.underground(spot);
        if (!below) {
            return missing_colour;
        }
        // Tinted under the sea, so coasts still show.
        return sampler.ocean(spot) ? mapgen::mix(*below, ocean_blue, 0.4) : *below;
    }
    case Overlay::Temperature:
    case Overlay::Air:
    case Overlay::Sky:
    case Overlay::Pressure:
        return weather_at(sampler, overlay, spot);
    case Overlay::Dryness: {
        const std::span<const float> dryness = sampler.layer("forest/dryness");
        if (dryness.empty()) {
            return missing_colour;
        }
        if (sampler.ocean(spot)) {
            return ocean_blue;
        }
        return mapgen::moisture_colour(fixed(1.0 - sampler.smooth(dryness, spot)));
    }
    case Overlay::Plants:
        return plants_at(sampler, spot);
    case Overlay::Fire: {
        const std::span<const float> fire = sampler.layer("forest/fire");
        const Rgb plants = plants_at(sampler, spot);
        return fire.empty() ? missing_colour : fire_colour(plants, sampler.nearest(fire, spot));
    }
    case Overlay::Wildlife: {
        if (species.empty()) {
            return missing_colour;
        }
        const double density = sampler.smooth(species, spot);
        return mapgen::density_colour(sampler.ground(spot), most > 0.0 ? density / most : 0.0);
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
    case Overlay::Minerals:
        return "minerals";
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
    case Overlay::Minerals:
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
    case Overlay::Minerals:
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

mapgen::Image render_overlay(const sim::CellGrid& grid, const Frame& frame,
                             const probe::Ground* ground, Overlay overlay, std::string_view species,
                             const MapRect& shown, int width, jobs::JobSystem& jobs) {
    const mapgen::ImageSize size = mapgen::image_size(mapgen::PlanetView{}, width);
    mapgen::Image image{.width = size.width,
                        .height = size.height,
                        .pixels =
                            std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                      static_cast<std::size_t>(size.height))};
    const Sampler sampler{grid, frame, ground};
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
                    const Rgb colour =
                        colour_at(sampler, overlay, animals, most,
                                  sampler.spot(pixel_point(shown, size.width, size.height, x, y)));
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

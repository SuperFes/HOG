#include <hog/mapgen/weather_render.hpp>
#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/natural.hpp>
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
    DVec3 cells;                       // where the simulation's cells are read
    std::optional<std::size_t> sample; // of the surveyed ground, if it reaches here
};

// Reads a frame's layers, and the ground surveyed where there is some, at points on the planet.
class Sampler {
public:
    Sampler(const sim::CellGrid& grid, const Frame& frame, const probe::Ground* ground,
            bool natural)
        : grid_(&grid), frame_(&frame), ground_(ground),
          reach_(natural ? sim::natural_reach(grid.level()) : 0.0) {}

    [[nodiscard]] Spot spot(DVec2 point) const {
        const DVec3 direction = map_direction(point);
        return Spot{.direction = direction,
                    .cells = reach_ > 0.0
                                 ? sim::natural_point(direction, reach_, frame_->welcome().world_id)
                                 : direction,
                    .sample = ground_ != nullptr ? sample_at(*ground_, point) : std::nullopt};
    }

    [[nodiscard]] std::span<const float> layer(std::string_view name) const {
        return frame_->layer(name);
    }

    // Smoothed between cells, without seams when natural. Precondition: the layer isn't empty.
    [[nodiscard]] double smooth(std::span<const float> values, const Spot& spot) const {
        const auto at = [values](std::uint32_t cell) { return static_cast<double>(values[cell]); };
        return reach_ > 0.0 ? sim::blend(*grid_, spot.cells, at)
                            : grid_->interpolate(spot.cells, at);
    }

    // The cell under the point. Precondition: the layer isn't empty.
    [[nodiscard]] double nearest(std::span<const float> values, const Spot& spot) const {
        return static_cast<double>(values[cell(spot)]);
    }

    [[nodiscard]] std::uint32_t cell(const Spot& spot) const {
        return grid_->index(planet::cell_at(spot.cells, grid_->level()));
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

    // A lake or river over land, or else `under`. Rivers show only where surveyed; lakes show from
    // the cells' biomes elsewhere.
    [[nodiscard]] Rgb with_water(Rgb under, const Spot& spot) const {
        if (ocean(spot)) {
            return under;
        }
        if (biome(spot) == planet::Biome::Lake) {
            return mapgen::biome_colour(planet::Biome::Lake);
        }
        if (spot.sample && ground_->river[*spot.sample] > 0) {
            return mapgen::river_colour(static_cast<double>(ground_->river[*spot.sample]));
        }
        return under;
    }

    // Metres down to the water table, where surveyed or else smoothed between cells, if known.
    [[nodiscard]] std::optional<double> water_table(const Spot& spot) const {
        if (spot.sample) {
            return static_cast<double>(ground_->water_table[*spot.sample]);
        }
        const std::span<const float> depth = layer("planet/water_table");
        if (depth.empty()) {
            return std::nullopt;
        }
        return smooth(depth, spot);
    }

    // An open cave, else the rarest deposit, else the rock, at the surveyed depth; none where that
    // wasn't surveyed. Caves too tight to walk show the rock through them, more so to crawl.
    [[nodiscard]] std::optional<Rgb> underground(const Spot& spot) const {
        if (!spot.sample || ground_->rock.empty() || ground_->cave.empty()) {
            return std::nullopt;
        }
        const int rock = ground_->rock[*spot.sample];
        const int deposit = ground_->deposit[*spot.sample];
        const int cave = ground_->cave[*spot.sample];
        if (rock >= planet::rock_count || deposit > planet::resource_count ||
            cave > planet::cave_count) {
            return std::nullopt;
        }
        const Rgb solid = mapgen::rock_colour(static_cast<planet::Rock>(rock));
        if (cave > 0) {
            const double room = ground_->room[*spot.sample] / 10.0;
            const double through = room < planet::crawl_clearance  ? 0.6
                                   : room < planet::walk_clearance ? 0.3
                                                                   : 0.0;
            return mapgen::mix(mapgen::cave_colour(static_cast<planet::Cave>(cave - 1)), solid,
                               through);
        }
        return deposit > 0 ? mapgen::resource_colour(static_cast<planet::Resource>(deposit - 1))
                           : solid;
    }

    // °C of the rock at the surveyed depth; none where that wasn't surveyed.
    [[nodiscard]] std::optional<double> heat(const Spot& spot) const {
        if (!spot.sample || ground_->heat.empty()) {
            return std::nullopt;
        }
        return static_cast<double>(ground_->heat[*spot.sample]);
    }

private:
    const sim::CellGrid* grid_;
    const Frame* frame_;
    const probe::Ground* ground_;
    double reach_; // of natural edges, or zero for the cells as they are
};

double distance_to_segment(DVec2 point, DVec2 from, DVec2 to) {
    const DVec2 along = to - from;
    const double t = std::clamp(glm::dot(point - from, along) / glm::dot(along, along), 0.0, 1.0);
    return glm::length(point - (from + (along * t)));
}

// How much of the pixel at `offset` from its square's centre an arrow covers, from 0 to 1. The
// arrow points `heading` (right and down on the image) and grows with `speed` in m/s.
double arrow_cover(DVec2 offset, DVec2 heading, double speed, int spacing) {
    constexpr double still = 0.005; // m/s, slower than which no arrow shows
    constexpr double fastest = 0.5; // m/s, from which arrows are longest
    constexpr double half_width = 0.6;
    if (speed < still) {
        return 0.0;
    }
    // Most currents are slow, so lengths grow fastest among them.
    const double length =
        static_cast<double>(spacing) * (0.25 + (0.65 * std::sqrt(std::min(speed / fastest, 1.0))));
    const DVec2 tip = heading * (length / 2.0);
    const DVec2 tail = -tip;
    const double barb = std::max(length * 0.35, 3.0);
    const double turn = 0.5; // radians either side of straight back
    const auto back = [&](double angle) {
        const DVec2 behind = -heading;
        return tip + (DVec2{(behind.x * std::cos(angle)) - (behind.y * std::sin(angle)),
                            (behind.x * std::sin(angle)) + (behind.y * std::cos(angle))} *
                      barb);
    };
    const double away = std::min({distance_to_segment(offset, tail, tip),
                                  distance_to_segment(offset, tip, back(turn)),
                                  distance_to_segment(offset, tip, back(-turn))});
    return std::clamp(1.0 - (away - half_width), 0.0, 1.0);
}

// The sea's current drawn over a pixel: one arrow per square, following the current at the
// square's centre, on the sea only.
Rgb with_arrow(const Sampler& sampler, const MapRect& shown, mapgen::ImageSize size, int x, int y,
               Rgb under) {
    const std::span<const float> east = sampler.layer("ocean/current_east");
    const std::span<const float> north = sampler.layer("ocean/current_north");
    if (east.empty() || north.empty()) {
        return under;
    }
    const int spacing = arrow_spacing(size.width);
    const int centre_x = ((x / spacing) * spacing) + (spacing / 2);
    const int centre_y = ((y / spacing) * spacing) + (spacing / 2);
    const Spot centre =
        sampler.spot(pixel_point(shown, size.width, size.height, centre_x, centre_y));
    if (!sampler.ocean(centre)) {
        return under;
    }
    const DVec2 current{sampler.smooth(east, centre), sampler.smooth(north, centre)};
    const double speed = glm::length(current);
    if (speed == 0.0) {
        return under;
    }
    const DVec2 offset{static_cast<double>(x - centre_x), static_cast<double>(y - centre_y)};
    const double cover = arrow_cover(offset, DVec2{current.x, -current.y} / speed, speed, spacing);
    return cover > 0.0 ? mapgen::mix(under, arrow_colour, cover) : under;
}

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

Rgb sea_at(const Sampler& sampler, const Spot& spot) {
    const std::span<const float> warmth = sampler.layer("ocean/temperature");
    if (warmth.empty()) {
        return missing_colour;
    }
    if (!sampler.ocean(spot)) {
        return sampler.with_water(sampler.ground(spot), spot);
    }
    return mapgen::temperature_colour(fixed(sampler.smooth(warmth, spot)));
}

// Rock, the rarest deposit or the rock's heat at the surveyed depth, tinted under the sea so
// coasts still show.
Rgb underground_at(const Sampler& sampler, Overlay overlay, const Spot& spot) {
    std::optional<Rgb> below;
    if (overlay == Overlay::Heat) {
        const std::optional<double> heat = sampler.heat(spot);
        if (heat) {
            below = mapgen::heat_colour(fixed(*heat));
        }
    } else {
        below = sampler.underground(spot);
    }
    if (!below) {
        return missing_colour;
    }
    return sampler.ocean(spot) ? mapgen::mix(*below, ocean_blue, 0.4) : *below;
}

Rgb colour_at(const Sampler& sampler, Overlay overlay, std::span<const float> species, double most,
              const Spot& spot) {
    switch (overlay) {
    case Overlay::Terrain:
        return sampler.with_water(sampler.ground(spot), spot);
    case Overlay::Biome: {
        const std::optional<planet::Biome> biome = sampler.biome(spot);
        return biome ? sampler.with_water(mapgen::biome_colour(*biome), spot) : missing_colour;
    }
    case Overlay::Water: {
        if (sampler.layer("planet/water_table").empty()) {
            return missing_colour;
        }
        if (sampler.ocean(spot)) {
            return sampler.ground(spot);
        }
        const std::optional<double> depth = sampler.water_table(spot);
        return depth ? sampler.with_water(mapgen::water_table_colour(fixed(*depth)), spot)
                     : missing_colour;
    }
    case Overlay::Minerals:
    case Overlay::Heat:
        return underground_at(sampler, overlay, spot);
    case Overlay::Temperature:
    case Overlay::Air:
    case Overlay::Sky:
    case Overlay::Pressure:
        return weather_at(sampler, overlay, spot);
    case Overlay::Sea:
        return sea_at(sampler, spot);
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
        // Smoothing reaches past the coast; every species lives on land.
        if (sampler.ocean(spot)) {
            return sampler.ground(spot);
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
    case Overlay::Water:
        return "water";
    case Overlay::Minerals:
        return "minerals";
    case Overlay::Heat:
        return "heat";
    case Overlay::Temperature:
        return "temperature";
    case Overlay::Air:
        return "air";
    case Overlay::Sky:
        return "sky";
    case Overlay::Pressure:
        return "pressure";
    case Overlay::Sea:
        return "sea";
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
    case Overlay::Terrain: // for its lakes
    case Overlay::Biome:
        add({"planet/biome"});
        break;
    case Overlay::Water:
        add({"planet/biome", "planet/water_table"});
        break;
    case Overlay::Minerals:
    case Overlay::Heat:
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
    case Overlay::Sea:
        add({"planet/biome", "ocean/temperature", "ocean/current_east", "ocean/current_north"});
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
    case Overlay::Heat:
        return std::nullopt;
    case Overlay::Water:
        layer = "planet/water_table";
        break;
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
    case Overlay::Sea:
        layer = "ocean/temperature";
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
    std::span<const float> values = frame.layer(layer);
    if (values.empty()) {
        return std::nullopt;
    }
    // The sea's range, not the land's, where the sea is known.
    std::vector<float> sea;
    const std::span<const float> ocean = frame.layer("planet/ocean");
    if (overlay == Overlay::Sea && ocean.size() == values.size()) {
        for (std::size_t cell = 0; cell < values.size(); ++cell) {
            if (ocean[cell] > 0.5F) {
                sea.push_back(values[cell]);
            }
        }
        if (!sea.empty()) {
            values = sea;
        }
    }
    const auto [low, high] = std::ranges::minmax(values);
    const auto& offered = frame.welcome().layers;
    const auto info = std::ranges::find(offered, layer, &probe::LayerInfo::name);
    return Legend{.layer = layer,
                  .unit = info != offered.end() ? info->unit : std::string{},
                  .low = static_cast<double>(low),
                  .high = static_cast<double>(high)};
}

int arrow_spacing(int width) noexcept {
    constexpr int across = 48; // arrows across the image
    constexpr int closest = 8;
    return std::max(width / across, closest);
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
                             const MapRect& shown, int width, bool natural, jobs::JobSystem& jobs) {
    const mapgen::ImageSize size = mapgen::image_size(mapgen::PlanetView{}, width);
    mapgen::Image image{.width = size.width,
                        .height = size.height,
                        .pixels =
                            std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                      static_cast<std::size_t>(size.height))};
    const Sampler sampler{grid, frame, ground, natural};
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
                    Rgb colour =
                        colour_at(sampler, overlay, animals, most,
                                  sampler.spot(pixel_point(shown, size.width, size.height, x, y)));
                    if (overlay == Overlay::Sea) {
                        colour = with_arrow(sampler, shown, size, x, y, colour);
                    }
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

#include <hog/core/hash.hpp>
#include <hog/core/time.hpp>
#include <hog/mapgen/photo.hpp>
#include <hog/mapgen/weather_render.hpp>
#include <hog/mapgen/wildlife_render.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/natural.hpp>
#include <hog/sim/weather.hpp>
#include <hog/sim/weather_tuning.hpp>
#include <hog/viewer/ground.hpp>
#include <hog/viewer/overlay.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
constexpr Rgb snow_white{245, 248, 252};
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

// How much of the rock shows through a cave with this many decimetres of room: none where it can
// be walked, more where it must be crawled, and most through a squeeze.
double showing_through(std::uint8_t decimetres) {
    switch (planet::room_for(decimetres / 10.0)) {
    case planet::Room::Walk:
        return 0.0;
    case planet::Room::Crawl:
        return 0.3;
    case planet::Room::Squeeze:
        return 0.6;
    }
    return 0.0;
}

// Reads a frame's layers, and the ground surveyed where there is some, at points on the planet.
class Sampler {
public:
    Sampler(const sim::CellGrid& grid, const Frame& frame, const probe::Ground* ground,
            bool natural)
        : grid_(&grid), frame_(&frame), ground_(ground),
          reach_(natural ? sim::natural_reach(grid.level()) : 0.0) {}

    [[nodiscard]] Spot spot(DVec2 point) const { return spot(map_direction(point), point); }
    [[nodiscard]] Spot spot(DVec3 direction) const {
        return spot(direction, direction_point(direction));
    }

    [[nodiscard]] Spot spot(DVec3 direction, DVec2 point) const {
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

    // Metres the land stands above the sea, and none for the sea, if known.
    [[nodiscard]] std::optional<double> height(const Spot& spot) const {
        if (const auto metres = elevation(spot)) {
            return std::max(*metres, 0.0);
        }
        const std::span<const float> elevation = layer("planet/elevation");
        if (elevation.empty()) {
            return std::nullopt;
        }
        return ocean(spot) ? 0.0 : std::max(smooth(elevation, spot), 0.0);
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

    // Metres down to the water table, where surveyed or else smoothed between cells, if known,
    // risen or fallen with the seasons where the land's water is known.
    [[nodiscard]] std::optional<double> water_table(const Spot& spot) const {
        const std::span<const float> rise = layer("land/water_table_rise");
        const double risen = rise.empty() ? 0.0 : smooth(rise, spot);
        if (spot.sample) {
            return std::max(static_cast<double>(ground_->water_table[*spot.sample]) - risen, 0.0);
        }
        const std::span<const float> depth = layer("planet/water_table");
        if (depth.empty()) {
            return std::nullopt;
        }
        return std::max(smooth(depth, spot) - risen, 0.0);
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
            return mapgen::mix(mapgen::cave_colour(static_cast<planet::Cave>(cave - 1)), solid,
                               showing_through(ground_->room[*spot.sample]));
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

// The sea's surface: deeper is darker, iced over where it's colder than the sea freezes at.
mapgen::Surface sea_surface(const Sampler& sampler, const Spot& spot) {
    mapgen::Surface surface{.biome = planet::Biome::Ocean};
    if (const auto metres = sampler.elevation(spot)) {
        surface.depth = std::max(-*metres, 0.0);
    } else if (const std::span<const float> elevation = sampler.layer("planet/elevation");
               !elevation.empty()) {
        surface.depth = std::max(-sampler.smooth(elevation, spot), 0.0);
    }
    if (const std::span<const float> warmth = sampler.layer("ocean/temperature"); !warmth.empty()) {
        // Over a degree, so the ice's edge isn't a line.
        surface.ice =
            std::clamp(sim::OceanTuning{}.freezing + 0.5 - sampler.smooth(warmth, spot), 0.0, 1.0);
    }
    return surface;
}

// The ground as a camera would see it, missing where the land's heights or kinds aren't known.
Rgb photo_at(const Sampler& sampler, const Spot& spot) {
    if (sampler.layer("planet/elevation").empty() && !spot.sample) {
        return missing_colour;
    }
    if (sampler.ocean(spot)) {
        return mapgen::to_srgb(mapgen::true_colour(sea_surface(sampler, spot)));
    }
    const std::optional<planet::Biome> biome = sampler.biome(spot);
    if (!biome) {
        return missing_colour;
    }
    const auto field = [&](std::string_view name, double otherwise) {
        const std::span<const float> values = sampler.layer(name);
        return values.empty() ? otherwise : sampler.smooth(values, spot);
    };
    const double capacity = field("forest/capacity", 0.0);
    const mapgen::Surface surface{
        // Cells along the coast may be sea, while the point is land.
        .biome = biome == planet::Biome::Ocean ? planet::Biome::Beach : *biome,
        .height = sampler.height(spot).value_or(0.0),
        .plants = capacity > 0.0 ? field("forest/biomass", capacity) / capacity : 1.0,
        .fire = field("forest/fire", 0.0),
        .soil = field("land/soil", 0.0),
        .snow = field("land/snow", 0.0)};
    return mapgen::to_srgb(mapgen::true_colour(surface));
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
    case Overlay::Photo:
        return photo_at(sampler, spot);
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
    case Overlay::Soil: {
        const std::span<const float> soil = sampler.layer("land/soil");
        const std::span<const float> snow = sampler.layer("land/snow");
        if (soil.empty() || snow.empty()) {
            return missing_colour;
        }
        if (sampler.ocean(spot)) {
            return ocean_blue;
        }
        return sampler.with_water(
            snow_colour(mapgen::moisture_colour(fixed(sampler.smooth(soil, spot))),
                        sampler.smooth(snow, spot)),
            spot);
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

// What an overlay colours each point of the planet by.
struct Painting {
    Painting(const sim::CellGrid& grid, const Frame& frame, const probe::Ground* ground,
             Overlay shown, std::string_view species, bool natural)
        : sampler{grid, frame, ground, natural}, overlay{shown},
          // Densities rather than counts, since cells differ in size.
          animals{overlay == Overlay::Wildlife
                      ? densities(frame, frame.layer("wildlife/" + std::string{species}))
                      : std::vector<float>{}},
          most{animals.empty() ? 0.0 : static_cast<double>(std::ranges::max(animals))} {}

    [[nodiscard]] Rgb colour(const Spot& spot) const {
        return colour_at(sampler, overlay, animals, most, spot);
    }

    Sampler sampler;
    Overlay overlay;
    std::vector<float> animals;
    double most;
};

mapgen::Image blank(mapgen::ImageSize size) {
    return {.width = size.width,
            .height = size.height,
            .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                static_cast<std::size_t>(size.height))};
}

void put(mapgen::Image& image, int x, int y, Rgb colour) {
    const auto offset = 3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)) +
                             static_cast<std::size_t>(x));
    std::ranges::copy(colour, image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Where clouds gather and part: a field of noise for each hour of the world's time, fading into
// the next through the hour, so clouds never repeat and hold still while the clock does.
class CloudNoise {
public:
    CloudNoise(std::uint64_t world, GameTime time, double across)
        : across_{across}, next_{eased(into_hour(time))},
          // Two fields blended spread less than either, so the blend is widened back.
          spread_{std::sqrt(((1.0 - next_) * (1.0 - next_)) + (next_ * next_))},
          this_seed_{hash_values(world, hour(time))},
          next_seed_{hash_values(world, hour(time) + 1)} {}

    [[nodiscard]] double operator()(DVec3 metres) const noexcept {
        const double now = mapgen::cloud_noise(this_seed_, metres, across_);
        const double later = next_ > 0.0 ? mapgen::cloud_noise(next_seed_, metres, across_) : 0.0;
        return (((1.0 - next_) * now) + (next_ * later)) / spread_;
    }

private:
    static std::uint64_t hour(GameTime time) noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::floor<std::chrono::hours>(time.time_since_epoch()).count());
    }
    static double into_hour(GameTime time) noexcept {
        const GameDuration since = time.time_since_epoch();
        return std::chrono::duration<double, std::chrono::hours::period>(
                   since - std::chrono::floor<std::chrono::hours>(since))
            .count();
    }
    static double eased(double share) noexcept { return share * share * (3.0 - (2.0 * share)); }

    double across_;
    double next_; // how far into the next hour's field, 0 to 1
    double spread_;
    std::uint64_t this_seed_;
    std::uint64_t next_seed_;
};

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
    case Overlay::Photo:
        return "photo";
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
    case Overlay::Soil:
        return "soil";
    case Overlay::Plants:
        return "plants";
    case Overlay::Fire:
        return "fire";
    case Overlay::Wildlife:
        return "wildlife";
    }
    return "unknown";
}

std::vector<std::string> layers_for(Overlay overlay, std::string_view species, bool clouds) {
    std::vector<std::string> layers{"planet/ocean", "planet/elevation"};
    const auto add = [&layers](std::initializer_list<std::string_view> names) {
        for (const std::string_view name : names) {
            if (std::ranges::find(layers, name) == layers.end()) {
                layers.emplace_back(name);
            }
        }
    };
    if (clouds) {
        add({"weather/cloud", "weather/storm"});
    }
    switch (overlay) {
    case Overlay::Terrain: // for its lakes
    case Overlay::Biome:
        add({"planet/biome"});
        break;
    case Overlay::Photo:
        add({"planet/biome", "forest/biomass", "forest/capacity", "forest/fire", "land/soil",
             "land/snow", "ocean/temperature"});
        break;
    case Overlay::Water:
        add({"planet/biome", "planet/water_table", "land/water_table_rise"});
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
    case Overlay::Soil:
        add({"planet/biome", "land/soil", "land/snow"});
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
    case Overlay::Photo:
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
    case Overlay::Soil:
        layer = "land/soil";
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

Rgb snow_colour(Rgb ground, double snow) noexcept {
    if (!(snow > 0.0)) {
        return ground;
    }
    return mapgen::mix(ground, snow_white, snow / (snow + sim::LandTuning{}.covering_snow));
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
    mapgen::Image image = blank(size);
    const Painting painting{grid, frame, ground, overlay, species, natural};
    jobs.parallel_for(static_cast<std::uint32_t>(size.height), 1,
                      [&](jobs::Range rows, std::uint32_t /*thread*/) {
                          for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                              const auto y = static_cast<int>(row);
                              for (int x = 0; x < size.width; ++x) {
                                  Rgb colour = painting.colour(painting.sampler.spot(
                                      pixel_point(shown, size.width, size.height, x, y)));
                                  if (overlay == Overlay::Sea) {
                                      colour =
                                          with_arrow(painting.sampler, shown, size, x, y, colour);
                                  }
                                  put(image, x, y, colour);
                              }
                          }
                      });
    return image;
}

mapgen::Image render_faces(const sim::CellGrid& grid, const Frame& frame,
                           const probe::Ground* ground, Overlay overlay, std::string_view species,
                           int size, bool natural, jobs::JobSystem& jobs) {
    mapgen::Image image = blank({.width = size, .height = planet::face_count * size});
    const Painting painting{grid, frame, ground, overlay, species, natural};
    jobs.parallel_for(
        static_cast<std::uint32_t>(image.height), 1,
        [&](jobs::Range rows, std::uint32_t /*thread*/) {
            for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                const auto y = static_cast<int>(row);
                const int face = y / size;
                const double v = planet::st_to_uv(((y % size) + 0.5) / size);
                for (int x = 0; x < size; ++x) {
                    const double u = planet::st_to_uv((x + 0.5) / size);
                    put(image, x, y,
                        painting.colour(painting.sampler.spot(planet::face_uv_to_xyz(face, u, v))));
                }
            }
        });
    return image;
}

std::vector<float> render_heights(const sim::CellGrid& grid, const Frame& frame,
                                  const probe::Ground* ground, int steps, bool natural,
                                  jobs::JobSystem& jobs) {
    const auto across = static_cast<std::size_t>(steps) + 1;
    std::vector<float> heights(planet::face_count * across * across);
    const Sampler sampler{grid, frame, ground, natural};
    jobs.parallel_for(
        static_cast<std::uint32_t>(planet::face_count * across), 1,
        [&](jobs::Range rows, std::uint32_t /*thread*/) {
            for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                const auto face = static_cast<int>(row / across);
                const double v = planet::st_to_uv(static_cast<double>(row % across) / steps);
                for (std::size_t column = 0; column < across; ++column) {
                    const double u = planet::st_to_uv(static_cast<double>(column) / steps);
                    const Spot spot = sampler.spot(planet::face_uv_to_xyz(face, u, v));
                    heights[(row * across) + column] =
                        static_cast<float>(sampler.height(spot).value_or(0.0));
                }
            }
        });
    return heights;
}

std::vector<std::uint8_t> render_clouds(const sim::CellGrid& grid, const Frame& frame, int size,
                                        bool natural, jobs::JobSystem& jobs) {
    const std::span<const float> cloud = frame.layer("weather/cloud");
    const std::span<const float> storm = frame.layer("weather/storm");
    if (cloud.empty() || storm.empty()) {
        return {};
    }
    const Sampler sampler{grid, frame, nullptr, natural};
    const double radius = frame.welcome().planet_radius > 0.0 ? frame.welcome().planet_radius : 1.0;
    // Two of the grid's cells a noise lattice cell, as the photos have it.
    const double across =
        2.0 * radius * std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid.size()));
    const CloudNoise noise{frame.welcome().world_id, frame.time(), across};
    const auto width = static_cast<std::size_t>(size);
    std::vector<std::uint8_t> texels(std::size_t{4} * planet::face_count * width * width);
    jobs.parallel_for(
        static_cast<std::uint32_t>(planet::face_count * size), 1,
        [&](jobs::Range rows, std::uint32_t /*thread*/) {
            for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                const auto y = static_cast<int>(row);
                const double v = planet::st_to_uv(((y % size) + 0.5) / size);
                for (int x = 0; x < size; ++x) {
                    const DVec3 direction = glm::normalize(
                        planet::face_uv_to_xyz(y / size, planet::st_to_uv((x + 0.5) / size), v));
                    const Spot spot = sampler.spot(direction);
                    const auto cover = static_cast<double>(
                        sim::cloud_cover(static_cast<float>(sampler.smooth(cloud, spot))));
                    const mapgen::Cloud over = mapgen::cloud_over(cover, noise(direction * radius));
                    const Rgb colour =
                        mapgen::to_srgb(mapgen::cloud_colour(over, sampler.smooth(storm, spot)));
                    const std::size_t at =
                        4 * ((std::size_t{row} * width) + static_cast<std::size_t>(x));
                    std::ranges::copy(colour, texels.begin() + static_cast<std::ptrdiff_t>(at));
                    texels[at + 3] = static_cast<std::uint8_t>(
                        std::lround(255.0 * std::clamp(over.opacity(), 0.0, 1.0)));
                }
            }
        });
    return texels;
}

} // namespace hog::viewer

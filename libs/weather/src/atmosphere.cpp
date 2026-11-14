#include <hog/globe/cell.hpp>
#include <hog/weather/atmosphere.hpp>
#include <hog/weather/wind.hpp>

#include <glm/geometric.hpp>

namespace hog::weather {
namespace {

// Cloud water, as a share of what the air could hold as vapour, at which most of a layer's sky
// is covered: warm air holds far more, so it needs far more cloud to close over than cold.
constexpr double cover_spread = 0.03;
// Kelvin over and under freezing between all rain and all snow.
constexpr double freezing = 273.15;
constexpr double sleet_span = 4.0;

// The unit vectors east and north along the ground at `up`.
struct Frame {
    DVec3 east;
    DVec3 north;
};

Frame frame_at(DVec3 up) {
    DVec3 east = glm::cross(DVec3{0.0, 0.0, 1.0}, up);
    east = glm::length(east) < 1e-9 ? DVec3{0.0, 1.0, 0.0} : glm::normalize(east);
    return {.east = east, .north = glm::cross(up, east)};
}

Vec2 along(const Frame& frame, Vec3 wind) {
    const DVec3 w{wind};
    return {static_cast<float>(glm::dot(w, frame.east)),
            static_cast<float>(glm::dot(w, frame.north))};
}

// How much of a layer's sky its cloud covers, from 0 to 1, as a share of the vapour its air could
// hold, `held`.
double cover_of(double cloud, double held) {
    return cloud > 0.0 && held > 0.0 ? 1.0 - std::exp(-cloud / (cover_spread * held)) : 0.0;
}

// Relative humidity, from 0 to 1, of air holding `vapour` that could hold `held`.
float humidity_of(double vapour, double held) {
    return held > 0.0 ? static_cast<float>(std::clamp(vapour / held, 0.0, 1.0)) : 0.0F;
}

// How much of what falls on a column falls as snow, from 0 to 1: snow where the ground's air is
// below freezing, easing to rain over a few kelvin; `none` for a column that's ground through.
float snow_share(const Field& field, std::size_t column, float none) {
    for (int layer = 0; layer < field.grid.layers; ++layer) {
        if (!field.solid(column, layer)) {
            const double cold = freezing - static_cast<double>(field.at(column, layer).temperature);
            return static_cast<float>(std::clamp(0.5 + (cold / sleet_span), 0.0, 1.0));
        }
    }
    return none;
}

} // namespace

Atmosphere::Atmosphere(const globe::Generator& generator, int level, int layers)
    : planet_(generator.planet()), field_(start_field(generator, level, layers)),
      stencil_(field_.grid) {}

void Atmosphere::advance_once(DVec3 sun, double seconds) {
    warm_surface(field_, planet_, sun, seconds);
    step(field_, stencil_, planet_, seconds);
}

Column Atmosphere::column(std::size_t column) const {
    const Grid& grid = field_.grid;
    const DVec3 up = globe::center(globe::cell_at_index(grid.level, column));
    const Frame frame = frame_at(up);
    const auto surface = static_cast<double>(field_.surface[column]);

    Column shown;
    shown.sea = field_.over_sea[column] != 0;
    // Layers of cloud touching overlap as much as they can; those with clear air between, as
    // chance has it.
    double clear = 1.0;
    double run = 0.0;
    double carried = 0.0;
    Vec3 push{0.0F};
    bool lowest = true;
    shown.layers.resize(static_cast<std::size_t>(grid.layers));
    int first_open = grid.layers;
    for (int layer = 0; layer < grid.layers; ++layer) {
        if (field_.solid(column, layer)) {
            continue;
        }
        first_open = std::min(first_open, layer);
        const Air& air = field_.at(column, layer);
        const double height = grid.radius_of(layer) - planet_.body.radius;
        const double pressure = pressure_at(planet_, surface, height);
        const auto temperature = static_cast<double>(air.temperature);
        const double mass = air_density(temperature, pressure) * grid.depth_of(layer);
        const auto cloud = static_cast<double>(air.cloud);
        const double held_here = saturation_ratio(temperature, pressure);
        const double covered = cover_of(cloud, held_here);
        if (covered > 0.0) {
            run = std::max(run, covered);
        } else if (run > 0.0) {
            clear *= 1.0 - run;
            run = 0.0;
        }

        if (lowest) {
            shown.wind = along(frame, air.wind);
            const double held = saturation_ratio(temperature, pressure);
            shown.humidity = humidity_of(static_cast<double>(air.vapour), held);
        }
        lowest = false;
        shown.layers[static_cast<std::size_t>(layer)] = {.cover = static_cast<float>(covered),
                                                         .cloud = static_cast<float>(cloud * mass)};
        shown.cloud += static_cast<float>(cloud * mass);
        carried += cloud * mass;
        push += Vec3{static_cast<float>(cloud * mass)} * air.wind;
    }
    for (int layer = 0; layer < first_open; ++layer) {
        shown.layers[static_cast<std::size_t>(layer)] =
            shown.layers[static_cast<std::size_t>(std::min(first_open, grid.layers - 1))];
    }
    shown.cover = static_cast<float>(1.0 - (clear * (1.0 - run)));
    if (carried > 0.0) {
        shown.drift = along(frame, push / static_cast<float>(carried));
    } else {
        shown.drift = shown.wind;
    }

    shown.falling = field_.precipitation[column];
    shown.snow = snow_share(field_, column, shown.snow);
    return shown;
}

} // namespace hog::weather

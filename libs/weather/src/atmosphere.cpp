#include <hog/globe/cell.hpp>
#include <hog/weather/atmosphere.hpp>
#include <hog/weather/wind.hpp>

#include <glm/geometric.hpp>

namespace hog::weather {
namespace {

// Condensate that makes air visibly cloud, per kilogram of dry air.
constexpr double visible = 5e-5;
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

} // namespace

Atmosphere::Atmosphere(const globe::Generator& generator, double day, int level, int layers)
    : planet_(generator.planet()), field_(start_field(generator, level, layers)),
      stencil_(field_.grid) {
    planet_.day = day;
}

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
    double carried = 0.0;
    Vec3 push{0.0F};
    bool lowest = true;
    bool grounded = true;
    for (int layer = 0; layer < grid.layers; ++layer) {
        if (field_.solid(column, layer)) {
            continue;
        }
        const Air& air = field_.at(column, layer);
        const double height = grid.radius_of(layer) - planet_.radius;
        const double pressure = pressure_at(planet_, surface, height);
        const auto temperature = static_cast<double>(air.temperature);
        const double mass = air_density(temperature, pressure) * grid.thickness;
        const auto cloud = static_cast<double>(air.cloud);

        if (lowest) {
            shown.wind = along(frame, air.wind);
            const double held = saturation_ratio(planet_.volatiles, temperature, pressure);
            shown.humidity = held > 0.0 ? static_cast<float>(std::clamp(
                                              static_cast<double>(air.vapour) / held, 0.0, 1.0))
                                        : 0.0F;
        }
        const bool misty = cloud > visible;
        if (misty && grounded) {
            shown.fog_top = static_cast<float>(grid.floor_of(layer + 1) - planet_.radius);
        } else {
            grounded = false;
            if (misty) {
                const auto floor = static_cast<float>(grid.floor_of(layer) - planet_.radius);
                if (shown.top == 0.0F) {
                    shown.base = floor;
                }
                shown.top = static_cast<float>(grid.floor_of(layer + 1) - planet_.radius);
            }
        }
        lowest = false;
        shown.cloud += static_cast<float>(cloud * mass);
        carried += cloud * mass;
        push += Vec3{static_cast<float>(cloud * mass)} * air.wind;
    }
    if (carried > 0.0) {
        shown.drift = along(frame, push / static_cast<float>(carried));
    } else {
        shown.drift = shown.wind;
    }

    shown.falling = field_.precipitation[column];
    // Snow where the ground's air is below freezing, easing to rain over a few kelvin.
    for (int layer = 0; layer < grid.layers; ++layer) {
        if (!field_.solid(column, layer)) {
            const double cold =
                freezing - static_cast<double>(field_.at(column, layer).temperature);
            shown.snow = static_cast<float>(std::clamp(0.5 + (cold / sleet_span), 0.0, 1.0));
            break;
        }
    }
    return shown;
}

} // namespace hog::weather

#include <hog/globe/cell.hpp>
#include <hog/weather/atmosphere.hpp>
#include <hog/weather/wind.hpp>

#include <glm/geometric.hpp>

namespace hog::weather {
namespace {

// Condensate that makes air visibly cloud, per kilogram of dry air; a layer holding twice that is
// cloud through and through.
constexpr double visible = 5e-5;
constexpr double full = 2.0 * visible;
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

// How much of a layer is cloud, from 0 to 1, taking it as filling the layer evenly once it holds
// `full`; none in the ground.
double filling(const Field& field, std::size_t column, int layer) {
    if (layer < 0 || layer >= field.grid.layers || field.solid(column, layer)) {
        return 0.0;
    }
    return std::clamp(static_cast<double>(field.at(column, layer).cloud) / full, 0.0, 1.0);
}

// Where a cloud's edge lies, metres from the centre, between the middles of a layer that's cloud
// (half filled or more) and one beside it that isn't, as a straight line between how filled they
// are crosses a half: a thin cloud's edges close in on its middle, and a thick one's fall at the
// layers' edges.
double edge_between(const Grid& grid, int inside, int outside, double inner, double outer) {
    const double across = inner > outer ? (inner - 0.5) / (inner - outer) : 0.5;
    // Half way across is the shared edge whatever the layers' depths, so each half of the span is
    // scaled by its own layer's.
    const double sign = static_cast<double>(outside - inside);
    const double inner_half = 0.5 * grid.depth_of(inside);
    const double outer_half = 0.5 * grid.depth_of(outside);
    const double reach = across <= 0.5 ? 2.0 * across * inner_half
                                       : inner_half + (2.0 * (across - 0.5) * outer_half);
    return grid.radius_of(inside) + (sign * reach);
}

double edge_above(const Field& field, std::size_t column, int layer) {
    const Grid& grid = field.grid;
    if (layer + 1 >= grid.layers) {
        return grid.top();
    }
    return edge_between(grid, layer, layer + 1, filling(field, column, layer),
                        filling(field, column, layer + 1));
}

double edge_below(const Field& field, std::size_t column, int layer) {
    const Grid& grid = field.grid;
    if (layer == 0 || field.solid(column, layer - 1)) {
        return grid.floor_of(layer);
    }
    return edge_between(grid, layer, layer - 1, filling(field, column, layer),
                        filling(field, column, layer - 1));
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
    for (int layer = 0; layer < grid.layers; ++layer) {
        if (field_.solid(column, layer)) {
            continue;
        }
        const Air& air = field_.at(column, layer);
        const double height = grid.radius_of(layer) - planet_.body.radius;
        const double pressure = pressure_at(planet_, surface, height);
        const auto temperature = static_cast<double>(air.temperature);
        const double mass = air_density(temperature, pressure) * grid.depth_of(layer);
        const auto cloud = static_cast<double>(air.cloud);
        const double held_here = saturation_ratio(temperature, pressure);
        const double covered = cloud > 0.0 && held_here > 0.0
                                   ? 1.0 - std::exp(-cloud / (cover_spread * held_here))
                                   : 0.0;
        if (covered > 0.0) {
            run = std::max(run, covered);
        } else if (run > 0.0) {
            clear *= 1.0 - run;
            run = 0.0;
        }

        if (lowest) {
            shown.wind = along(frame, air.wind);
            const double held = saturation_ratio(temperature, pressure);
            shown.humidity = held > 0.0 ? static_cast<float>(std::clamp(
                                              static_cast<double>(air.vapour) / held, 0.0, 1.0))
                                        : 0.0F;
        }
        if (cloud > visible) {
            if (shown.top == 0.0F) {
                const double sunk = lowest ? grid.depth_of(layer) : 0.0;
                shown.base = static_cast<float>(edge_below(field_, column, layer) - sunk -
                                                planet_.body.radius);
            }
            shown.top = static_cast<float>(edge_above(field_, column, layer) - planet_.body.radius);
        }
        lowest = false;
        shown.cloud += static_cast<float>(cloud * mass);
        carried += cloud * mass;
        push += Vec3{static_cast<float>(cloud * mass)} * air.wind;
    }
    shown.cover = static_cast<float>(1.0 - (clear * (1.0 - run)));
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

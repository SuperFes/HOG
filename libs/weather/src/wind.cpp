#include <hog/core/profile.hpp>
#include <hog/globe/cell.hpp>
#include <hog/weather/moisture.hpp>
#include <hog/weather/wind.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

namespace hog::weather {
namespace {

// Radiation draws the air back to its equilibrium over weeks aloft and days by the ground; the
// ground slows air within the lowest 30% of its pressure, in about a day.
constexpr double free_relaxation = 1.0 / (40.0 * 86'400.0);
constexpr double ground_relaxation = 1.0 / (4.0 * 86'400.0);
constexpr double drag = 1.0 / 86'400.0;
constexpr double boundary_pressure = 0.7;
// Cells across the eddies too small for the grid, whose shedding of the wind's energy is u³ over
// it.
constexpr double eddy_cells = 4.0;

// The temperature the air at a cell would have brought down to the reference level without
// heat: what rising and sinking leave unchanged.
double potential(const Field& field, const globe::Planet& planet, std::size_t cell) {
    const int layer = static_cast<int>(cell % static_cast<std::size_t>(field.grid.layers));
    return static_cast<double>(field.air[cell].temperature) +
           (dry_lapse_rate(planet) * (field.grid.radius_of(layer) - planet.body.radius));
}

// How much lighter than dry air at its temperature the air is: vapour is lighter, drops and cloud
// weigh it down.
double lightness(const Air& air) {
    constexpr double vapour_excess = 0.608;
    return (vapour_excess * static_cast<double>(air.vapour)) - static_cast<double>(air.cloud) -
           static_cast<double>(air.rain);
}

bool open(const Field& field, const Link& link) {
    return link.cell != Link::none && field.blocked[link.cell] == 0;
}

Vec3 rotate_about_z(Vec3 v, double angle) {
    const auto c = static_cast<float>(std::cos(angle));
    const auto s = static_cast<float>(std::sin(angle));
    return {(v.x * c) - (v.y * s), (v.x * s) + (v.y * c), v.z};
}

// The bit of each of a cell's links that is closed to it.
std::uint8_t closed_links(const Field& field, const Stencil& stencil, std::size_t cell) {
    std::uint8_t closed = 0;
    const auto& links = stencil.links(cell);
    for (std::size_t i = 0; i < links.size(); ++i) {
        closed |= static_cast<std::uint8_t>(open(field, links[i]) ? 0U : 1U << i);
    }
    return closed;
}

// Per open cell, for each link, the direction a pressure's difference along that link pushes the
// cell: the link's direction through the least-squares gradient's matrix, so that the slope they
// sum to is the pressure's true one however the neighbours lie, and the gradient's transpose, the
// divergence, has the same weights. Across an axis with a neighbour either side that's half the
// direction, and against the ground with one, the whole.
using Pushes = std::vector<std::array<Vec3, links_per_cell>>;

Pushes pushes_of(const Field& field, const Stencil& stencil) {
    Pushes found(field.air.size());
    for (std::size_t cell = 0; cell < found.size(); ++cell) {
        if (field.blocked[cell] != 0) {
            continue;
        }
        const auto& links = stencil.links(cell);
        const std::uint8_t closed = closed_links(field, stencil, cell);
        const Mat3 matrix = gradient_matrix(links, closed);
        for (std::size_t i = 0; i < links.size(); ++i) {
            found[cell][i] = matrix * links[i].direction;
        }
    }
    return found;
}

// The slope of a pressure at each open cell: the differences to its open neighbours along the
// directions between them.
void gradient(const Field& field, const Stencil& stencil, const Pushes& pushes,
              const std::vector<double>& pressure, std::vector<DVec3>& out) {
    for (std::size_t cell = 0; cell < out.size(); ++cell) {
        out[cell] = DVec3{0.0};
        if (field.blocked[cell] != 0) {
            continue;
        }
        const auto& links = stencil.links(cell);
        for (std::size_t i = 0; i < links.size(); ++i) {
            if (open(field, links[i])) {
                out[cell] += DVec3{pushes[cell][i]} * ((pressure[links[i].cell] - pressure[cell]) /
                                                       static_cast<double>(links[i].distance));
            }
        }
    }
}

// The gradient's transpose: minus the divergence, so that pressure and wind pair off exactly and
// a pressure solved for removes all of the divergence it was solved against.
void adjoint(const Field& field, const Stencil& stencil, const Pushes& pushes,
             const std::vector<DVec3>& wind, std::vector<double>& out) {
    std::ranges::fill(out, 0.0);
    for (std::size_t cell = 0; cell < out.size(); ++cell) {
        if (field.blocked[cell] != 0) {
            continue;
        }
        const auto& links = stencil.links(cell);
        for (std::size_t i = 0; i < links.size(); ++i) {
            if (!open(field, links[i])) {
                continue;
            }
            const double along = glm::dot(wind[cell], DVec3{pushes[cell][i]}) /
                                 static_cast<double>(links[i].distance);
            out[cell] -= along;
            out[links[i].cell] += along;
        }
    }
}

// Kilograms a cubic metre of each cell's air weighs in the reference state its column holds, which
// only depends on height: the flow is balanced against that, and what it carries is counted by it,
// so warming a cell never makes it a heavier or lighter thing to conserve.
std::vector<double> densities(const Field& field, const globe::Planet& planet) {
    std::vector<double> found(field.air.size(), 1.0);
    const auto layers = static_cast<std::size_t>(field.grid.layers);
    for (std::size_t cell = 0; cell < found.size(); ++cell) {
        const std::size_t column = cell / layers;
        const double height =
            field.grid.radius_of(static_cast<int>(cell % layers)) - planet.body.radius;
        const auto surface = static_cast<double>(field.surface[column]);
        found[cell] = air_density(air_temperature(planet, surface, height),
                                  pressure_at(planet, surface, height));
    }
    return found;
}

// Kilograms of dry air each cell holds, which water is counted per kilogram of.
std::vector<double> masses(const Field& field, const globe::Planet& planet) {
    std::vector<double> found = densities(field, planet);
    const auto layers = static_cast<std::size_t>(field.grid.layers);
    std::vector<double> angles(field.grid.columns());
    for (std::size_t column = 0; column < angles.size(); ++column) {
        angles[column] = globe::solid_angle(globe::cell_at_index(field.grid.level, column));
    }
    for (std::size_t cell = 0; cell < found.size(); ++cell) {
        const double radius = field.grid.radius_of(static_cast<int>(cell % layers));
        found[cell] *= field.blocked[cell] != 0
                           ? 0.0
                           : angles[cell / layers] * radius * radius *
                                 field.grid.depth_of(static_cast<int>(cell % layers));
    }
    return found;
}

// Scales what the carrying gave `next` of one species so its total is what `field` held: tracing
// back along the wind gives and takes a little water at every step, which would sum to storms
// out of nothing.
template <typename Member>
void keep_total(const Field& field, std::vector<Air>& next, const std::vector<double>& mass,
                Member member) {
    double before = 0.0;
    double after = 0.0;
    for (std::size_t cell = 0; cell < mass.size(); ++cell) {
        before += static_cast<double>(field.air[cell].*member) * mass[cell];
        after += static_cast<double>(next[cell].*member) * mass[cell];
    }
    if (after <= 0.0) {
        return;
    }
    const auto scale = static_cast<float>(before / after);
    for (Air& air : next) {
        air.*member *= scale;
    }
}

// The mass flux of each open cell's air: its wind times its density.
std::vector<DVec3> flux_of(const Field& field, const std::vector<double>& density) {
    std::vector<DVec3> found(field.air.size());
    for (std::size_t cell = 0; cell < found.size(); ++cell) {
        found[cell] =
            field.blocked[cell] != 0 ? DVec3{0.0} : DVec3{field.air[cell].wind} * density[cell];
    }
    return found;
}

// Per cell, the divergence of the wind.
std::vector<double> divergence_of(const Field& field, const Stencil& stencil,
                                  const globe::Planet& planet) {
    std::vector<double> found(field.air.size());
    adjoint(field, stencil, pushes_of(field, stencil), flux_of(field, densities(field, planet)),
            found);
    for (double& value : found) {
        value = -value;
    }
    return found;
}

// The operator the pressure solves: the gradient's transpose of the gradient.
void apply_laplacian(const Field& field, const Stencil& stencil, const Pushes& pushes,
                     const std::vector<double>& in, std::vector<double>& out,
                     std::vector<DVec3>& slope) {
    gradient(field, stencil, pushes, in, slope);
    adjoint(field, stencil, pushes, slope, out);
}

double dot(const std::vector<double>& a, const std::vector<double>& b) {
    double total = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        total += a[i] * b[i];
    }
    return total;
}

// Where a parcel of air now at a cell's middle was a moment ago, as a cell and the offset of the
// point from its middle.
struct Departure {
    std::size_t cell = 0;
    Vec3 offset{0.0F};
    Vec3 up{0.0F}; // the direction up there, if it came from elsewhere
    bool moved = false;
};

Vec3 middle_of(const Field& field, const Stencil& stencil, std::size_t cell) {
    const std::size_t column = cell / static_cast<std::size_t>(field.grid.layers);
    const int layer = static_cast<int>(cell % static_cast<std::size_t>(field.grid.layers));
    return stencil.up(column) * static_cast<float>(field.grid.radius_of(layer));
}

std::optional<std::size_t> locate(const Field& field, DVec3 point) {
    const double radius = glm::length(point);
    const auto layer = field.grid.layer_at(radius);
    if (!layer) {
        return std::nullopt;
    }
    const auto column = static_cast<std::size_t>(
        globe::index_of(globe::cell_under(point / radius, field.grid.level)));
    const std::size_t cell = field.grid.index(column, *layer);
    return field.blocked[cell] != 0 ? std::nullopt : std::optional{cell};
}

// A value at a point near a cell's middle, from the cell's and its neighbours' by the gradient
// they make, kept between theirs so it makes no new extremes.
template <typename Value, typename Get>
Value reconstruct(const Field& field, const Stencil& stencil, std::size_t cell, Vec3 offset,
                  Get get) {
    const Value centre = get(cell);
    Value low = centre;
    Value high = centre;
    Value change{};
    const auto& links = stencil.links(cell);
    std::uint8_t closed = 0;
    for (std::size_t i = 0; i < links.size(); ++i) {
        closed |= static_cast<std::uint8_t>(open(field, links[i]) ? 0U : 1U << i);
    }
    const Vec3 along = gradient_weights(links, closed, offset);
    for (const Link& link : links) {
        if (!open(field, link)) {
            continue;
        }
        const Value other = get(link.cell);
        low = glm::min(low, other);
        high = glm::max(high, other);
        change += (other - centre) * (glm::dot(link.direction, along) / link.distance);
    }
    return glm::clamp(centre + change, low, high);
}

// Turns a vector at one place on the sphere to lie as it did there at another.
Vec3 carry(Vec3 v, Vec3 from, Vec3 to) {
    const Vec3 axis = glm::cross(from, to);
    const float sine = glm::length(axis);
    if (sine < 1e-9F) {
        return v;
    }
    const Vec3 k = axis / sine;
    const float cosine = glm::dot(from, to);
    return (v * cosine) + (glm::cross(k, v) * sine) + (k * glm::dot(k, v) * (1.0F - cosine));
}

// The wind at a point near a cell's middle, as reconstruct has it, made in the cell's own frame:
// each neighbour's turned to lie as it would here, and kept between theirs across and up apart,
// so winds along the ground about it never sum to one rising or sinking wherever the cell is.
Vec3 wind_near(const Field& field, const Stencil& stencil, std::size_t cell, Vec3 offset) {
    const auto layers = static_cast<std::size_t>(field.grid.layers);
    const Vec3 up = stencil.up(cell / layers);
    const Vec3 first = stencil.links(cell)[0].direction;
    const Vec3 second = glm::cross(up, first);
    const Vec3 made = reconstruct<Vec3>(field, stencil, cell, offset, [&](std::size_t at) {
        const Vec3 wind = carry(field.air[at].wind, stencil.up(at / layers), up);
        return Vec3{glm::dot(wind, first), glm::dot(wind, second), glm::dot(wind, up)};
    });
    return (first * made.x) + (second * made.y) + (up * made.z);
}

Departure trace(const Field& field, const Stencil& stencil, std::size_t cell, double seconds) {
    const Vec3 here = middle_of(field, stencil, cell);
    const DVec3 start{here};
    const DVec3 half = start - (0.5 * seconds * DVec3{field.air[cell].wind});
    const auto midway = locate(field, half);
    if (!midway) {
        return {.cell = cell};
    }
    const Vec3 offset{half - DVec3{middle_of(field, stencil, *midway)}};
    const DVec3 back = start - (seconds * DVec3{wind_near(field, stencil, *midway, offset)});
    const auto there = locate(field, back);
    if (!there) {
        return {.cell = cell};
    }
    return {.cell = *there,
            .offset = Vec3{back - DVec3{middle_of(field, stencil, *there)}},
            .up = Vec3{glm::normalize(back)},
            .moved = true};
}

} // namespace

void accelerate(Field& field, const Stencil& stencil, const globe::Planet& planet, double seconds) {
    const Grid& grid = field.grid;
    const auto layers = static_cast<std::size_t>(grid.layers);
    std::vector<double> mean(layers, 0.0);
    std::vector<double> warmth(layers, 0.0);
    std::vector<double> light(layers, 0.0);
    std::vector<double> count(layers, 0.0);
    for (std::size_t cell = 0; cell < grid.size(); ++cell) {
        if (field.blocked[cell] == 0) {
            mean[cell % layers] += potential(field, planet, cell);
            warmth[cell % layers] += static_cast<double>(field.air[cell].temperature);
            light[cell % layers] += lightness(field.air[cell]);
            count[cell % layers] += 1.0;
        }
    }
    for (std::size_t layer = 0; layer < layers; ++layer) {
        mean[layer] = count[layer] > 0.0 ? mean[layer] / count[layer] : 1.0;
        warmth[layer] = count[layer] > 0.0 ? warmth[layer] / count[layer] : 1.0;
        light[layer] = count[layer] > 0.0 ? light[layer] / count[layer] : 0.0;
    }

    const double spin = 2.0 * std::numbers::pi / (planet.body.day * 3'600.0);
    const double scale = scale_height(planet, globe::mean_temperature(planet));
    for (std::size_t cell = 0; cell < grid.size(); ++cell) {
        if (field.blocked[cell] != 0) {
            field.air[cell].wind = Vec3{0.0F};
            continue;
        }
        const std::size_t column = cell / layers;
        const int layer = static_cast<int>(cell % layers);
        Air& air = field.air[cell];

        const double reference = mean[cell % layers];
        const double buoyancy =
            planet.body.gravity *
            (((potential(field, planet, cell) - reference) / warmth[cell % layers]) +
             (lightness(air) - light[cell % layers]));
        air.wind += stencil.up(column) * static_cast<float>(buoyancy * seconds);
        // The planet turns under moving air: the wind turns about its axis at twice its spin.
        air.wind = rotate_about_z(air.wind, -2.0 * spin * seconds);

        const double above = grid.radius_of(layer) - field.ground[column];
        const double pressure = std::exp(-above / scale);
        const double near =
            std::clamp((pressure - boundary_pressure) / (1.0 - boundary_pressure), 0.0, 1.0);
        air.wind *= static_cast<float>(std::exp(-drag * near * seconds));
        // Eddies too small for the grid shed a wind's energy at u³ over their size.
        const double eddy = static_cast<double>(glm::length(air.wind)) * seconds /
                            (eddy_cells * static_cast<double>(stencil.links(cell)[0].distance));
        air.wind /= static_cast<float>(1.0 + eddy);

        const double target = air_temperature(planet, static_cast<double>(field.surface[column]),
                                              grid.radius_of(layer) - planet.body.radius);
        const double rate = free_relaxation + ((ground_relaxation - free_relaxation) * near);
        air.temperature += static_cast<float>((target - static_cast<double>(air.temperature)) *
                                              (1.0 - std::exp(-rate * seconds)));
    }
}

double divergence(const Field& field, const Stencil& stencil, const globe::Planet& planet) {
    const std::vector<double> found = divergence_of(field, stencil, planet);
    double total = 0.0;
    double count = 0.0;
    for (std::size_t cell = 0; cell < found.size(); ++cell) {
        if (field.blocked[cell] == 0) {
            total += found[cell] * found[cell];
            count += 1.0;
        }
    }
    return count > 0.0 ? std::sqrt(total / count) : 0.0;
}

int project(Field& field, const Stencil& stencil, const globe::Planet& planet, double tolerance,
            int iterations) {
    const std::size_t n = field.grid.size();
    const std::vector<double> density = densities(field, planet);
    // Gᵀ G φ = Gᵀ m: the pressure whose slope is the mass flux's divergent part.
    std::vector<double> residual(n);
    const Pushes pushes = pushes_of(field, stencil);
    adjoint(field, stencil, pushes, flux_of(field, density), residual);

    std::vector<double> inverse(n, 0.0);
    for (std::size_t cell = 0; cell < n; ++cell) {
        double diagonal = 0.0;
        if (field.blocked[cell] == 0) {
            for (const Link& link : stencil.links(cell)) {
                if (open(field, link)) {
                    diagonal += 1.0 / (static_cast<double>(link.distance) *
                                       static_cast<double>(link.distance));
                }
            }
        }
        inverse[cell] = diagonal > 0.0 ? 1.0 / diagonal : 0.0;
    }

    std::vector<double> pressure(n, 0.0);
    std::vector<double> direction(n);
    std::vector<double> product(n);
    std::vector<double> preconditioned(n);
    std::vector<DVec3> slope(n);
    for (std::size_t cell = 0; cell < n; ++cell) {
        preconditioned[cell] = inverse[cell] * residual[cell];
    }
    direction = preconditioned;
    double rz = dot(residual, preconditioned);
    const double start = std::sqrt(dot(residual, residual));
    int done = 0;
    while (done < iterations && start > 0.0 &&
           std::sqrt(dot(residual, residual)) > tolerance * start) {
        apply_laplacian(field, stencil, pushes, direction, product, slope);
        const double curvature = dot(direction, product);
        if (curvature <= 0.0) {
            break;
        }
        const double step = rz / curvature;
        for (std::size_t cell = 0; cell < n; ++cell) {
            pressure[cell] += step * direction[cell];
            residual[cell] -= step * product[cell];
            preconditioned[cell] = inverse[cell] * residual[cell];
        }
        const double next = dot(residual, preconditioned);
        const double beta = next / rz;
        rz = next;
        for (std::size_t cell = 0; cell < n; ++cell) {
            direction[cell] = preconditioned[cell] + (beta * direction[cell]);
        }
        ++done;
    }

    gradient(field, stencil, pushes, pressure, slope);
    for (std::size_t cell = 0; cell < n; ++cell) {
        if (field.blocked[cell] == 0) {
            field.air[cell].wind -= Vec3{slope[cell] / density[cell]};
        }
    }
    return done;
}

void advect(Field& field, const Stencil& stencil, const globe::Planet& planet, double seconds) {
    std::vector<Air> next = field.air;
    const auto layers = static_cast<std::size_t>(field.grid.layers);
    for (std::size_t cell = 0; cell < next.size(); ++cell) {
        if (field.blocked[cell] != 0) {
            continue;
        }
        const Departure from = trace(field, stencil, cell, seconds);
        if (!from.moved) {
            continue;
        }
        const auto warmth =
            reconstruct<float>(field, stencil, from.cell, from.offset, [&](std::size_t i) {
                return static_cast<float>(potential(field, planet, i));
            });
        const auto scalar = [&](auto member) {
            return reconstruct<float>(field, stencil, from.cell, from.offset,
                                      [&](std::size_t i) { return field.air[i].*member; });
        };
        Air& air = next[cell];
        const int layer = static_cast<int>(cell % layers);
        air.temperature =
            warmth - static_cast<float>(dry_lapse_rate(planet) *
                                        (field.grid.radius_of(layer) - planet.body.radius));
        air.vapour = scalar(&Air::vapour);
        air.cloud = scalar(&Air::cloud);
        air.rain = scalar(&Air::rain);
        air.wind = carry(wind_near(field, stencil, from.cell, from.offset), from.up,
                         stencil.up(cell / layers));
    }
    const std::vector<double> mass = masses(field, planet);
    keep_total(field, next, mass, &Air::vapour);
    keep_total(field, next, mass, &Air::cloud);
    keep_total(field, next, mass, &Air::rain);
    // The same for the warmth the air carries: its potential temperature, which rising doesn't
    // change.
    double warmth_before = 0.0;
    double warmth_after = 0.0;
    double total = 0.0;
    for (std::size_t cell = 0; cell < mass.size(); ++cell) {
        if (field.blocked[cell] != 0) {
            continue;
        }
        warmth_before += potential(field, planet, cell) * mass[cell];
        const double height =
            field.grid.radius_of(static_cast<int>(cell % layers)) - planet.body.radius;
        warmth_after +=
            (static_cast<double>(next[cell].temperature) + (dry_lapse_rate(planet) * height)) *
            mass[cell];
        total += mass[cell];
    }
    if (total > 0.0) {
        const auto shift = static_cast<float>((warmth_before - warmth_after) / total);
        for (std::size_t cell = 0; cell < next.size(); ++cell) {
            if (field.blocked[cell] == 0) {
                next[cell].temperature += shift;
            }
        }
    }
    field.air = std::move(next);
}

void step(Field& field, const Stencil& stencil, const globe::Planet& planet, double seconds) {
    HOG_ZONE("air step");
    {
        HOG_ZONE("accelerate");
        accelerate(field, stencil, planet, seconds);
    }
    {
        HOG_ZONE("project");
        project(field, stencil, planet);
    }
    {
        HOG_ZONE("advect");
        advect(field, stencil, planet, seconds);
    }
    {
        HOG_ZONE("moisture");
        evaporate(field, planet, seconds);
        exchange_heat(field, planet, seconds);
        condense(field, planet);
        precipitate(field, planet, seconds);
    }
}

} // namespace hog::weather

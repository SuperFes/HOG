#include "terms.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/vents.hpp>

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <cstddef>

namespace hog::globe::terms {
namespace {

// Rounds the cone's tip, in radii, so its slope is smooth where the crater doesn't cover it.
constexpr double tip = 0.05;
// Flanks steepen towards the top.
constexpr double flank = 1.6;
constexpr std::array<int, 3> harmonic{2, 3, 5};

void add_cell(Noise& sum, const Planet& planet, DVec3 direction, double lod, Cell cell) {
    const auto volcano = volcano_in(planet, cell);
    if (!volcano) {
        return;
    }
    const DVec3 middle = volcano->centre;
    const DVec3 side_axis = std::abs(middle.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(side_axis, middle));
    const DVec3 north = glm::cross(middle, east);
    const DVec3 offset = direction - middle;
    const double apart =
        planet.radius * maths::atan2(glm::length(glm::cross(direction, middle)),
                                     glm::dot(direction, middle));
    // The outline: the radius at this bearing, and how it changes with the bearing.
    const double bearing = maths::atan2(glm::dot(offset, north), glm::dot(offset, east));
    double warp = 1.0;
    double warp_slope = 0.0;
    for (std::size_t k = 0; k < harmonic.size(); ++k) {
        const double angle = (harmonic[k] * bearing) + volcano->phase[k];
        warp += volcano->amplitude[k] * maths::cos(angle);
        warp_slope -= volcano->amplitude[k] * harmonic[k] * maths::sin(angle);
    }
    const double reach = volcano->radius * warp;
    const double x = apart / reach;
    if (x >= 1.0) {
        return;
    }
    // The cone, concave, smooth at the tip and into the ground.
    const double rounded = std::sqrt((x * x) + (tip * tip));
    double height = 0.0;
    double slope = 0.0; // per radius
    if (rounded < 1.0) {
        const double up = 1.0 - rounded;
        height = volcano->height * maths::pow(up, flank);
        slope = -flank * volcano->height * maths::pow(up, flank - 1.0) * x / rounded;
    }
    // The crater, a bowl with a smooth lip; narrower than the detail, it fades out.
    const double shown = smoothstep((volcano->crater * reach / (2.0 * lod)) - 1.0);
    const double u = x / volcano->crater;
    if (u < 1.0) {
        const double bowl = 1.0 - (u * u);
        height -= shown * volcano->depth * bowl * bowl;
        slope += shown * volcano->depth * 4.0 * bowl * u / volcano->crater;
    }
    sum.value += height;
    const DVec3 outward = tangential(direction - middle, direction);
    const double out_length = glm::length(outward);
    if (out_length > 1e-15) {
        const DVec3 out = outward / out_length;
        // Along the bearing too, as the outline pulls the cone in and out.
        const DVec3 round = glm::cross(middle, out);
        sum.gradient += (slope / reach) * out;
        sum.gradient += (-slope * warp_slope / (reach * warp)) * round;
    }
}

} // namespace

Noise volcanoes(const Context& context, DVec3 direction, double lod) noexcept {
    const Planet& planet = *context.planet;
    const Cell home = cell_under(direction, 6);
    Noise sum;
    add_cell(sum, planet, direction, lod, home);
    const CellNeighbors near = around(home);
    for (const Cell cell : near.all()) {
        add_cell(sum, planet, direction, lod, cell);
    }
    return sum;
}

} // namespace hog::globe::terms

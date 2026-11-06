#include <hog/flow/glacier.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/server/ice_bake.hpp>
#include <hog/weather/snowpack.hpp>

#include <algorithm>
#include <cmath>

namespace hog::server {

double IceMap::at(std::uint32_t i, std::uint32_t j) const noexcept {
    if (side == 0 || i >= columns() || j >= columns()) {
        return 0.0;
    }
    // Node n's middle is at column (n + 1/2) * stride.
    const auto along = [&](std::uint32_t column) {
        return (static_cast<double>(column) + 0.5) / stride - 0.5;
    };
    const double u = std::clamp(along(i), 0.0, static_cast<double>(side - 1));
    const double v = std::clamp(along(j), 0.0, static_cast<double>(side - 1));
    const auto i0 = static_cast<std::uint32_t>(u);
    const auto j0 = static_cast<std::uint32_t>(v);
    const std::uint32_t i1 = std::min(i0 + 1, side - 1);
    const std::uint32_t j1 = std::min(j0 + 1, side - 1);
    const auto node = [&](std::uint32_t a, std::uint32_t b) {
        return ice[(static_cast<std::size_t>(b) * side) + a];
    };
    const double fu = u - i0;
    const double fv = v - j0;
    return std::lerp(std::lerp(node(i0, j0), node(i1, j0), fu),
                     std::lerp(node(i0, j1), node(i1, j1), fu), fv);
}

IceMap bake_ice_map(const globe::Generator& generator, globe::Cell corner, std::uint32_t side,
                    std::uint32_t stride, const ClimateAt& climate, int years) {
    const globe::Layers& layers = generator.layers();
    const globe::Planet& planet = generator.planet();
    IceMap map{.corner = corner, .stride = stride, .side = side, .ice = {}};
    const std::size_t count = static_cast<std::size_t>(side) * side;
    map.ice.assign(count, 0.0);

    const auto column_of = [&](std::uint32_t a, std::uint32_t b) {
        return globe::Cell{.level = corner.level,
                           .face = corner.face,
                           .i = corner.i + (a * stride) + (stride / 2),
                           .j = corner.j + (b * stride) + (stride / 2)};
    };
    // The column's width where the ground is, for the distance between nodes.
    const globe::Cell middle = column_of(side / 2, side / 2);
    const auto ground_there =
        generator
            .column(globe::center(middle), layers.thickness * stride)
            .ground();
    const double width = std::sqrt(
        layers.volume(middle, std::max(layers.layer_at(ground_there), 0)) / layers.thickness);

    flow::Glacier glacier{count, {.cell = width * stride, .wear = 0.0}};
    std::vector<globe::Cell> cells(count);
    for (std::uint32_t b = 0; b < side; ++b) {
        for (std::uint32_t a = 0; a < side; ++a) {
            const std::size_t n = (static_cast<std::size_t>(b) * side) + a;
            cells[n] = column_of(a, b);
            glacier[n].rock =
                generator.column(globe::center(cells[n]), layers.thickness * stride).ground();
            const auto at = [&](std::uint32_t x, std::uint32_t y, bool inside) {
                return inside ? static_cast<int>((static_cast<std::size_t>(y) * side) + x)
                              : flow::Glacier::none;
            };
            glacier[n].next = {at(a + 1, b, a + 1 < side), at(a, b + 1, b + 1 < side),
                               at(a - 1, b, a > 0), at(a, b - 1, b > 0)};
        }
    }
    const auto rim = [&](std::size_t n) {
        return glacier[n].next[0] == flow::Glacier::none || glacier[n].next[1] == flow::Glacier::none ||
               glacier[n].next[2] == flow::Glacier::none || glacier[n].next[3] == flow::Glacier::none;
    };
    // Each node's balance is what its climate gives at the height of its surface, taken afresh as
    // the ice raises or the rock under it lies.
    std::vector<weather::Climate> climates(count);
    for (std::size_t n = 0; n < count; ++n) {
        climates[n] = climate(cells[n]);
    }
    for (int year = 0; year < years; ++year) {
        for (std::size_t n = 0; n < count; ++n) {
            const double height = glacier.surface(n) - planet.radius;
            glacier[n].ice = std::max(
                glacier[n].ice + weather::ice_balance(planet, climates[n], height).net(), 0.0);
            if (rim(n)) {
                glacier[n].ice = 0.0;
            }
        }
        for (double left = 1.0; left > 1.0e-9;) {
            left -= glacier.step(left);
        }
        // The rim is open: ice reaching it is gone.
        for (std::size_t n = 0; n < count; ++n) {
            if (rim(n)) {
                glacier[n].ice = 0.0;
            }
        }
    }
    for (std::size_t n = 0; n < count; ++n) {
        map.ice[n] = glacier[n].ice;
    }
    return map;
}

} // namespace hog::server

#include <hog/globe/grip.hpp>
#include <hog/globe/surface.hpp>

#include <glm/geometric.hpp>

#include <array>
#include <cmath>

namespace hog::globe {

namespace {

// How far under the feet ground is looked for, in layers.
constexpr std::uint32_t reach = 4;
constexpr std::uint32_t tallest = 64;

} // namespace

std::optional<double> grip_at(const World& world, DVec3 feet) {
    const Layers& layers = world.layers();
    const double radius = glm::length(feet);
    const Cell column = cell_under(feet / radius, layers.level);
    const int start = layers.layer_at(radius);
    if (start < 0 || start >= layers.count) {
        return std::nullopt;
    }
    // The voxels met, kept so the lookup can hand out pointers to them.
    std::array<Voxel, tallest + reach + 2> met{};
    const auto lookup = [&](std::uint32_t layer) -> const Voxel* {
        if (layer >= static_cast<std::uint32_t>(layers.count)) {
            return nullptr;
        }
        Voxel& slot = met[layer % met.size()];
        slot = world.at({.face = column.face, .i = column.i, .j = column.j, .layer = layer});
        return &slot;
    };
    const std::optional<Surface> surface =
        surface_below(static_cast<std::uint32_t>(start), reach, tallest, layers.thickness, lookup);
    if (!surface) {
        return std::nullopt;
    }
    return std::atan(surface->holds(layers.thickness, world.planet().gravity));
}

} // namespace hog::globe

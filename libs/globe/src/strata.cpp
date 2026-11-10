#include "terms.hpp"

#include <hog/core/hash.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace hog::globe::terms {
namespace {

double unit(std::uint64_t value) {
    return static_cast<double>(value >> 11U) * 0x1.0p-53;
}

} // namespace

Material material_at(const Context& context, const Layering& layers, double depth,
                     double radius) noexcept {
    // What a formed world laid over its beds, from the top down.
    const std::array<std::pair<double, Material>, 13> laid{{
        {layers.rubble, layers.rubble_of},
        {layers.ice, Material::Ice},
        {layers.beach, layers.beach_of},
        {layers.organic, Material::Soil},
        {layers.topsoil, Material::Soil},
        {layers.subsoil, Material::Clay},
        {layers.loess, Material::Sediment},
        {layers.sand, Material::Sand},
        {layers.alluvium, Material::Sediment},
        {layers.lake_beds, Material::Clay},
        {layers.till, Material::Gravel},
        {layers.saprolite, Material::Clay},
        {layers.lava, Material::Basalt},
    }};
    double bottom = 0.0;
    for (const auto& [thickness, material] : laid) {
        bottom += thickness;
        if (depth < bottom) {
            return material;
        }
    }
    if (depth >= layers.cover) {
        return layers.basement;
    }
    return bed_rock(context, bed_at(context, layers, radius));
}

std::int64_t bed_at(const Context& context, const Layering& layers, double radius) noexcept {
    const double lifted = radius - context.planet->body.radius + layers.fold;
    return static_cast<std::int64_t>(std::floor(lifted / bed_thickness));
}

double bed_bottom(const Context& context, const Layering& layers, std::int64_t bed) noexcept {
    return context.planet->body.radius - layers.fold + (static_cast<double>(bed) * bed_thickness);
}

Material bed_rock(const Context& context, std::int64_t bed) noexcept {
    // Each bed's rock drawn once for the planet, so a bed is the same rock wherever it shows.
    const double draw = unit(hash_values(context.seed(Term::Strata), bed));
    // Limestone is laid down only in seas.
    if (context.shape->sea && draw < 0.3) {
        return Material::Limestone;
    }
    return draw < 0.65 ? Material::Shale : Material::Sandstone;
}

} // namespace hog::globe::terms

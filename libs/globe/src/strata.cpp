#include "terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace hog::globe::terms {
namespace {

double unit(std::uint64_t value) {
    return static_cast<double>(value >> 11U) * 0x1.0p-53;
}

// Pascals loose ground and the hardest rock bear, between which contacts narrow.
constexpr double loose_strength = 1.0e4;
constexpr double hard_strength = 1.5e8;
constexpr double narrowest_contact = 0.2; // metres

// Scales the noise so its values spread nearly evenly from 0 to 1.
constexpr double scatter_spread = 3.0;

} // namespace

double contact_width(Material material) noexcept {
    static const std::array<double, material_count> widths = [] {
        std::array<double, material_count> made{};
        for (std::size_t k = 0; k < made.size(); ++k) {
            const double strength =
                std::max(properties(static_cast<Material>(k)).strength, loose_strength);
            const double hardness = std::min(1.0, maths::log2(strength / loose_strength) /
                                                      maths::log2(hard_strength / loose_strength));
            made.at(k) = widest_contact + ((narrowest_contact - widest_contact) * hardness);
        }
        return made;
    }();
    return widths.at(static_cast<std::size_t>(material));
}

double scatter(const Context& context, Term term, DVec3 direction) noexcept {
    // Read with all its octaves at every detail, so a coarse column shows what's at its middle
    // and coarse ground holds each material in the same share as fine.
    const double voxel = context.planet->voxel;
    const Octaves octaves{.wavelength = 32.0 * voxel, .gain = 0.5, .finest = 2.0 * voxel};
    const double x =
        scatter_spread *
        fbm(direction * context.planet->body.radius, octaves, 0.0, context.seed(term)).value;
    return 0.5 + (0.5 * x / std::sqrt(1.0 + (x * x)));
}

Scatter scatter_at(const Context& context, DVec3 direction, const Layering& layers) noexcept {
    Scatter made;
    made.contacts = scatter(context, Term::Contacts, direction);
    if (layers.beach > 0.0 && layers.beach_share > 0.0) {
        made.beach = scatter(context, Term::Beach, direction);
    }
    if (layers.basement_share > 0.0) {
        made.basement = scatter(context, Term::Basement, direction);
    }
    return made;
}

Material basement_at(const Layering& layers, const Scatter& scatter) noexcept {
    return scatter.basement < layers.basement_share ? layers.basement_with : layers.basement;
}

namespace {

// What a formed world laid over its beds, from the top down, each run of one material as one
// layer. Rubble already shows scattered, in the share its blocks cover, so its contact stays put.
struct Laid {
    double thickness = 0.0;
    Material material = Material::Air;
    bool wanders = true;
};
struct Stack {
    std::array<Laid, 13> layers{};
    std::size_t count = 0;
};

Stack stacked(const Layering& layers, Material beach) noexcept {
    Stack stack;
    if (layers.rubble > 0.0) {
        stack.layers.at(stack.count++) = {
            .thickness = layers.rubble, .material = layers.rubble_of, .wanders = false};
    }
    for (const auto& [thickness, material] : std::array<std::pair<double, Material>, 12>{{
             {layers.ice, Material::Ice},
             {layers.beach, beach},
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
         }}) {
        if (thickness <= 0.0) {
            continue;
        }
        Laid* const last = stack.count > 0 ? &stack.layers.at(stack.count - 1) : nullptr;
        if (last != nullptr && last->wanders && last->material == material) {
            last->thickness += thickness;
        } else {
            stack.layers.at(stack.count++) = {.thickness = thickness, .material = material};
        }
    }
    return stack;
}

// Whether a point `depth` metres down lies past the contact `bottom` metres down, under a layer
// `thickness` thick: the contact wanders half its `width` either way of where it was laid, as
// far through that as `wander`, but up no more than half the layer, so a layer thinning to
// nothing shows ever less, and none where it ends.
bool past(double depth, double wander, double bottom, double width, double thickness) noexcept {
    const double up = 0.5 * std::min(width, thickness);
    return depth + (width * wander) - up >= bottom;
}

// The beds' rock at a point past what was laid over them, `laid` metres deep, or the basement,
// the beds either side showing as the contacts with them wander into the point's.
Material bedded(const Context& context, const Layering& layers, double wander, double depth,
                double radius, double laid, Material basement) noexcept {
    if (layers.cover <= laid) {
        return basement;
    }
    const std::int64_t bed = bed_at(context, layers, radius);
    const Material rock = bed_rock(context, bed);
    if (past(depth, wander, layers.cover, std::max(contact_width(rock), contact_width(basement)),
             layers.cover - laid)) {
        return basement;
    }
    const double base = depth + radius - bed_bottom(context, layers, bed);
    const double roof = base - bed_thickness;
    if (roof > laid) {
        const Material above = bed_rock(context, bed + 1);
        if (!past(depth, wander, roof, std::max(contact_width(rock), contact_width(above)),
                  bed_thickness)) {
            return above;
        }
    }
    const Material below = base >= layers.cover ? basement : bed_rock(context, bed - 1);
    return past(depth, wander, base, std::max(contact_width(rock), contact_width(below)),
                bed_thickness)
               ? below
               : rock;
}

} // namespace

Material material_at(const Context& context, const Layering& layers, const Scatter& scatter,
                     double depth, double radius) noexcept {
    const Material basement = basement_at(layers, scatter);
    if (depth >= layers.cover + contact_reach) {
        return basement;
    }
    const Material beach = scatter.beach < layers.beach_share ? layers.beach_with : layers.beach_of;
    const Stack stack = stacked(layers, beach);
    double bottom = 0.0;
    for (std::size_t k = 0; k < stack.count; ++k) {
        const Laid& layer = stack.layers.at(k);
        bottom += layer.thickness;
        if (depth >= bottom + contact_reach) {
            continue;
        }
        // What lies under it: the next layer, or rock.
        Material under = basement;
        if (k + 1 < stack.count) {
            under = stack.layers.at(k + 1).material;
        } else if (bottom < layers.cover) {
            under = bed_rock(context, bed_at(context, layers, radius + depth - bottom));
        }
        const double width =
            layer.wanders ? std::max(contact_width(layer.material), contact_width(under)) : 0.0;
        if (!past(depth, scatter.contacts, bottom, width, layer.thickness)) {
            return layer.material;
        }
    }
    return bedded(context, layers, scatter.contacts, depth, radius, bottom, basement);
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

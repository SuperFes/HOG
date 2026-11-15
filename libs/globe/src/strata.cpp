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
    return scatter_in(context, term, direction * context.planet->body.radius);
}

double scatter_in(const Context& context, Term term, DVec3 point) noexcept {
    // Read with all its octaves at every detail, so a coarse voxel shows what's at its middle
    // and coarse ground holds each material in the same share as fine.
    const double voxel = context.planet->voxel;
    const Octaves octaves{.wavelength = 32.0 * voxel, .gain = 0.5, .finest = 2.0 * voxel};
    const double x = scatter_spread * fbm(point, octaves, 0.0, context.seed(term)).value;
    return 0.5 + (0.5 * x / std::sqrt(1.0 + (x * x)));
}

Scatter scatter_at(const Context& context, DVec3 direction, const Layering& layers) noexcept {
    Scatter made;
    made.contacts = scatter(context, Term::Contacts, direction);
    if (layers.beach > 0.0 && layers.beach_share > 0.0) {
        made.beach = scatter(context, Term::Beach, direction);
    }
    return made;
}

Material basement_at(const Context& context, const Layering& layers, DVec3 point) noexcept {
    if (layers.basement_share <= 0.0) {
        return layers.basement;
    }
    return scatter_in(context, Term::Basement, point) < layers.basement_share ? layers.basement_with
                                                                              : layers.basement;
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
                double radius, double laid, DVec3 point) noexcept {
    // Two basements are both hard rock, so either's contact width stands for both.
    const Material basement = layers.basement;
    if (layers.cover <= laid) {
        return basement_at(context, layers, point);
    }
    Beds beds{context, layers};
    const std::int64_t bed = beds.at(radius);
    const Material rock = bed_rock(context, bed);
    if (past(depth, wander, layers.cover, std::max(contact_width(rock), contact_width(basement)),
             layers.cover - laid)) {
        return basement_at(context, layers, point);
    }
    const double bottom = beds.bottom(bed);
    const double top = beds.top(bed);
    const double base = depth + radius - bottom;
    const double roof = base - (top - bottom);
    if (roof > laid) {
        const Material above = bed_rock(context, bed + 1);
        if (!past(depth, wander, roof, std::max(contact_width(rock), contact_width(above)),
                  beds.top(bed + 1) - top)) {
            return above;
        }
    }
    const bool on_basement = base >= layers.cover;
    const Material below = on_basement ? basement : bed_rock(context, bed - 1);
    if (!past(depth, wander, base, std::max(contact_width(rock), contact_width(below)),
              top - bottom)) {
        return rock;
    }
    return on_basement ? basement_at(context, layers, point) : below;
}

} // namespace

Material material_at(const Context& context, const Layering& layers, const Scatter& scatter,
                     DVec3 direction, double depth, double radius) noexcept {
    const DVec3 point = direction * radius;
    if (depth >= layers.cover + contact_reach) {
        return basement_at(context, layers, point);
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
        Material under = layers.basement;
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
    return bedded(context, layers, scatter.contacts, depth, radius, bottom, point);
}

Beds::Beds(const Context& context, const Layering& layers) noexcept
    : seed_{context.seed(Term::Bedding)}, base_{context.planet->body.radius - layers.fold} {}

void Beds::draw(std::int64_t set) noexcept {
    if (drawn_ && set == set_) {
        return;
    }
    // Most beds thin, a few massive, some twenty-five times another's thickness.
    double sum = 0.0;
    for (std::int64_t k = 0; k < in_set; ++k) {
        const double draw = unit(hash_values(seed_, set, k));
        sum += 0.15 + (4.0 * draw * draw * draw);
        bottoms_.at(static_cast<std::size_t>(k + 1)) = sum;
    }
    const double scale = static_cast<double>(in_set) * bed_thickness / sum;
    for (double& bottom : bottoms_) {
        bottom *= scale;
    }
    set_ = set;
    drawn_ = true;
}

std::int64_t Beds::at(double radius) noexcept {
    const double thickness = static_cast<double>(in_set) * bed_thickness;
    const double lifted = radius - base_;
    const auto set = static_cast<std::int64_t>(std::floor(lifted / thickness));
    draw(set);
    const double within = lifted - (static_cast<double>(set) * thickness);
    std::int64_t k = 0;
    while (k + 1 < in_set && bottoms_.at(static_cast<std::size_t>(k + 1)) <= within) {
        ++k;
    }
    return (set * in_set) + k;
}

double Beds::bottom(std::int64_t bed) noexcept {
    const std::int64_t set = (bed >= 0 ? bed : bed - in_set + 1) / in_set;
    draw(set);
    return base_ + (static_cast<double>(set * in_set) * bed_thickness) +
           bottoms_.at(static_cast<std::size_t>(bed - (set * in_set)));
}

std::int64_t bed_at(const Context& context, const Layering& layers, double radius) noexcept {
    return Beds{context, layers}.at(radius);
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

#include "terms.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hog::globe::terms {
namespace {

// Passages wind some tens of metres between bends, a few metres wide, and run twice as far
// along as up or down: water dissolves its way along beds and toward the water table.
constexpr double tunnel_wavelength = 90.0;
constexpr double tunnel_radius = 3.5;
constexpr double stretch = 2.0;

// Chambers open where dissolution pooled, rarely.
constexpr double chamber_wavelength = 160.0;
constexpr double chamber_threshold = 0.6;

// Caves gather in districts a couple of kilometres across, where water found its way in.
constexpr double district_wavelength = 2'000.0;

// Dissolution needs water moving: it fades a kilometre down. Lava tubes lie within tens of
// metres of the flows' tops.
constexpr double karst_depth = 1'000.0;
constexpr double tube_depth = 30.0;

// How readily each rock holds open passages.
double openness(Material rock, double depth, bool watered) {
    switch (rock) {
    case Material::Limestone:
        return watered ? 1.0 : 0.0;
    case Material::Shale:
    case Material::Sandstone:
        return watered ? 0.25 : 0.0;
    case Material::Basalt:
        // Lava tubes, on any world with flows.
        return 1.0 - smoothstep((depth - tube_depth) / tube_depth);
    case Material::Granite:
    case Material::Anorthosite:
        return 0.1;
    default:
        return 0.0;
    }
}

// The field's gradient against the point itself, given its gradient against the stretched point.
DVec3 unstretched(DVec3 gradient, DVec3 direction, double tangent_scale) {
    const double along = glm::dot(gradient, direction);
    return ((gradient - (along * direction)) * tangent_scale) + ((stretch * along) * direction);
}

// The cave fields at a point, whatever rock it's in, each found when first wanted.
class Fields {
public:
    Fields(const Context& context, DVec3 direction, double radius, double lod)
        : context_{&context}, direction_{direction}, radius_{radius}, lod_{lod},
          // Stretched across the radius, so passages run more along than up.
          lifted_{context.planet->radius + (stretch * (radius - context.planet->radius))} {}

    // Where caves gather, from 0 to 1.
    double district() {
        if (!district_) {
            const Noise field = fbm(direction_ * context_->planet->radius,
                                    {.wavelength = district_wavelength,
                                     .gain = 0.5,
                                     .finest = district_wavelength / 2.0},
                                    0.0, context_->seed(Term::CaveDistricts));
            district_ = smoothstep((field.value + 0.1) / 0.4);
        }
        return *district_;
    }

    // Metres out from where both passage fields are zero, near enough.
    double across() {
        if (!across_) {
            const Octaves winding{
                .wavelength = tunnel_wavelength, .gain = 0.3, .finest = tunnel_wavelength / 8.0};
            const auto metres = [&](Term term) {
                const Noise field = fbm(at(), winding, lod_, context_->seed(term));
                return field.value / std::max(glm::length(unstretched(field.gradient)), 1e-9);
            };
            const double a = metres(Term::CaveA);
            const double b = metres(Term::CaveB);
            across_ = std::sqrt((a * a) + (b * b));
        }
        return *across_;
    }

    // The chamber field, and its rate per metre.
    Noise pool() {
        if (!pool_) {
            const Noise field = fbm(
                at(),
                {.wavelength = chamber_wavelength, .gain = 0.5, .finest = chamber_wavelength / 8.0},
                lod_, context_->seed(Term::Chambers));
            pool_ = Noise{.value = field.value, .gradient = unstretched(field.gradient)};
        }
        return *pool_;
    }

private:
    [[nodiscard]] DVec3 at() const { return direction_ * lifted_; }
    [[nodiscard]] DVec3 unstretched(DVec3 gradient) const {
        return terms::unstretched(gradient, direction_, lifted_ / radius_);
    }

    const Context* context_;
    DVec3 direction_;
    double radius_;
    double lod_;
    double lifted_;
    std::optional<double> district_;
    std::optional<double> across_;
    std::optional<Noise> pool_;
};

// The widest a chamber's roof spans in `rock`: a bed of it as a beam bearing its own weight,
// rock bearing a tenth as much pulled as pressed.
double widest_span(const Planet& planet, Material rock) {
    const Properties& properties = hog::globe::properties(rock);
    return std::sqrt(2.0 * (properties.strength / 10.0) * bed_thickness /
                     (properties.density * planet.gravity));
}

// Metres to the open space of the caves in `rock` as `open` as it is there, were it all that rock.
double cave_in(Fields& fields, Material rock, double open, bool watered, double cover, double lod,
               const Planet& planet) {
    const double presence = open * fields.district();
    const double size = tunnel_radius * presence;
    // Passages too narrow for the detail asked for close up smoothly.
    const double shown = size * smoothstep((size / lod) - 1.0);
    double distance = shown > 0.0 ? fields.across() - shown : cave_reach;
    if (rock == Material::Limestone && watered) {
        // Fewer where passages are fewer: none where there are none.
        const double threshold = chamber_threshold + ((1.0 - presence) * 0.6);
        // A roof arches over a span only under about as much rock again; under less it falls
        // in, so chambers shrink to nothing toward open air.
        const double held = smoothstep(cover / widest_span(planet, rock));
        const double needed = 1.0 - ((1.0 - threshold) * held);
        const Noise pool = fields.pool();
        distance =
            std::min(distance, (needed - pool.value) / std::max(glm::length(pool.gradient), 1e-9));
    }
    return distance;
}

} // namespace

double cave_distance(const Context& context, const Layering& layers, DVec3 direction, double radius,
                     double ground, double cover, double lod) noexcept {
    const Planet& planet = *context.planet;
    const bool watered = planet.volatiles != Volatile::None;
    const double depth = ground - radius;
    const double fade = 1.0 - smoothstep((depth - karst_depth) / karst_depth);
    if (fade <= 0.0) {
        return cave_reach;
    }

    // The rock the point is in and that beside it, each with how far the point lies outside it.
    struct Unit {
        Material rock = Material::Air;
        double outside = 0.0;
    };
    std::array<Unit, 4> units{};
    std::size_t count = 0;
    const double base = ground - layers.cover; // beds lie over it, the basement under it
    if (layers.cover > 0.0) {
        const std::int64_t bed = bed_at(context, layers, radius);
        for (std::int64_t near = bed - 1; near <= bed + 1; ++near) {
            const double bottom = bed_bottom(context, layers, near);
            const double top = bottom + bed_thickness;
            if (top > base) {
                units.at(count++) = {.rock = bed_rock(context, near),
                                     .outside =
                                         std::max({bottom - radius, radius - top, base - radius})};
            }
        }
    }
    units.at(count++) = {.rock = layers.basement, .outside = radius - base};

    double nearest = cave_reach;
    Fields fields{context, direction, radius, lod};
    for (const Unit& unit : std::span{units}.first(count)) {
        const double open = openness(unit.rock, depth, watered) * fade;
        if (open <= 0.0 || unit.outside >= nearest) {
            continue;
        }
        const double inside = cave_in(fields, unit.rock, open, watered, cover, lod, planet);
        nearest = std::min(nearest, std::max(inside, unit.outside));
    }
    return nearest;
}

} // namespace hog::globe::terms

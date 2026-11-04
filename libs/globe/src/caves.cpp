#include "terms.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
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

constexpr Octaves winding{
    .wavelength = tunnel_wavelength, .gain = 0.3, .finest = tunnel_wavelength / 8.0};
constexpr Octaves pooling{
    .wavelength = chamber_wavelength, .gain = 0.5, .finest = chamber_wavelength / 8.0};

// Caves gather in districts a couple of kilometres across, where water found its way in.
constexpr double district_wavelength = 2'000.0;

// Water makes a passage where enough of it drains through a cell, widening as its flow does.
constexpr double karst_catchment = 1.0e5; // square metres draining through below which none forms
constexpr double karst_radius = 1.5;      // metres across at that catchment
constexpr double karst_widest = 15.0;
constexpr double karst_growth = 0.3; // the radius' power of the catchment
// How steeply the meanders' offset may change over a metre.
constexpr double karst_meander = 0.4;
// The share of the water's cutting down that had been done when the passage was dissolved.
constexpr double karst_stage = 0.5;
constexpr Octaves meandering{.wavelength = 120.0, .gain = 0.4, .finest = 20.0};

// Lava tubes lie within tens of metres of the flows' tops.
constexpr double tube_depth = 30.0;
// What a tunnel's wall bears against the rock's strength: about two and a half times the weight
// of the rock over it, and the rock mass holds a fifth of what intact rock does, being jointed.
constexpr double wall_stress = 2.5;
constexpr double mass_strength = 0.2;

double karst_width(double catchment) {
    return std::min(karst_widest,
                    karst_radius * std::pow(catchment / karst_catchment, karst_growth));
}

// Height over the radius the passage under a cell lies at: the water table as it stood part way
// down, and its roof clear of it by a passage's width.
double karst_level(const Generator::Shape& shape, std::size_t cell) {
    return static_cast<double>(shape.table[cell]) +
           (karst_stage * static_cast<double>(shape.cut[cell])) -
           (2.0 * karst_width(static_cast<double>(shape.catchment[cell])));
}

// The depth passages in `rock` are held open to, and none are by twice it: where the stress
// round them overcomes the rock.
double closure_depth(const Planet& planet, Material rock) {
    const Properties& properties = hog::globe::properties(rock);
    return mass_strength * properties.strength /
           (wall_stress * properties.density * planet.gravity);
}

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

    [[nodiscard]] DVec3 direction() const { return direction_; }
    [[nodiscard]] double radius() const { return radius_; }

    double district() {
        if (!district_) {
            district_ = cave_district(*context_, direction_);
        }
        return *district_;
    }

    // Metres out from where both passage fields are zero, near enough; or, `bound`, no more
    // than it truly is, by the fields' steepest slope rather than their slope here.
    double across(bool bound) {
        std::optional<double>& kept = bound ? across_bound_ : across_;
        if (!kept) {
            const auto metres = [&](Term term) {
                const Noise field = fbm(at(), winding, lod_, context_->seed(term));
                const double slope =
                    bound ? steepest(winding) : glm::length(unstretched(field.gradient));
                return field.value / std::max(slope, 1e-9);
            };
            const double a = metres(Term::CaveA);
            const double b = metres(Term::CaveB);
            kept = std::sqrt((a * a) + (b * b));
        }
        return *kept;
    }

    // The steepest a field of `octaves` gets here, per metre of the point itself.
    [[nodiscard]] double steepest(const Octaves& octaves) const {
        return fbm_steepest(octaves) * std::max(stretch, lifted_ / radius_);
    }

    // The chamber field, and its rate per metre; or, `bound`, the steepest it gets.
    Noise pool(bool bound) {
        if (!pool_) {
            const Noise field = fbm(at(), pooling, lod_, context_->seed(Term::Chambers));
            pool_ = Noise{.value = field.value, .gradient = unstretched(field.gradient)};
        }
        if (bound) {
            return Noise{.value = pool_->value, .gradient = DVec3{steepest(pooling), 0.0, 0.0}};
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
    std::optional<double> across_bound_;
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
double cave_in(const Context& context, Fields& fields, Material rock, double open, bool watered,
               double cover, double lod, const Planet& planet, bool bound) {
    const double presence = open * fields.district();
    const double size = tunnel_radius * presence;
    // Passages too narrow for the detail asked for close up smoothly.
    const double shown = size * smoothstep((size / lod) - 1.0);
    double distance = shown > 0.0 ? fields.across(bound) - shown : cave_reach;
    if (rock == Material::Limestone && watered) {
        // Dissolved by the water that ran through it, not opened by chance.
        distance = karst_distance(context, fields.direction(), fields.radius(), lod, bound);
        // Fewer where passages are fewer: none where there are none.
        const double threshold = chamber_threshold + ((1.0 - presence) * 0.6);
        // A roof arches over a span only under about as much rock again; under less it falls
        // in, so chambers shrink to nothing toward open air.
        const double held = smoothstep(cover / widest_span(planet, rock));
        const double needed = 1.0 - ((1.0 - threshold) * held);
        const Noise pool = fields.pool(bound);
        // Bounded, a chamber is wherever the field exceeds the threshold, which `needed` never
        // falls under, so none lies nearer than that.
        const double over = bound ? threshold : needed;
        if (!bound || threshold < 1.0) {
            distance = std::min(distance,
                                (over - pool.value) / std::max(glm::length(pool.gradient), 1e-9));
        }
    }
    return distance;
}

} // namespace

double karst_distance(const Context& context, DVec3 direction, double radius, double lod,
                      bool bound) noexcept {
    const Generator::Shape& shape = *context.shape;
    if (shape.receiver.empty()) {
        return cave_reach;
    }
    const double planet_radius = context.planet->radius;
    // The point as the meanders carry it: a passage is a straight run in this offset space.
    const DVec3 at = direction * radius;
    // Each of the three offsets changes by at most the noise's slope, so together by root three
    // of it.
    const double reach = karst_meander / (std::numbers::sqrt3 * fbm_steepest(meandering));
    const std::uint64_t seed = context.seed(Term::Meander);
    const DVec3 point = at + (reach * DVec3{fbm(at, meandering, lod, seed).value,
                                            fbm(at, meandering, lod, seed + 1).value,
                                            fbm(at, meandering, lod, seed + 2).value});
    const int level = shape.cut_level;
    const Cell home = cell_under(direction, level);
    const CellNeighbors beside = around(home);
    double nearest = cave_reach;
    const auto run_from = [&](Cell cell) {
        const auto from = static_cast<std::size_t>(index_of(cell));
        const std::size_t to = shape.receiver[from];
        const double catchment = static_cast<double>(shape.catchment[from]);
        if (to == from || catchment < karst_catchment) {
            return;
        }
        const Cell receiver = cell_at_index(level, to);
        const DVec3 a = center(cell) * (planet_radius + karst_level(shape, from));
        const DVec3 b = center(receiver) * (planet_radius + karst_level(shape, to));
        const DVec3 along = b - a;
        const double share =
            std::clamp(glm::dot(point - a, along) / glm::dot(along, along), 0.0, 1.0);
        const double away = glm::length(point - (a + (share * along)));
        const double narrow = karst_width(catchment);
        const double wide = karst_width(static_cast<double>(shape.catchment[to]));
        if (bound) {
            // The meanders can stretch a distance by up to their slope, but never a way inside.
            const double clear = away - std::max(narrow, wide);
            nearest = std::min(nearest, clear < 0.0 ? clear : clear / (1.0 + karst_meander));
            return;
        }
        const double width = narrow + (share * (wide - narrow));
        // Too narrow for the detail asked for, a passage closes up smoothly.
        nearest = std::min(nearest, away - (width * smoothstep((width / lod) - 1.0)));
    };
    run_from(home);
    for (const Cell cell : beside.all()) {
        run_from(cell);
    }
    return nearest;
}

double cave_floor(const Context& context) noexcept {
    const Planet& planet = *context.planet;
    // Lava tubes, and those the rocks that can hold passages hold, as far as they do.
    double floor = 2.0 * tube_depth;
    for (const Material rock : {Material::Granite, Material::Anorthosite}) {
        floor = std::max(floor, 2.0 * closure_depth(planet, rock));
    }
    if (planet.volatiles != Volatile::None) {
        for (const Material rock : {Material::Limestone, Material::Shale, Material::Sandstone}) {
            floor = std::max(floor, 2.0 * closure_depth(planet, rock));
        }
    }
    return floor;
}

double cave_district(const Context& context, DVec3 direction) noexcept {
    const Noise field =
        fbm(direction * context.planet->radius,
            {.wavelength = district_wavelength, .gain = 0.5, .finest = district_wavelength / 2.0},
            0.0, context.seed(Term::CaveDistricts));
    return smoothstep((field.value + 0.1) / 0.4);
}

namespace {

double cave_nearest(const Context& context, const Layering& layers, DVec3 direction, double radius,
                    double ground, double cover, double lod, bool bound) noexcept {
    const Planet& planet = *context.planet;
    const bool watered = planet.volatiles != Volatile::None;
    // Bounded, as open as the rock is a reach higher: it only closes with depth.
    const double depth = ground - radius;
    const double opening = bound ? std::max(0.0, depth - cave_reach) : depth;
    if (depth >= cave_floor(context)) {
        return cave_reach;
    }

    // The rock the point is in and that beside it, each with how far the point lies outside it.
    struct Unit {
        Material rock = Material::Air;
        double outside = 0.0;
    };
    // Bounded, every bed within reach, as a cave in any of them could be the nearest.
    const std::int64_t beds_about =
        bound ? static_cast<std::int64_t>(cave_reach / bed_thickness) + 1 : 1;
    std::array<Unit, 16> units{};
    std::size_t count = 0;
    const double base = ground - layers.cover; // beds lie over it, the basement under it
    double nearest = cave_reach;
    if (layers.cover > 0.0) {
        const std::int64_t bed = bed_at(context, layers, radius);
        for (std::int64_t near = bed - beds_about; near <= bed + beds_about; ++near) {
            const double bottom = bed_bottom(context, layers, near);
            const double top = bottom + bed_thickness;
            if (top > base && bottom < ground && count < units.size()) {
                units.at(count++) = {.rock = bed_rock(context, near),
                                     .outside =
                                         std::max({bottom - radius, radius - top, base - radius})};
            }
        }
    }
    units.at(count++) = {.rock = layers.basement, .outside = radius - base};

    Fields fields{context, direction, radius, lod};
    for (const Unit& unit : std::span{units}.first(count)) {
        const double closure = closure_depth(planet, unit.rock);
        const double open = openness(unit.rock, opening, watered) *
                            (1.0 - smoothstep((opening - closure) / closure));
        if (open <= 0.0 || unit.outside >= nearest) {
            continue;
        }
        const double inside =
            cave_in(context, fields, unit.rock, open, watered, cover, lod, planet, bound);
        nearest = std::min(nearest, std::max(inside, unit.outside));
    }
    return nearest;
}

} // namespace

double cave_distance(const Context& context, const Layering& layers, DVec3 direction, double radius,
                     double ground, double cover, double lod) noexcept {
    return cave_nearest(context, layers, direction, radius, ground, cover, lod, false);
}

double cave_bound(const Context& context, const Layering& layers, DVec3 direction, double radius,
                  double ground, double cover, double lod) noexcept {
    return cave_nearest(context, layers, direction, radius, ground, cover, lod, true);
}

} // namespace hog::globe::terms

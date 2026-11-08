#include "terms.hpp"

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/maths.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::globe::terms {
namespace {

// The smallest crater drawn, and each size bin twice the last.
constexpr double smallest = 4.0;
constexpr int bin_count = 24;

// Neukum's lunar chronology: craters over 1 km across per km² on a surface `age` Gyr old.
double per_km2_over_1km(double age) {
    return (5.44e-14 * (maths::exp(6.93 * age) - 1.0)) + (8.38e-4 * age);
}

// Cumulative craters over `diameter` metres per km²: falling as D⁻² above a kilometre and more
// steeply, D⁻³, below; never more than the equilibrium the lunar maria show, where new craters
// erase old ones as fast as they come, about 6% of the craters that would tile the ground.
double per_km2_over(double diameter, double over_1km) {
    const double km = diameter / 1'000.0;
    const double produced = km >= 1.0 ? over_1km / (km * km) : over_1km / (km * km * km);
    const double equilibrium = 0.1 / (km * km);
    return std::min(produced, equilibrium);
}

// How fast slopes creep smooth, m²/Myr: about 5 on the airless Moon, faster where wind moves
// dust, far faster where water runs.
double diffusivity(const Derived& derived) {
    if (derived.fluvial) {
        return 1e4;
    }
    return derived.aeolian ? 50.0 : 5.0;
}

// Air thick enough burns up the small impactors: Venus's 9.2 MPa leaves no crater under about
// 3 km across.
double atmosphere_cut(double pressure) {
    return 3'000.0 * pressure / 9.2e6;
}

// Ejecta thin as r⁻³ beyond the rim, and stop by this many radii.
constexpr double reach = 2.5;

double unit(std::uint64_t value) {
    return static_cast<double>(value >> 11U) * 0x1.0p-53;
}

} // namespace

namespace {

// Ejecta, thinning from the rim and eased to nothing at the reach.
CraterProfile ejecta(double x, double rim) {
    if (x >= reach) {
        return {};
    }
    const double fade = 1.0 - smoothstep((x - 1.8) / (reach - 1.8));
    const double fade_slope = -smoothstep_slope((x - 1.8) / (reach - 1.8)) / (reach - 1.8);
    const double thin = 1.0 / (x * x * x);
    return {.height = rim * thin * fade,
            .slope = rim * ((-3.0 * thin / x * fade) + (thin * fade_slope))};
}

// The bowl, carried on past the rim as the same curve.
CraterProfile bowl(double x, double size, double depth, double rim) {
    // Complex craters' floors flatten out from the middle as they grow.
    const double floor = size < 1.0 ? 0.0 : 0.2 + (0.3 * smoothstep((size - 1.0) / 4.0));
    CraterProfile shape{.height = rim - depth, .slope = 0.0};
    if (x > floor) {
        const double t = (x - floor) / (1.0 - floor);
        shape = {.height = rim - (depth * (1.0 - (t * t))),
                 .slope = depth * 2.0 * t / (1.0 - floor)};
    }
    if (size >= 1.0) {
        // A central peak, rebounded from the floor, half the depth high.
        constexpr double peak_width = 0.15;
        const double p = x / peak_width;
        if (p < 1.0) {
            const double bump = 1.0 - (p * p);
            shape.height += 0.5 * depth * bump * bump * bump;
            shape.slope += 0.5 * depth * 3.0 * bump * bump * (-2.0 * p / peak_width);
        }
    }
    return shape;
}

} // namespace

CraterProfile crater_profile(double x, double diameter, double transition,
                             double rounding) noexcept {
    const double size = diameter / transition;
    // Pike's fits to fresh lunar craters, joined at the transition: bowls a fifth as deep as
    // wide, then shallower as D^0.3; rims 3.6% of the width, then rising as D^0.4.
    const double depth = 0.2 * (size < 1.0 ? diameter : transition * maths::pow(size, 0.3));
    const double rim = 0.036 * (size < 1.0 ? diameter : transition * maths::pow(size, 0.4));
    const double width = std::clamp(rounding, 0.0, 0.5);
    if (x >= 1.0 + width) {
        return ejecta(x, rim);
    }
    if (x <= 1.0 - width) {
        return bowl(x, size, depth, rim);
    }
    // Across the crest, the wall gives way to the ejecta smoothly; both stand at the rim's height
    // there, so only the slope turns.
    const CraterProfile inner = bowl(x, size, depth, rim);
    const CraterProfile outer = ejecta(x, rim);
    const double s = smoothstep((x - (1.0 - width)) / (2.0 * width));
    const double s_slope = smoothstep_slope((x - (1.0 - width)) / (2.0 * width)) / (2.0 * width);
    return {.height = inner.height + ((outer.height - inner.height) * s),
            .slope = inner.slope + ((outer.slope - inner.slope) * s) +
                     ((outer.height - inner.height) * s_slope)};
}

double craters_over(const Planet& planet, double diameter) noexcept {
    return per_km2_over(std::max(diameter, atmosphere_cut(planet.pressure)),
                        planet.impact_flux * per_km2_over_1km(planet.surface_age));
}

namespace {

// What the craters of one size bin need to know, shared by every cell they're drawn in.
struct Bin {
    int index = 0;
    double diameter = 0.0; // the smallest in the bin
    double slope = 0.0;    // of the cumulative count against size
    double side = 0.0;     // of the cells they're drawn in, metres
    double expected = 0.0; // in each cell
};

// How many fell in a cell, a Poisson draw by inversion.
int fallen(double expected, double draw) {
    double chance = maths::exp(-expected);
    double cumulative = chance;
    int count = 0;
    while (draw > cumulative && count < 16) {
        ++count;
        chance *= expected / count;
        cumulative += chance;
    }
    return count;
}

// A crater of a bin as it falls in its cell: where, how wide, and how much of it shows at the
// detail asked for and how rounded its crest.
struct Crater {
    DVec3 centre{0.0};
    double radius = 0.0; // metres
    double size = 0.0;   // across
    double shown = 0.0;
    double rounding = 0.0; // metres
};

// Each crater of the bin in `cell` that shows at `lod`, none smaller than `least` across, in the
// order they fell.
template <typename Visit>
void craters_in(const Context& context, const Bin& bin, Cell cell, double lod, double least,
                const Visit& visit) {
    const Planet& planet = *context.planet;
    const double radius = planet.radius;
    const std::uint64_t seed =
        hash_values(context.seed(Term::Craters), bin.index, cell.face, cell.i, cell.j);
    const int count = fallen(bin.expected, unit(hash_values(seed, 0)));
    if (count == 0) {
        return;
    }
    const DVec3 middle = center(cell);
    const DVec3 side_axis = std::abs(middle.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(side_axis, middle));
    const DVec3 north = glm::cross(middle, east);
    const double kappa = diffusivity(*context.derived);
    for (int crater = 1; crater <= count; ++crater) {
        const std::uint64_t own = hash_values(seed, crater);
        const DVec3 centre =
            glm::normalize(middle + ((((unit(hash_values(own, 1)) - 0.5) * east) +
                                      ((unit(hash_values(own, 2)) - 0.5) * north)) *
                                     (bin.side / radius)));
        // Within the bin, sizes fall off as the counts do.
        const double size =
            bin.diameter *
            maths::pow(1.0 - (unit(hash_values(own, 3)) * (1.0 - maths::pow(2.0, -bin.slope))),
                       -1.0 / bin.slope);
        if (size < least) {
            continue;
        }
        const double crater_radius = 0.5 * size;
        // Worn by creep over its age: features narrower than the creep's reach fade.
        const double age = unit(hash_values(own, 4)) * planet.surface_age * 1'000.0;
        const double crept = std::sqrt(kappa * age);
        const double creep = crept / (0.3 * crater_radius);
        // And faded in as the detail asked for reaches them.
        const double shown = smoothstep((size / (2.0 * lod)) - 1.0) / (1.0 + (creep * creep));
        if (shown < 0.01) {
            continue;
        }
        // The crest is rounded as far as creep has worn it, and no sharper than the detail.
        visit(Crater{.centre = centre,
                     .radius = crater_radius,
                     .size = size,
                     .shown = shown,
                     .rounding = std::max(crept, lod)});
    }
}

// The crater as it shapes the ground at `direction`.
void add_crater(Noise& sum, const Context& context, DVec3 direction, const Crater& crater) {
    const double radius = context.planet->radius;
    const double apart = radius * maths::atan2(glm::length(glm::cross(direction, crater.centre)),
                                               glm::dot(direction, crater.centre));
    if (apart >= reach * crater.radius) {
        return;
    }
    const CraterProfile shape =
        crater_profile(apart / crater.radius, crater.size, context.derived->crater_transition,
                       crater.rounding / crater.radius);
    sum.value += crater.shown * shape.height;
    const DVec3 outward = tangential(direction - crater.centre, direction);
    const double out_length = glm::length(outward);
    if (out_length > 1e-15) {
        sum.gradient += (crater.shown * shape.slope / crater.radius / out_length) * outward;
    }
}

// Each bin drawn at `lod` and the level of the cells its craters are drawn in, smallest first;
// none on a world without craters.
template <typename Visit>
void bins_at(const Context& context, double least, const Visit& visit) {
    const Planet& planet = *context.planet;
    const double over_1km = planet.impact_flux * per_km2_over_1km(planet.surface_age);
    if (over_1km <= 0.0) {
        return;
    }
    // A cell's mean side at level 0, in metres.
    const double side_at_root = std::sqrt(4.0 * std::numbers::pi / 6.0) * planet.radius;
    for (int index = 0; index < bin_count; ++index) {
        const double diameter = std::ldexp(smallest, index);
        if (diameter > 0.5 * planet.radius) {
            break;
        }
        if (2.0 * diameter < least) {
            continue;
        }
        // Cells wide enough that a crater's reach never runs past the cells around its own.
        const int level = std::clamp(
            static_cast<int>(std::floor(maths::log(side_at_root / (reach * diameter * 1.25)) /
                                        std::numbers::ln2)),
            0, Cell::max_level);
        Bin bin{.index = index,
                .diameter = diameter,
                .slope = diameter >= 1'000.0 ? 2.0 : 3.0,
                .side = side_at_root / std::ldexp(1.0, level)};
        bin.expected = (per_km2_over(diameter, over_1km) - per_km2_over(2.0 * diameter, over_1km)) *
                       bin.side * bin.side * 1e-6;
        visit(bin, level);
    }
}

double least_shown(const Planet& planet, double lod) {
    return std::max({smallest, atmosphere_cut(planet.pressure), 2.0 * lod});
}

// Each crater of a bin about `home`, its own cell's then those around it, in that order.
template <typename Visit>
void craters_about(const Context& context, const Bin& bin, Cell home, double lod, double least,
                   const Visit& visit) {
    craters_in(context, bin, home, lod, least, visit);
    const CellNeighbors near = around(home);
    for (const Cell cell : near.all()) {
        craters_in(context, bin, cell, lod, least, visit);
    }
}

} // namespace

Noise craters(const Context& context, DVec3 direction, double lod) noexcept {
    const double least = least_shown(*context.planet, lod);
    Noise sum;
    bins_at(context, least, [&](const Bin& bin, int level) {
        craters_about(context, bin, cell_under(direction, level), lod, least,
                      [&](const Crater& crater) { add_crater(sum, context, direction, crater); });
    });
    return sum;
}

void craters(const Context& context, std::span<const Cell> cells, double lod,
             std::span<Noise> into) {
    HOG_ASSERT(cells.size() == into.size(), "a sum for each cell");
    const double least = least_shown(*context.planet, lod);
    std::vector<DVec3> directions;
    directions.reserve(cells.size());
    for (const Cell cell : cells) {
        directions.push_back(center(cell));
    }
    std::unordered_map<std::uint64_t, std::vector<Crater>> about;
    bins_at(context, least, [&](const Bin& bin, int level) {
        about.clear();
        for (std::size_t n = 0; n < cells.size(); ++n) {
            const Cell cell = cells[n];
            // A cell's middle lies well inside the coarser cell holding it.
            const auto shift = static_cast<unsigned>(cell.level - level);
            const Cell home = std::cmp_less_equal(level, cell.level)
                                  ? Cell{.level = static_cast<std::uint8_t>(level),
                                         .face = cell.face,
                                         .i = cell.i >> shift,
                                         .j = cell.j >> shift}
                                  : cell_under(directions[n], level);
            auto [found, added] = about.try_emplace(index_of(home));
            if (added) {
                craters_about(context, bin, home, lod, least,
                              [&](const Crater& crater) { found->second.push_back(crater); });
            }
            for (const Crater& crater : found->second) {
                add_crater(into[n], context, directions[n], crater);
            }
        }
    });
}

} // namespace hog::globe::terms

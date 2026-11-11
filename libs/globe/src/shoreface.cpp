#include "shoreface.hpp"

#include <hog/core/portable_maths.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

namespace hog::globe {

namespace {

constexpr double water_density = 1000.0;
constexpr double viscosity = 1.0e-6; // m²/s, water's
// Ferguson and Church's constants for natural grains: Stokes' drag and the turbulent one.
constexpr double stokes = 18.0;
constexpr double turbulent = 1.0;
// Waves break in water about this much deeper than they're high (McCowan 1894).
constexpr double breaking = 0.78;
// Bars stand from trough to crest about half the depth over them, their crests at about three
// fifths of their troughs' depth.
constexpr double bar_relief = 0.5;
// Halvings a bracket is narrowed by to find where a beach comes to hold what it's built of.
constexpr int halvings = 40;

double smoothstep(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// Metres a second grains `size` across, `submerged` times as heavy as the water they displace,
// sink through it.
double sinking(double size, double submerged, double gravity) {
    const double weight = submerged * gravity;
    return weight * size * size /
           ((stokes * viscosity) + std::sqrt(0.75 * turbulent * weight * size * size * size));
}

// The integral of max(0, `at` + `rate` x) over x from `from` to `to`.
double above_line(double at, double rate, double from, double to) {
    if (to <= from) {
        return 0.0;
    }
    const auto line = [&](double x) { return at + (rate * x); };
    // Clipped to where it's above none.
    if (rate != 0.0) {
        const double zero = -at / rate;
        if (rate > 0.0) {
            from = std::max(from, zero);
        } else {
            to = std::min(to, zero);
        }
    } else if (at <= 0.0) {
        return 0.0;
    }
    if (to <= from) {
        return 0.0;
    }
    return 0.5 * (line(from) + line(to)) * (to - from);
}

// The integral of max(0, `at` + `rate` h + (h / `scale`)^1.5) over h from none to `to`, `at` never
// below none: the sand between a shoreface and a plane, down its depths.
double above_shoreface(double at, double rate, double scale, double to) {
    if (to <= 0.0) {
        return 0.0;
    }
    const double cube = scale * std::sqrt(scale);
    const auto span = [&](double h) { return at + (rate * h) + (h * std::sqrt(h) / cube); };
    const auto whole = [&](double h) {
        return (at * h) + (0.5 * rate * h * h) + (0.4 * h * h * std::sqrt(h) / cube);
    };
    // Falling at first only where the plane falls away faster than the shoreface's start, it
    // dips lowest where its rate comes to none.
    if (rate >= 0.0) {
        return whole(to);
    }
    const double root = -2.0 * rate * cube / 3.0;
    const double lowest = root * root;
    if (span(lowest) >= 0.0) {
        return whole(to);
    }
    // Newton's way to either side of its lowest comes to where it crosses none without
    // overshooting, it being convex: from where it starts, and from past where it's risen again.
    const auto rate_at = [&](double h) { return rate + (1.5 * std::sqrt(h) / cube); };
    const auto root_from = [&](double h) {
        for (int step = 0; step < 64; ++step) {
            const double next = h - (span(h) / rate_at(h));
            if (std::abs(next - h) <= 1e-12 * (h + 1.0)) {
                return next;
            }
            h = next;
        }
        return h;
    };
    const double first = root_from(0.0);
    if (to <= first) {
        return whole(to);
    }
    double sum = whole(first);
    double past = 2.0 * lowest;
    while (span(past) < 0.0) {
        past *= 2.0;
    }
    const double second = root_from(past);
    if (to > second) {
        sum += whole(to) - whole(second);
    }
    return sum;
}

} // namespace

Grains grains_of(Material material, double gravity) {
    const Properties& of = properties(material);
    const double solid = of.density / std::max(1.0 - of.porosity, 1e-6);
    const double submerged = std::max((solid / water_density) - 1.0, 1e-3);
    double small = 1e-7;
    double large = 1.0;
    for (int step = 0; step < 64; ++step) {
        const double middle = std::sqrt(small * large);
        (sinking(middle, submerged, gravity) < of.settling ? small : large) = middle;
    }
    const double soaked = of.saturated_repose > 0.0 ? of.saturated_repose : of.repose;
    const double dry = of.repose > 0.0 ? of.repose : soaked;
    return {.size = std::sqrt(small * large),
            .settling = of.settling,
            // Dean (1987), from the settling in centimetres a second.
            .scale = 0.067 * maths::pow(100.0 * of.settling, 0.44),
            .steepest = maths::tan(soaked),
            .dry = maths::tan(dry)};
}

Grains mixed(std::span<const Grains> grains, std::span<const double> shares) {
    Grains mix;
    for (std::size_t kind = 0; kind < grains.size(); ++kind) {
        mix.size += grains[kind].size * shares[kind];
        mix.settling += grains[kind].settling * shares[kind];
        mix.scale += grains[kind].scale * shares[kind];
        mix.steepest += grains[kind].steepest * shares[kind];
        mix.dry += grains[kind].dry * shares[kind];
    }
    return mix;
}

double face_slope(const Sea& sea, const Grains& grains, double gravity) {
    if (sea.usual <= 0.0 || sea.period <= 0.0 || grains.size <= 0.0) {
        return grains.steepest;
    }
    const double stirring = sea.usual / (std::sqrt(gravity * grains.size) * sea.period);
    return std::min(0.12 / std::sqrt(stirring), grains.steepest);
}

double runup(const Sea& sea, double face, double gravity) {
    const double length = gravity * sea.period * sea.period / (2.0 * std::numbers::pi);
    const double reach = sea.usual * length;
    return 1.1 * ((0.35 * face * std::sqrt(reach)) +
                  (0.5 * std::sqrt(reach * ((0.563 * face * face) + 0.004))));
}

double closure_depth(const Sea& sea, double gravity) {
    if (sea.period <= 0.0) {
        return 0.0;
    }
    return std::max(0.0, (2.28 * sea.storm) -
                             (68.5 * sea.storm * sea.storm / (gravity * sea.period * sea.period)));
}

Profile profile_for(const Sea& sea, const Grains& grains, double gravity) {
    const double face = face_slope(sea, grains, gravity);
    return {.scale = grains.scale,
            .face = face,
            .berm = runup(sea, face, gravity),
            .closure = closure_depth(sea, gravity),
            .toe = grains.steepest,
            .back = grains.dry};
}

double out_to(const Profile& profile, double depth) {
    const double down = std::max(depth, 0.0) / profile.scale;
    return (std::max(depth, 0.0) / profile.face) + (down * std::sqrt(down));
}

Across across(const Profile& profile, double out) {
    if (out <= 0.0) {
        const double height = -profile.face * out;
        return height >= profile.berm ? Across{.height = profile.berm, .slope = 0.0}
                                      : Across{.height = height, .slope = -profile.face};
    }
    const double reach = out_to(profile, profile.closure);
    if (out >= reach) {
        return {.height = -profile.closure - (profile.toe * (out - reach)), .slope = -profile.toe};
    }
    // Newton's way down from above to where out_to comes to `out`, which it does from above
    // without overshooting, rising ever slower.
    const double scale = profile.scale;
    double depth = std::min(profile.face * out, scale * maths::pow(out, 2.0 / 3.0));
    for (int step = 0; step < 32; ++step) {
        const double root = std::sqrt(depth / scale);
        const double over = (depth / profile.face) + (depth / scale * root) - out;
        const double rate = (1.0 / profile.face) + (1.5 * root / scale);
        depth = std::max(0.0, depth - (over / rate));
        if (std::abs(over) <= 1e-9 * out) {
            break;
        }
    }
    const double rate = (1.0 / profile.face) + (1.5 * std::sqrt(depth / profile.scale) / scale);
    return {.height = -depth, .slope = -1.0 / rate};
}

double held(const Profile& profile, const Ground& ground, double ahead, double top) {
    // Summed by height: at each, from the ground out to the beach's top, up to `top`.
    const double inland = 1.0 / ground.slope;
    const double over =
        above_line(ahead, inland - (1.0 / profile.face), 0.0, std::min(profile.berm, top));
    const double from = std::max(0.0, -top);
    const double rate = (1.0 / profile.face) - inland;
    const double deepest = std::min(profile.closure, ground.floor);
    const double shoreface = from < deepest ? above_shoreface(ahead, rate, profile.scale, deepest) -
                                                  above_shoreface(ahead, rate, profile.scale, from)
                                            : 0.0;
    const double reach = out_to(profile, profile.closure);
    const double toe =
        above_line(ahead + reach - (profile.closure / profile.toe), (1.0 / profile.toe) - inland,
                   std::max(profile.closure, from), ground.floor);
    return over + shoreface + toe;
}

Profile as_built(const Profile& profile, const Built& built) {
    Profile cut = profile;
    cut.closure = std::min(profile.closure, built.down);
    return cut;
}

namespace {

// Where a beach of `volume` lies, `holds` telling how much it holds built so: gathering at the
// ground's foot `lowest` metres up and filling up from there until it comes up to the beach's berm;
// then its face built out from the waterline, at least `least_ahead` for it to stand whole; then
// its shoreface reaching down as far as there's sand for; then the whole of it built out.
template <typename Holds>
Built build(const Profile& profile, const Holds& holds, double volume, double least_ahead,
            double lowest) {
    if (volume < holds(Built{})) {
        double low = lowest;
        double high = profile.berm;
        for (int step = 0; step < halvings; ++step) {
            const double middle = 0.5 * (low + high);
            (holds(Built{.top = middle}) < volume ? low : high) = middle;
        }
        return {.top = 0.5 * (low + high)};
    }
    if (volume < holds(Built{.ahead = least_ahead})) {
        double near = 0.0;
        double far = least_ahead;
        for (int step = 0; step < halvings; ++step) {
            const double middle = 0.5 * (near + far);
            (holds(Built{.ahead = middle}) < volume ? near : far) = middle;
        }
        return {.ahead = 0.5 * (near + far)};
    }
    if (volume < holds(Built{.ahead = least_ahead, .down = profile.closure})) {
        double shallow = 0.0;
        double deep = profile.closure;
        for (int step = 0; step < halvings; ++step) {
            const double middle = 0.5 * (shallow + deep);
            (holds(Built{.ahead = least_ahead, .down = middle}) < volume ? shallow : deep) = middle;
        }
        return {.ahead = least_ahead, .down = 0.5 * (shallow + deep)};
    }
    const auto whole = [&](double ahead) {
        return holds(Built{.ahead = ahead, .down = profile.closure});
    };
    double near = least_ahead;
    double far = least_ahead + 1.0;
    for (int step = 0; step < 64 && whole(far) < volume; ++step) {
        near = far;
        far = least_ahead + (2.0 * (far - least_ahead));
    }
    for (int step = 0; step < halvings; ++step) {
        const double middle = 0.5 * (near + far);
        (whole(middle) < volume ? near : far) = middle;
    }
    return {.ahead = 0.5 * (near + far), .down = profile.closure};
}

// A beach's top `out` metres out from the waterline of ground that rises over its berm `inland`
// metres out, its shoreface's depth `depth` metres out from its own waterline.
template <typename Depth>
Across top_with(const Profile& profile, const Built& built, double inland, double out,
                const Depth& depth) {
    Across top;
    const double from = out - built.ahead;
    const double down = std::min(profile.closure, built.down);
    if (out < inland) {
        top = {.height = profile.berm - (profile.back * (inland - out)), .slope = profile.back};
    } else if (from <= 0.0) {
        const double height = -profile.face * from;
        top = height >= profile.berm ? Across{.height = profile.berm, .slope = 0.0}
                                     : Across{.height = height, .slope = -profile.face};
    } else if (const double reach = out_to(profile, down); from < reach) {
        top = depth(from);
    } else {
        top = {.height = -down - (profile.toe * (from - reach)), .slope = -profile.toe};
    }
    // None over what it's filled up to.
    if (top.height > built.top) {
        top = {.height = built.top, .slope = 0.0};
    }
    return top;
}

// The ground across a shore `out` metres out from its waterline.
double ground_at(const Section& section, double out) {
    const auto& outs = section.out;
    const auto& heights = section.height;
    constexpr std::size_t last = Section::samples - 1;
    std::size_t at = 0;
    while (at + 1 < last && outs[at + 1] < out) {
        ++at;
    }
    const double across = outs[at + 1] - outs[at];
    return heights[at] + ((heights[at + 1] - heights[at]) * (out - outs[at]) / across);
}

} // namespace

std::optional<Built> built_on(const Profile& profile, const Ground& ground, double volume) {
    if (volume <= 0.0 || ground.floor <= 0.0) {
        return std::nullopt;
    }
    const auto holds = [&](const Built& built) {
        return held(as_built(profile, built), ground, built.ahead, built.top);
    };
    const double least_ahead =
        std::max(0.0, profile.berm * ((1.0 / profile.face) - (1.0 / ground.slope)));
    return build(profile, holds, volume, least_ahead, -ground.floor);
}

std::optional<Built> built_along(const Profile& profile, const Section& section, double volume) {
    if (volume <= 0.0 || section.height.back() >= 0.0) {
        return std::nullopt;
    }
    // Summed across, slice by slice, its shoreface's depths read off a table of them.
    constexpr int slices = 256;
    const double from = section.out.front();
    const double width = (section.out.back() - from) / slices;
    std::array<double, slices> ground{};
    for (std::size_t at = 0; at < slices; ++at) {
        ground[at] = ground_at(section, from + ((static_cast<double>(at) + 0.5) * width));
    }
    constexpr int rows = 256;
    const double reach = out_to(profile, profile.closure);
    std::array<double, rows + 1> depths{};
    for (std::size_t row = 0; row <= rows; ++row) {
        depths[row] = across(profile, reach * static_cast<double>(row) / rows).height;
    }
    const auto depth = [&](double out) {
        const double place = std::clamp(out / reach * rows, 0.0, static_cast<double>(rows));
        const auto row = std::min(static_cast<std::size_t>(place), std::size_t{rows - 1});
        const double part = place - static_cast<double>(row);
        return Across{.height = depths[row] + ((depths[row + 1] - depths[row]) * part)};
    };
    const double inland = out_at(section, profile.berm).height;
    const auto holds = [&](const Built& built) {
        double sum = 0.0;
        for (std::size_t at = 0; at < slices; ++at) {
            const double out = from + ((static_cast<double>(at) + 0.5) * width);
            sum += std::max(0.0, top_with(profile, built, inland, out, depth).height - ground[at]);
        }
        return sum * width;
    };
    const double least_ahead = std::max(0.0, (profile.berm / profile.face) + inland);
    return build(profile, holds, volume, least_ahead, section.height.back());
}

Across top_along(const Profile& profile, const Section& section, const Built& built, double out) {
    return top_with(profile, built, out_at(section, profile.berm).height, out,
                    [&](double from) { return across(profile, from); });
}

std::optional<Across> beach_top(const Profile& profile, const Ground& ground, const Built& built,
                                double out) {
    const double inland = -profile.berm / ground.slope;
    Across top = out < inland ? Across{.height = profile.berm - (profile.back * (inland - out)),
                                       .slope = profile.back}
                              : across(as_built(profile, built), out - built.ahead);
    // None over what it's filled up to.
    if (top.height > built.top) {
        top = {.height = built.top, .slope = 0.0};
    }
    return top;
}

Bars bars_on(const Profile& profile, const Sea& sea, const Grains& grains, double gravity) {
    Bars bars;
    if (sea.storm <= 0.0 || sea.usual <= 0.0 || sea.period <= 0.0 || profile.closure <= 0.0) {
        return bars;
    }
    // Short and Aagaard's bar parameter: the shoreface's width against the waves' length, by
    // its slope. The gentler and wider, the more bars.
    const double reach = out_to(profile, profile.closure);
    const double parameter = reach * reach / (gravity * sea.period * sea.period * profile.closure);
    constexpr std::array<double, Bars::most> thresholds{20.0, 50.0, 100.0, 400.0};
    bars.count = static_cast<int>(
        std::ranges::count_if(thresholds, [&](double from) { return parameter >= from; }));
    // Where an ordinary day's waves break, out to where storms' do.
    const double inner = std::min(sea.usual / breaking, profile.closure);
    const double outer = std::max(inner, std::min(sea.storm / breaking, profile.closure));
    double inshore = 0.0;
    for (int bar = 0; bar < bars.count; ++bar) {
        const double along =
            bars.count == 1 ? 0.0 : static_cast<double>(bar) / static_cast<double>(bars.count - 1);
        const double depth = inner * maths::pow(outer / inner, along);
        const auto at = static_cast<std::size_t>(bar);
        bars.crest[at] = out_to(profile, depth);
        bars.width[at] = 0.5 * (bars.crest[at] - inshore);
        bars.relief[at] = bar_relief * depth;
        inshore = bars.crest[at];
    }
    // Dean's number: how far the waves lift the grains against how fast they sink. Steep beaches
    // reflect the waves and have no bars, flat ones lose them over straight bars, and those
    // between cut theirs into crescents.
    const double lifted = sea.usual / (grains.settling * sea.period);
    bars.rhythm = smoothstep(1.0, 2.0, lifted) * (1.0 - smoothstep(5.0, 6.0, lifted));
    return bars;
}

Across bars_at(const Bars& bars, double out, double shift) {
    Across sum;
    for (int at = 0; at < bars.count; ++at) {
        const auto bar = static_cast<std::size_t>(at);
        const double width = bars.width[bar];
        if (width <= 0.0) {
            continue;
        }
        // One whole wave of sand moved from the trough to the crest, as much taken as laid.
        const double phase = (out - bars.crest[bar] - (shift * width)) / width;
        if (phase < -1.5 || phase > 0.5) {
            continue;
        }
        const double half = 0.5 * bars.relief[bar];
        sum.height += half * maths::cos(std::numbers::pi * phase);
        sum.slope -= half * std::numbers::pi / width * maths::sin(std::numbers::pi * phase);
    }
    return sum;
}

Across out_at(const Section& section, double height) {
    const auto& out = section.out;
    const auto& heights = section.height;
    constexpr std::size_t last = Section::samples - 1;
    // The stretch it falls in, its ends' stretches running on past them.
    std::size_t at = 0;
    while (at + 1 < last && heights[at + 1] > height) {
        ++at;
    }
    const double fall = heights[at] - heights[at + 1];
    const double per_metre = fall > 0.0 ? (out[at + 1] - out[at]) / fall : 0.0;
    return {.height = out[at] + ((heights[at] - height) * per_metre), .slope = per_metre};
}

Across lying_over(const Laid& laid, double height, double shift) {
    const Across placed = out_at(laid.section, height);
    const double out = placed.height;
    Across surface = top_along(laid.profile, laid.section, laid.built, out);
    // Each bar runs out to nothing either side, the outermost on past where the waves stop
    // stirring the bed by as far as it reaches.
    const double offshore = out - laid.built.ahead;
    if (offshore > 0.0) {
        const Across bar = bars_at(laid.bars, offshore, shift);
        surface.height += bar.height;
        surface.slope += bar.slope;
    }
    const double thick = surface.height - height;
    if (thick <= 0.0) {
        return {};
    }
    // A metre higher is that much further in.
    return {.height = thick, .slope = (-surface.slope * placed.slope) - 1.0};
}

} // namespace hog::globe

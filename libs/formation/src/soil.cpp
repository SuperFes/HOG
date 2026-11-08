#include <hog/core/portable_maths.hpp>
#include <hog/formation/soil.hpp>

#include <algorithm>

namespace hog::formation {

namespace {

double smoothstep(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

} // namespace

double soil_making(double celsius, double rain, const Body& body) {
    const double wet = std::max(rain, 0.0) / (std::max(rain, 0.0) + 0.5);
    // A yearly mean well under freezing still thaws in summer, enough for thin soils to form.
    const double thawed = smoothstep(-15.0, 5.0, celsius);
    const double chemical = maths::exp(body.weathering_warmth * (celsius - 15.0)) * wet * thawed;
    // Water freezing in cracks shatters rock fastest a few degrees under freezing (Hales and
    // Roering), needing far less water than rock rotting does.
    const double frost_at = (celsius + 5.0) / 5.0;
    const double damp = std::max(rain, 0.0) / (std::max(rain, 0.0) + 0.1);
    const double frost = body.frost_weathering * maths::exp(-(frost_at * frost_at)) * damp;
    return body.soil_making * (chemical + frost);
}

double balanced_depth(double making, double wear, const Body& body) {
    const double worn = std::max(wear, body.slowest_wear);
    if (making <= worn) {
        return 0.0;
    }
    return maths::log(making / worn) / body.soil_falloff;
}

double grown_depth(double making, double wear, double years, const Body& body) {
    const double falloff = body.soil_falloff;
    const double worn = std::max(wear, body.slowest_wear);
    const double time = std::max(years, 0.0);
    if (making <= 0.0) {
        return 0.0;
    }
    if (worn <= 0.0) {
        return maths::log(1.0 + (falloff * making * time)) / falloff;
    }
    // dh/dt = making·e^(-falloff·h) - worn from h = 0, solved for e^(falloff·h).
    const double ratio = making / worn;
    const double grown = ratio + ((1.0 - ratio) * maths::exp(-falloff * worn * time));
    return grown > 1.0 ? maths::log(grown) / falloff : 0.0;
}

namespace {

// What was laid at a point, top down: what water laid (in a hollow or along a river), then the till
// ice laid before it.
struct Laid {
    double water = 0.0;
    double till = 0.0;
    bool hollow = false;
};

// How deep soil grows over what was laid on rock: from the loose deposit where it's deep enough to
// hold all the soil the years allow, otherwise from the rock under a thin veneer of it.
double grown_over(double rock_making, double wear, double laid, double years, const Body& body) {
    if (laid <= 0.0) {
        return grown_depth(rock_making, wear, years, body);
    }
    // Ground that gathers what's laid on it wears only as slowly as the stillest land.
    const double depth = grown_depth(rock_making * body.loose_weathering, 0.0, years, body);
    if (depth <= laid) {
        return depth;
    }
    return std::max(laid, grown_depth(rock_making, wear, years, body));
}

Soil bare(std::uint32_t size) {
    Soil soil;
    soil.depth.assign(size, 0.0);
    soil.alluvium.assign(size, 0.0);
    soil.lake_beds.assign(size, 0.0);
    soil.till.assign(size, 0.0);
    soil.making.assign(size, 0.0);
    soil.years.assign(size, 0.0);
    soil.parent.assign(size, Parent::None);
    return soil;
}

void leave(Soil& soil, std::uint32_t point, const Laid& laid) {
    (laid.hollow ? soil.lake_beds : soil.alluvium)[point] = laid.water;
    soil.till[point] = laid.till;
}

// Soil grows down through what was laid, leaving the rest under it; its parent is what most of it
// formed from.
void grow(Soil& soil, std::uint32_t point, const Laid& laid, double rock_making, double wear,
          double years, const Body& body) {
    const double depth = grown_over(rock_making, wear, laid.water + laid.till, years, body);
    const double from_water = std::min(laid.water, depth);
    const double from_till = std::min(laid.till, depth - from_water);
    const double from_rock = depth - from_water - from_till;
    leave(soil, point,
          {.water = laid.water - from_water, .till = laid.till - from_till, .hollow = laid.hollow});
    // Bare ground would form soil from whatever lies on top.
    const double water = depth > 0.0 ? from_water : laid.water;
    const double till = depth > 0.0 ? from_till : laid.till;
    const double rock = depth > 0.0 ? from_rock : 0.0;
    Parent parent = Parent::Rock;
    if (till > rock) {
        parent = Parent::Till;
    }
    if (water > std::max(till, rock) || (depth <= 0.0 && water > 0.0)) {
        parent = laid.hollow ? Parent::LakeBed : Parent::Alluvium;
    }
    soil.depth[point] = depth;
    soil.making[point] = parent == Parent::Rock ? rock_making : rock_making * body.loose_weathering;
    soil.years[point] = years;
    soil.parent[point] = parent;
}

} // namespace

Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate, const Body& body) {
    Soil soil = bare(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] == point) {
            continue; // under the liquid
        }
        const double making = soil_making(climate.temperature[point], climate.rain[point], body);
        grow(soil, point, {}, making, rivers.wear[point], body.soil_years, body);
    }
    return soil;
}

Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate, const Ice& ice,
             const Lakes& lakes, const Sediment& sediment, const Body& body) {
    Soil soil = bare(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] == point) {
            continue; // under the liquid
        }
        const Laid laid{.water = sediment.alluvium[point],
                        .till = ice.till[point],
                        .hollow = lakes.basin[point] != Lakes::none};
        if (ice.thickness[point] > 0.0 || sediment.lake_depth[point] > 0.0 ||
            lakes.salt_flat[point] != 0) {
            leave(soil, point, laid);
            continue;
        }
        // Where the coldest times' ice lay, it scraped the older soil away.
        const double years = ice.coldest[point] > 0.0 ? body.ice_left_years : body.soil_years;
        const double making = soil_making(climate.temperature[point], climate.rain[point], body);
        grow(soil, point, laid, making, rivers.wear[point], years, body);
    }
    return soil;
}

Profile profile_at(const Sphere& sphere, const Soil& soil, const Sphere::Spot& spot) {
    return {.soil = sphere.sample(soil.depth, spot),
            .alluvium = sphere.sample(soil.alluvium, spot),
            .lake_beds = sphere.sample(soil.lake_beds, spot),
            .till = sphere.sample(soil.till, spot)};
}

Layer layer_at(const Profile& profile, double depth) {
    double bottom = profile.soil;
    if (depth < bottom) {
        return Layer::Soil;
    }
    bottom += profile.alluvium;
    if (depth < bottom) {
        return Layer::Alluvium;
    }
    bottom += profile.lake_beds;
    if (depth < bottom) {
        return Layer::LakeBed;
    }
    bottom += profile.till;
    return depth < bottom ? Layer::Till : Layer::Rock;
}

} // namespace hog::formation

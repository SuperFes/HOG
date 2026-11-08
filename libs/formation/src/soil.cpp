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

struct Grown {
    double depth = 0.0;
    double loose = 0.0;
    double making = 0.0;
    Parent parent = Parent::Rock;
};

// Soil over `till` and `alluvium` laid on rock: grown from the loose deposit where it's deep enough
// to hold all the soil the years allow, otherwise from the rock under a thin veneer of it.
Grown grown_at(double rock_making, double wear, double till, double alluvium, double years,
               const Body& body) {
    const double laid = till + alluvium;
    if (laid <= 0.0) {
        return {.depth = grown_depth(rock_making, wear, years, body),
                .loose = 0.0,
                .making = rock_making,
                .parent = Parent::Rock};
    }
    // Ground that gathers what's laid on it wears only as slowly as the stillest land.
    const double making = rock_making * body.loose_weathering;
    const double depth = grown_depth(making, 0.0, years, body);
    const Parent parent = till >= alluvium ? Parent::Till : Parent::Alluvium;
    if (depth <= laid) {
        return {.depth = depth, .loose = laid - depth, .making = making, .parent = parent};
    }
    const double on_rock = std::max(laid, grown_depth(rock_making, wear, years, body));
    const bool mostly_laid = laid >= on_rock / 2.0;
    return {.depth = on_rock,
            .loose = 0.0,
            .making = mostly_laid ? making : rock_making,
            .parent = mostly_laid ? parent : Parent::Rock};
}

Soil bare(std::uint32_t size) {
    Soil soil;
    soil.depth.assign(size, 0.0);
    soil.loose.assign(size, 0.0);
    soil.making.assign(size, 0.0);
    soil.years.assign(size, 0.0);
    soil.parent.assign(size, Parent::None);
    return soil;
}

void keep(Soil& soil, std::uint32_t point, const Grown& grown, double years) {
    soil.depth[point] = grown.depth;
    soil.loose[point] = grown.loose;
    soil.making[point] = grown.making;
    soil.years[point] = years;
    soil.parent[point] = grown.parent;
}

} // namespace

Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate, const Body& body) {
    Soil soil = bare(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] == point) {
            continue; // under the liquid
        }
        const double making = soil_making(climate.temperature[point], climate.rain[point], body);
        keep(soil, point, grown_at(making, rivers.wear[point], 0.0, 0.0, body.soil_years, body),
             body.soil_years);
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
        const double till = ice.till[point];
        const double alluvium = sediment.alluvium[point];
        soil.loose[point] = till + alluvium;
        if (ice.thickness[point] > 0.0 || sediment.lake_depth[point] > 0.0 ||
            lakes.salt_flat[point] != 0) {
            continue;
        }
        // Where the coldest times' ice lay, it scraped the older soil away.
        const double years = ice.coldest[point] > 0.0 ? body.ice_left_years : body.soil_years;
        const double making = soil_making(climate.temperature[point], climate.rain[point], body);
        keep(soil, point, grown_at(making, rivers.wear[point], till, alluvium, years, body), years);
    }
    return soil;
}

} // namespace hog::formation

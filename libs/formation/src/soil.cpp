#include <hog/core/portable_maths.hpp>
#include <hog/formation/soil.hpp>

#include <algorithm>
#include <numbers>

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

double plant_growth(double celsius, double rain, const Body& body) {
    const double warmth = 3.0 / (1.0 + maths::exp(1.315 - (0.119 * celsius)));
    const double water = 3.0 * (1.0 - maths::exp(-0.664 * std::max(rain, 0.0)));
    // Plants grow only while the ground is thawed, little in a short summer.
    return body.plant_growth * std::min(warmth, water) * smoothstep(-15.0, 5.0, celsius);
}

double wetness(double celsius, double rain, const Body& body) {
    return std::max(rain, 0.0) / std::max(open_water_loss(celsius, body), 1e-3);
}

double waterlogged(double wetness) {
    return smoothstep(1.0, 2.5, wetness);
}

namespace {

// Per year dead matter rots in air: doubling every 10 °C, stopped by frost, slowed by drought.
double rotting_in_air(double celsius, double wetness, const Body& body) {
    const double thawed = smoothstep(-15.0, 5.0, celsius);
    const double damp = wetness / (wetness + 0.3);
    return body.rotting * maths::exp(std::numbers::ln2 * (celsius - 10.0) / 10.0) * thawed * damp;
}

} // namespace

double rotting_rate(double celsius, double wetness, const Body& body) {
    const double drowned = waterlogged(wetness);
    return rotting_in_air(celsius, wetness, body) *
           ((1.0 - drowned) + (body.drowned_rotting * drowned));
}

double gathered(double gain, double rate, double years) {
    const double time = std::max(years, 0.0);
    if (rate * time < 1e-9) {
        return gain * time;
    }
    return gain / rate * (1.0 - maths::exp(-rate * time));
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
    soil.topsoil.assign(size, 0.0);
    soil.organic.assign(size, 0.0);
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

// What plants leave over `years`: humus from their dying roots
// darkening the soil's top, and what dies above ground lying on it as litter rotting in the air;
// where the ground is waterlogged, a share sinks under the water first and lasts as peat (Clymo's
// two layers). Both are worn away as the ground wears.
void rot(Soil& soil, std::uint32_t point, double celsius, double rain, double wear, double years,
         const Body& body) {
    const double wet = wetness(celsius, rain, body);
    const double growing = plant_growth(celsius, rain, body);
    const double humus = gathered(growing * body.root_share * body.humus_share,
                                  rotting_rate(celsius, wet, body), years);
    soil.topsoil[point] = std::min(soil.depth[point], humus / body.topsoil_humus);
    const double in_air = rotting_in_air(celsius, wet, body);
    const double falling = growing * (1.0 - body.root_share) / body.peat_density;
    const double litter =
        gathered(std::max(falling - wear, 0.0), in_air * body.litter_rotting, years);
    const double sinking = falling * waterlogged(wet) * body.peat_burial;
    const double peat =
        gathered(std::max(sinking - wear, 0.0), in_air * body.drowned_rotting, years);
    soil.organic[point] = litter + peat;
}

} // namespace

Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate, const Body& body) {
    Soil soil = bare(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] == point) {
            continue; // under the liquid
        }
        const double celsius = climate.temperature[point];
        const double rain = climate.rain[point];
        grow(soil, point, {}, soil_making(celsius, rain, body), rivers.wear[point], body.soil_years,
             body);
        rot(soil, point, celsius, rain, rivers.wear[point], body.soil_years, body);
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
        const double celsius = climate.temperature[point];
        const double rain = climate.rain[point];
        // Ground gathering what's laid on it isn't wearing away.
        const double wear = laid.water + laid.till > 0.0 ? 0.0 : rivers.wear[point];
        grow(soil, point, laid, soil_making(celsius, rain, body), rivers.wear[point], years, body);
        rot(soil, point, celsius, rain, wear, years, body);
    }
    return soil;
}

Profile profile_at(const Sphere& sphere, const Soil& soil, const Sphere::Spot& spot) {
    const double topsoil = sphere.sample(soil.topsoil, spot);
    return {.organic = sphere.sample(soil.organic, spot),
            .topsoil = topsoil,
            .subsoil = std::max(0.0, sphere.sample(soil.depth, spot) - topsoil),
            .alluvium = sphere.sample(soil.alluvium, spot),
            .lake_beds = sphere.sample(soil.lake_beds, spot),
            .till = sphere.sample(soil.till, spot)};
}

Layer layer_at(const Profile& profile, double depth) {
    double bottom = profile.organic;
    if (depth < bottom) {
        return Layer::Organic;
    }
    bottom += profile.topsoil;
    if (depth < bottom) {
        return Layer::Topsoil;
    }
    bottom += profile.subsoil;
    if (depth < bottom) {
        return Layer::Subsoil;
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

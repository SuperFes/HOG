#include <hog/formation/soil.hpp>

#include <hog/core/portable_maths.hpp>

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

Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate, const Body& body) {
    Soil soil;
    soil.depth.assign(sphere.size(), 0.0);
    soil.making.assign(sphere.size(), 0.0);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] == point) {
            continue; // under the liquid
        }
        soil.making[point] = soil_making(climate.temperature[point], climate.rain[point], body);
        soil.depth[point] = balanced_depth(soil.making[point], rivers.wear[point], body);
    }
    return soil;
}

} // namespace hog::formation

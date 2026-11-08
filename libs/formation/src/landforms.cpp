#include <hog/core/portable_maths.hpp>
#include <hog/formation/landforms.hpp>
#include <hog/formation/soil.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::formation {

namespace {

constexpr double earth_radius = 6'371'000.0;

double angle(const DVec3& a, const DVec3& b) noexcept {
    return maths::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

double smoothstep(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

double bell(double x) noexcept {
    return maths::exp(-(x * x));
}

bool in_sea(const Rivers& rivers, std::uint32_t point) {
    return rivers.receiver[point] == point;
}

// The rock's top, under any ice lying on it.
double surface(const Rivers& rivers, const Ice& ice, std::uint32_t point) {
    return in_sea(rivers, point) ? 0.0 : rivers.height[point] - ice.thickness[point];
}

struct Around {
    bool coast = false;
    double slope = 0.0;
};

// Whether the sea is next to a point, and the steepest rise or fall of rock to a neighbour.
Around around(const Sphere& sphere, const Rivers& rivers, const Ice& ice, std::uint32_t point) {
    Around here;
    for (const std::uint32_t next : sphere.neighbors(point)) {
        here.coast = here.coast || in_sea(rivers, next);
        const double run = angle(sphere.point(point), sphere.point(next)) * earth_radius;
        here.slope = std::max(
            here.slope, std::abs(surface(rivers, ice, point) - surface(rivers, ice, next)) / run);
    }
    return here;
}

struct Faulting {
    double broken = 0.0; // how likely the ground is broken into scarps
    double active = 0.0; // whether lava still comes up there
};

// What a plate boundary `near` belt widths away does to the ground: rifts break it in steps and
// leak lava, transforms tear it, collisions thrust it up at the belt's front, and sinking or
// spreading sea floor feeds volcanoes.
Faulting faulting(Meeting meeting, double near) {
    switch (meeting) {
    case Meeting::Rift:
        return {.broken = bell(near / 0.7), .active = 1.0};
    case Meeting::Transform:
        return {.broken = 0.6 * bell(near / 0.4), .active = 0.0};
    case Meeting::Collision:
        return {.broken = 0.4 * bell((near - 0.8) / 0.3), .active = 0.0};
    case Meeting::Subduction:
    case Meeting::IslandArc:
    case Meeting::Ridge:
        return {.broken = 0.0, .active = 1.0};
    case Meeting::None:
        break;
    }
    return {};
}

// How well the top of the rock caps what's under it: hard beds in layered rock, lava wholly.
double cap_of(Rock rock, double layering) {
    if (rock == Rock::Layered) {
        return layering;
    }
    return rock == Rock::Lava ? 1.0 : 0.0;
}

} // namespace

Landforms landforms_on(const Sphere& sphere, const Crust& crust, const Bedrock& bedrock,
                       const Outcrop& outcrop, const Climate& climate, const Rivers& rivers,
                       const Ice& ice, const Body& body) {
    const std::uint32_t size = sphere.size();
    Landforms forms;
    for (auto* field : {&forms.canyons, &forms.mesas, &forms.escarpments, &forms.badlands,
                        &forms.karst, &forms.sea_cliffs, &forms.glacial, &forms.fault_scarps,
                        &forms.volcanoes, &forms.scree, &forms.strength, &forms.slope}) {
        field->assign(size, 0.0);
    }
    const double belt = std::max(body.belt_width, 1e-6);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (in_sea(rivers, point)) {
            continue;
        }
        const auto [coast, slope] = around(sphere, rivers, ice, point);
        const Rock rock = outcrop.rock[point];
        const double layering = bedrock.layering[point];
        const double soluble = bedrock.soluble[point];
        const double strength = rock_strength(rock, layering, soluble);
        const double celsius = climate.temperature[point];
        const double wet = wetness(celsius, climate.rain[point], body);
        // Drought keeps faces sharp; rain rounds them under soil and plants.
        const double dry = smoothstep(1.0, 0.3, wet);
        const double steep = smoothstep(0.002, 0.03, slope);
        const double cutting = smoothstep(1.0e-5, 2.0e-4, rivers.wear[point]);
        const double river = smoothstep(
            8.0, 10.5, maths::log(std::max(rivers.discharge[point], 1.0)) / std::numbers::ln10);
        // Beds lifted high enough for rivers to cut them into tables, with some cap still left.
        const bool capped = outcrop.cover[point] > 0.0 || outcrop.lava[point] > 0.0;
        const double raised = smoothstep(200.0, 1'200.0, rivers.height[point]);
        const double dissected = capped ? raised : 0.0;
        const double flat = 1.0 - smoothstep(0.2, 0.5, bedrock.folding[point]);
        const bool beds = rock == Rock::Layered || rock == Rock::Folded;

        forms.strength[point] = strength;
        forms.slope[point] = slope;
        forms.glacial[point] = smoothstep(20.0, 500.0, ice.carved[point]);
        if (ice.thickness[point] > 0.0) {
            continue; // the rest lies hidden under the ice
        }
        forms.canyons[point] = river * cutting * strength * (0.4 + (0.6 * dry));
        forms.mesas[point] = flat * cap_of(rock, layering) * dissected * dry;
        // Steps in hard beds show at gentler slopes than whole mountainsides.
        forms.escarpments[point] =
            rock == Rock::Layered ? layering * flat * smoothstep(0.001, 0.01, slope) : 0.0;
        forms.badlands[point] = rock == Rock::Layered ? (1.0 - layering) * (1.0 - soluble) * dry *
                                                            std::max(cutting, raised)
                                                      : 0.0;
        forms.karst[point] =
            beds ? soluble * smoothstep(0.3, 1.5, wet) * smoothstep(-5.0, 20.0, celsius) : 0.0;
        forms.sea_cliffs[point] =
            coast ? strength * smoothstep(0.0, 300.0, rivers.height[point]) : 0.0;

        const double near = crust.boundary[point] / belt;
        const auto [broken, active] = faulting(crust.meeting[point], near);
        forms.fault_scarps[point] = broken;
        forms.volcanoes[point] =
            near < 1.5 ? active * smoothstep(0.0, 0.5 * body.arc_lava, bedrock.lava[point]) : 0.0;
        // Water freezing in cracks shatters rock fastest a few degrees under freezing.
        const double frost = bell((celsius + 5.0) / 6.0);
        forms.scree[point] = frost * steep * strength;
    }
    return forms;
}

} // namespace hog::formation

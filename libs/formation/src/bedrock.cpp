#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>
#include <hog/formation/bedrock.hpp>
#include <hog/formation/fields.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::formation {

namespace {

double unit(std::uint64_t bits) noexcept {
    return static_cast<double>(bits >> 11U) * 0x1.0p-53;
}

DVec3 random_direction(std::uint64_t bits) noexcept {
    const double z = (2.0 * unit(mix64(bits))) - 1.0;
    const double turn = 2.0 * std::numbers::pi * unit(mix64(bits ^ 0x9E37'79B9'7F4A'7C15ULL));
    const double across = std::sqrt(std::max(0.0, 1.0 - (z * z)));
    return {across * maths::cos(turn), across * maths::sin(turn), z};
}

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

struct Flood {
    DVec3 heart{};
    double reach = 0.0;
};

} // namespace

Bedrock bedrock_on(const Sphere& sphere, const Crust& crust, const Body& body) {
    const std::uint32_t size = sphere.size();
    const int finest = std::max(2, std::min(sphere.subdivisions(), 6));
    // The history that laid the rock isn't simulated; where it laid more or less is seeded.
    const auto laid = sphere_noise(sphere, hash_values(body.seed, 20), 2, finest);
    const auto beds = sphere_noise(sphere, hash_values(body.seed, 21), 2, finest);
    const auto dissolving = sphere_noise(sphere, hash_values(body.seed, 22), 2, finest);
    const auto ranges = sphere_noise(sphere, hash_values(body.seed, 23), 1, std::min(finest, 3));
    std::vector<Flood> floods(static_cast<std::size_t>(std::max(body.lava_floods, 0)));
    for (std::size_t flood = 0; flood < floods.size(); ++flood) {
        floods[flood] = {.heart = random_direction(hash_values(body.seed, 24, flood)),
                         .reach = body.lava_flood_reach *
                                  (0.6 + (0.8 * unit(hash_values(body.seed, 25, flood))))};
    }

    Bedrock bedrock;
    bedrock.cover.assign(size, 0.0);
    bedrock.layering.assign(size, 0.0);
    bedrock.soluble.assign(size, 0.0);
    bedrock.folding.assign(size, 0.0);
    bedrock.lava.assign(size, 0.0);
    const double belt = std::max(body.belt_width, 1e-6);
    for (std::uint32_t point = 0; point < size; ++point) {
        const double light = crust.light[point];
        const double near = crust.boundary[point] / belt;
        const Meeting meeting = crust.meeting[point];

        // Light crust standing low gathered layered rock in its shallow seas and basins; rifts
        // filled as they sagged. The sea floor gathers mud as it ages.
        const double standing = smoothstep(2'500.0, 0.0, crust.base[point]);
        // Some of it sank into platforms and gathered; some stood as shields and gathered none.
        double on_light =
            body.cover_thickness * standing * std::clamp(0.5 + (1.5 * laid[point]), 0.0, 2.0);
        if (meeting == Meeting::Rift) {
            on_light += body.rift_fill * bell(near);
        }
        const double on_floor = body.sea_floor_mud * std::max(crust.floor_age[point], 0.0);
        bedrock.cover[point] = (light * on_light) + ((1.0 - light) * on_floor);
        bedrock.layering[point] = std::clamp(body.layering * (1.0 + beds[point]), 0.0, 1.0);
        bedrock.soluble[point] =
            light * std::clamp(body.soluble_share * (1.0 + dissolving[point]), 0.0, 1.0);

        // Colliding crust folds its layers across the belt; the light crust riding over sinking sea
        // floor folds behind its volcanoes; old ranges worn down left folds of their own.
        double belt_folds = 0.0;
        if (meeting == Meeting::Collision) {
            belt_folds = bell(near);
        } else if (meeting == Meeting::Subduction) {
            belt_folds = 0.7 * light * bell(near / 1.5);
        }
        // Old ranges run in long winding belts: where the seeded field crosses zero.
        const double old = body.old_folds * light * smoothstep(0.08, 0.02, std::abs(ranges[point]));
        bedrock.folding[point] = std::clamp(std::max(belt_folds, old), 0.0, 1.0);

        // Volcanoes stand a little behind a trench; rifts and ridges on land leak lava; great
        // floods of it once spread over the light crust.
        double lava = 0.0;
        if (meeting == Meeting::Subduction || meeting == Meeting::IslandArc) {
            const double over = meeting == Meeting::Subduction ? light : 1.0;
            lava += body.arc_lava * over * bell((near - 0.7) / 0.35);
        } else if (meeting == Meeting::Rift || meeting == Meeting::Ridge) {
            lava += 0.25 * body.arc_lava * light * bell(near / 0.5);
        }
        for (const Flood& flood : floods) {
            const double away = angle(sphere.point(point), flood.heart) / flood.reach;
            if (away < 3.0) {
                lava += body.lava_flood_depth * light * bell(away);
            }
        }
        bedrock.lava[point] = lava;
    }
    return bedrock;
}

namespace {

struct Stripped {
    Rock rock = Rock::Basement;
    double cover = 0.0;
    double lava = 0.0;
};

// Wear takes the lava first, then the layered rock, down to the basement.
Stripped stripped(const Crust& crust, const Bedrock& bedrock, std::uint32_t point, double eroded) {
    const double lava = std::max(0.0, bedrock.lava[point] - eroded);
    const double cover =
        std::max(0.0, bedrock.cover[point] - std::max(0.0, eroded - bedrock.lava[point]));
    Rock rock = crust.light[point] >= 0.5 ? Rock::Basement : Rock::SeaFloor;
    if (lava > 0.0) {
        rock = Rock::Lava;
    } else if (cover > 0.0) {
        rock = bedrock.folding[point] >= folded ? Rock::Folded : Rock::Layered;
    }
    return {.rock = rock, .cover = cover, .lava = lava};
}

} // namespace

Outcrop outcrop_on(const Sphere& sphere, const Crust& crust, const Bedrock& bedrock,
                   const Rivers& rivers, const Body& body) {
    const std::uint32_t size = sphere.size();
    Outcrop outcrop;
    outcrop.rock.assign(size, Rock::Basement);
    outcrop.eroded.assign(size, 0.0);
    outcrop.cover.assign(size, 0.0);
    outcrop.lava.assign(size, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        const double eroded = std::max(rivers.wear[point], 0.0) * std::max(body.exhumed_years, 0.0);
        const Stripped top = stripped(crust, bedrock, point, eroded);
        outcrop.rock[point] = top.rock;
        outcrop.eroded[point] = eroded;
        outcrop.cover[point] = top.cover;
        outcrop.lava[point] = top.lava;
    }
    return outcrop;
}

double rock_strength(Rock rock, double layering, double soluble) {
    switch (rock) {
    case Rock::Basement:
        return 1.0;
    case Rock::Lava:
        return 0.85;
    case Rock::SeaFloor:
        return 0.8;
    case Rock::Folded:
        return 0.6;
    case Rock::Layered:
        break;
    }
    return std::clamp(0.3 + (0.4 * layering) + (0.2 * soluble), 0.0, 1.0);
}

std::vector<double> standing_strength(const Sphere& sphere, const Crust& crust,
                                      const Bedrock& bedrock, const Body& body) {
    std::vector<double> strength(sphere.size(), 0.0);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const double eroded =
            std::max(crust.uplift[point], 0.0) * std::max(body.exhumed_years, 0.0);
        const Stripped top = stripped(crust, bedrock, point, eroded);
        strength[point] = rock_strength(top.rock, bedrock.layering[point], bedrock.soluble[point]);
    }
    return strength;
}

} // namespace hog::formation

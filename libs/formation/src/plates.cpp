#include <hog/formation/plates.hpp>

#include <hog/formation/fields.hpp>

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <queue>
#include <tuple>
#include <utility>

namespace hog::formation {

namespace {

constexpr std::uint16_t unclaimed = 0xFFFF;
// Metres of Earth's surface per radian, for speeds given at Earth's scale.
constexpr double earth_radius = 6'371'000.0;
// Sea floor older than this has sunk back into the mantle.
constexpr double oldest_floor = 200.0;

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

// Tangent directions at `where`, east and north, any way round at the poles.
std::pair<DVec3, DVec3> tangents(const DVec3& where) noexcept {
    const DVec3 across = std::abs(where.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{1, 0, 0};
    const DVec3 east = glm::normalize(glm::cross(across, where));
    return {east, glm::cross(where, east)};
}

std::uint32_t pair_key(std::uint16_t a, std::uint16_t b) noexcept {
    return (std::uint32_t{std::min(a, b)} << 16U) | std::max(a, b);
}

// Regions grown out from their seed points together, each claiming ground at its own pace and
// slower where `roughness` is high, until `area` steradians are claimed (all of it by default).
std::vector<std::uint16_t> grow(const Sphere& sphere, std::span<const std::uint32_t> seeds,
                                std::span<const double> pace, std::span<const double> roughness,
                                double area = std::numeric_limits<double>::infinity()) {
    std::vector<std::uint16_t> owner(sphere.size(), unclaimed);
    std::vector<double> cost(sphere.size(), std::numeric_limits<double>::infinity());
    using Entry = std::tuple<double, std::uint32_t, std::uint16_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    for (std::size_t region = 0; region < seeds.size(); ++region) {
        open.emplace(0.0, seeds[region], static_cast<std::uint16_t>(region));
    }
    double claimed = 0.0;
    while (!open.empty() && claimed < area) {
        const auto [spent, point, region] = open.top();
        open.pop();
        if (owner[point] != unclaimed) {
            continue;
        }
        owner[point] = region;
        claimed += sphere.area(point);
        for (const std::uint32_t next : sphere.neighbors(point)) {
            if (owner[next] != unclaimed) {
                continue;
            }
            const double step = angle(sphere.point(point), sphere.point(next)) * pace[region] *
                                std::max(0.2, 1.0 + (0.8 * roughness[next]));
            if (spent + step < cost[next]) {
                cost[next] = spent + step;
                open.emplace(spent + step, next, region);
            }
        }
    }
    return owner;
}

} // namespace

Plates::Plates(const Body& body, const Sphere& layout)
    : layout_{&layout}, seed_{body.seed}, boundary_age_{body.boundary_age} {
    const auto count = static_cast<std::size_t>(std::clamp(body.plates, 1, 1000));
    std::vector<std::uint32_t> seeds(count);
    std::vector<double> pace(count);
    spins_.resize(count);
    const double speed = body.plate_speed * std::numbers::pi / 180.0;
    for (std::size_t plate = 0; plate < count; ++plate) {
        seeds[plate] = layout.nearest(random_direction(hash_values(seed_, 1, plate)));
        pace[plate] = 0.5 + (1.2 * unit(hash_values(seed_, 2, plate)));
        spins_[plate] = random_direction(hash_values(seed_, 3, plate)) * speed *
                        (0.3 + (1.4 * unit(hash_values(seed_, 4, plate))));
    }
    plates_ = grow(layout, seeds, pace, sphere_noise(layout, hash_values(seed_, 5), 2, 5));

    const auto pieces = static_cast<std::size_t>(std::clamp(body.continents, 1, 1000));
    std::vector<std::uint32_t> cores(pieces);
    std::vector<double> spread(pieces);
    for (std::size_t piece = 0; piece < pieces; ++piece) {
        cores[piece] = layout.nearest(random_direction(hash_values(seed_, 6, piece)));
        spread[piece] = 0.6 + (0.8 * unit(hash_values(seed_, 7, piece)));
    }
    const double area = std::clamp(body.land_share, 0.0, 1.0) * 4.0 * std::numbers::pi;
    const std::vector<std::uint16_t> land =
        grow(layout, cores, spread, sphere_noise(layout, hash_values(seed_, 8), 3, 6), area);
    light_.resize(layout.size());
    for (std::uint32_t point = 0; point < layout.size(); ++point) {
        light_[point] = land[point] != unclaimed ? 1 : 0;
    }

    // Each stretch of boundary between neighbouring points on different plates. Which way it
    // runs, and which side is light, are taken over a reach of the same boundary, so the kind of
    // meeting holds along it rather than flickering with the layout's steps.
    struct Raw {
        std::uint32_t from;
        std::uint32_t to;
        DVec3 middle;
        DVec3 across; // from `from`'s plate towards `to`'s
    };
    std::vector<Raw> raws;
    std::map<std::uint32_t, std::vector<std::size_t>> by_pair;
    for (std::uint32_t point = 0; point < layout.size(); ++point) {
        for (const std::uint32_t other : layout.neighbors(point)) {
            if (other < point || plates_[point] == plates_[other]) {
                continue;
            }
            const DVec3 middle = glm::normalize(layout.point(point) + layout.point(other));
            const DVec3 step = layout.point(other) - layout.point(point);
            by_pair[pair_key(plates_[point], plates_[other])].push_back(raws.size());
            raws.push_back({.from = point,
                            .to = other,
                            .middle = middle,
                            .across = glm::normalize(step - (glm::dot(step, middle) * middle))});
        }
    }
    const double reach = 2.5 * layout.spacing();
    boundaries_.reserve(raws.size());
    for (const auto& [key, members] : by_pair) {
        for (const std::size_t index : members) {
            const Raw& raw = raws[index];
            const std::uint16_t a = plates_[raw.from];
            const std::uint16_t b = plates_[raw.to];
            DVec3 across{0.0};
            double light_a = 0.0;
            double light_b = 0.0;
            for (const std::size_t near : members) {
                const Raw& other = raws[near];
                if (angle(raw.middle, other.middle) > reach) {
                    continue;
                }
                const bool same_way = plates_[other.from] == a;
                across += same_way ? other.across : -other.across;
                light_a += light_[same_way ? other.from : other.to];
                light_b += light_[same_way ? other.to : other.from];
            }
            across -= glm::dot(across, raw.middle) * raw.middle;
            across = glm::length(across) > 0.0 ? glm::normalize(across) : raw.across;

            const DVec3 relative = velocity(b, raw.middle) - velocity(a, raw.middle);
            const double closing = -glm::dot(relative, across);
            const double total = glm::length(relative);
            const bool lit_a = light_a > light_b || (light_a == light_b && light_a > 0.0);
            const bool lit_b = light_b > light_a || (light_a == light_b && light_b > 0.0);
            const bool both_light = std::min(light_a, light_b) > 0.0;
            const double boundary_age = age(a, b);

            Boundary boundary{
                .where = raw.middle, .a = a, .b = b, .closing = closing, .width = body.belt_width};
            if (total <= 0.0 || std::abs(closing) < 0.35 * total) {
                boundary.meeting = Meeting::Transform;
            } else if (closing > 0.0) {
                const double shortening = closing * boundary_age;
                if (both_light) {
                    // Crust thickens over the belt's width; past twice its own thickness it
                    // spreads sideways instead, widening the belt into a plateau.
                    boundary.meeting = Meeting::Collision;
                    const double piled = body.thickening * shortening / std::sqrt(std::numbers::pi);
                    boundary.width = std::max(body.belt_width, piled);
                    boundary.thickened = piled * body.crust_thickness / boundary.width;
                } else if (lit_a || lit_b) {
                    boundary.meeting = Meeting::Subduction;
                    boundary.over = lit_a ? a : b;
                    const double piled =
                        0.5 * body.thickening * shortening / std::sqrt(std::numbers::pi);
                    boundary.thickened = std::min(piled * body.crust_thickness / body.belt_width,
                                                  0.6 * body.crust_thickness);
                } else {
                    boundary.meeting = Meeting::IslandArc;
                    boundary.over = rides_over(a, b) ? a : b;
                }
            } else {
                boundary.meeting = (lit_a || lit_b) ? Meeting::Rift : Meeting::Ridge;
            }
            boundaries_.push_back(boundary);
        }
    }
}

double Plates::age(std::uint16_t a, std::uint16_t b) const {
    const auto bits = hash_values(seed_, 9, std::min(a, b), std::max(a, b));
    return boundary_age_ * (0.25 + (1.5 * unit(bits)));
}

bool Plates::rides_over(std::uint16_t a, std::uint16_t b) const {
    const std::uint64_t mine = hash_values(seed_, 10, a);
    const std::uint64_t theirs = hash_values(seed_, 10, b);
    return mine != theirs ? mine > theirs : a < b;
}

Crust crust_on(const Sphere& sphere, const Plates& plates, const Body& body) {
    const Sphere& layout = plates.layout();
    const std::uint32_t size = sphere.size();
    Crust crust;
    crust.plate.resize(size);
    crust.light.resize(size);
    crust.base.resize(size);
    crust.uplift.resize(size);
    crust.meeting.resize(size, Meeting::None);
    crust.boundary.resize(size);
    crust.floor_age.resize(size);

    // Plates and light crust read off the layout through a warp, so their edges wander rather
    // than follow the layout's triangles; coasts get finer wiggles than the layout holds.
    std::vector<double> light_at_layout(layout.size());
    for (std::uint32_t point = 0; point < layout.size(); ++point) {
        light_at_layout[point] = plates.light(point) ? 1.0 : 0.0;
    }
    const int coarse = layout.subdivisions() + 1;
    const int fine = std::max(coarse, sphere.subdivisions());
    const std::vector<double> warp_east =
        sphere_noise(sphere, hash_values(body.seed, 12), coarse, coarse + 2);
    const std::vector<double> warp_north =
        sphere_noise(sphere, hash_values(body.seed, 13), coarse, coarse + 2);
    const std::vector<double> wiggle = sphere_noise(sphere, hash_values(body.seed, 11), coarse, fine);
    const double warp = 1.5 * layout.spacing();
    std::uint32_t start = 0;
    for (std::uint32_t point = 0; point < size; ++point) {
        const DVec3& here = sphere.point(point);
        const auto [east, north] = tangents(here);
        const DVec3 warped = glm::normalize(
            here + (warp * ((warp_east[point] * east) + (warp_north[point] * north))));
        const Sphere::Spot spot = layout.locate(warped, start);
        start = spot.corners[0];
        std::uint16_t best = plates.plate(spot.corners[0]);
        double best_weight = -1.0;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const std::uint16_t plate = plates.plate(spot.corners[corner]);
            double weight = 0.0;
            for (std::size_t other = 0; other < 3; ++other) {
                weight += plates.plate(spot.corners[other]) == plate
                              ? spot.weights[static_cast<glm::length_t>(other)]
                              : 0.0;
            }
            if (weight > best_weight) {
                best_weight = weight;
                best = plate;
            }
        }
        crust.plate[point] = best;
        crust.light[point] =
            smoothstep(0.35, 0.65, layout.sample(light_at_layout, spot) + (0.3 * wiggle[point]));
    }

    // Each fine stretch of boundary takes after the nearest of the layout's between the same
    // two plates; distances are kept from the fine ones.
    const std::span<const Boundary> boundaries = plates.boundaries();
    std::vector<Source> coarse_sources;
    std::map<std::uint32_t, std::vector<std::uint32_t>> by_pair;
    start = 0;
    for (std::uint32_t index = 0; index < boundaries.size(); ++index) {
        start = sphere.nearest(boundaries[index].where, start);
        coarse_sources.push_back({.where = boundaries[index].where, .point = start});
        by_pair[pair_key(boundaries[index].a, boundaries[index].b)].push_back(index);
    }
    const Nearest coarse_near = nearest_sources(sphere, coarse_sources);
    const Boundary sliding{.meeting = Meeting::Transform};

    std::vector<Source> sources;
    std::vector<const Boundary*> kinds;
    std::vector<Source> ridges;
    std::vector<double> ridge_opening;
    for (std::uint32_t point = 0; point < size; ++point) {
        for (const std::uint32_t other : sphere.neighbors(point)) {
            const std::uint16_t a = crust.plate[point];
            const std::uint16_t b = crust.plate[other];
            if (other < point || a == b) {
                continue;
            }
            const DVec3 middle = glm::normalize(sphere.point(point) + sphere.point(other));
            const Boundary* kind = &sliding;
            const std::uint32_t near = coarse_near.source[point];
            if (near != Nearest::none && pair_key(boundaries[near].a, boundaries[near].b) ==
                                             pair_key(a, b)) {
                kind = &boundaries[near];
            } else if (const auto found = by_pair.find(pair_key(a, b)); found != by_pair.end()) {
                double closest = std::numeric_limits<double>::infinity();
                for (const std::uint32_t index : found->second) {
                    const double distance = angle(middle, boundaries[index].where);
                    if (distance < closest) {
                        closest = distance;
                        kind = &boundaries[index];
                    }
                }
            }
            sources.push_back({.where = middle, .point = point});
            kinds.push_back(kind);
            if (kind->meeting == Meeting::Ridge) {
                ridges.push_back({.where = middle, .point = point});
                ridge_opening.push_back(-kind->closing);
            }
        }
    }

    const Nearest nearest = nearest_sources(sphere, sources);
    const Nearest ridge = nearest_sources(sphere, ridges);
    const double to_metres_a_year = earth_radius / 1.0e6;
    for (std::uint32_t point = 0; point < size; ++point) {
        const double light = crust.light[point];
        double age = oldest_floor;
        if (ridge.source[point] != Nearest::none) {
            const double half_rate = 0.5 * ridge_opening[ridge.source[point]];
            age = std::min(oldest_floor, ridge.distance[point] / std::max(half_rate, 1e-9));
        }
        crust.floor_age[point] = age;
        const double floor =
            std::max(body.abyss_depth, body.ridge_depth - (body.floor_sinking * std::sqrt(age)));
        double base = (light * body.continent_height) + ((1.0 - light) * floor);
        double uplift = light * 1.0e-5;

        if (nearest.source[point] != Nearest::none) {
            const Boundary& kind = *kinds[nearest.source[point]];
            const double distance = nearest.distance[point];
            const double x = distance / body.belt_width;
            const double closing = kind.closing * to_metres_a_year;
            const bool riding = kind.over == crust.plate[point];
            crust.boundary[point] = distance;
            crust.meeting[point] = x < 3.0 ? kind.meeting : Meeting::None;
            switch (kind.meeting) {
            case Meeting::Collision: {
                const double profile = bell(distance / kind.width);
                base += light * body.crust_rise * kind.thickened * profile;
                uplift += body.uplift_share * closing * profile;
                break;
            }
            case Meeting::Subduction:
                if (riding) {
                    base += light * ((body.arc_height * bell((x - 0.6) / 0.3)) +
                                     (body.crust_rise * kind.thickened * bell(x / 1.2)));
                    uplift += body.uplift_share * closing * bell((x - 0.5) / 0.6);
                } else {
                    base -= body.trench_depth * bell(x / 0.25);
                }
                break;
            case Meeting::IslandArc:
                if (riding) {
                    const double profile = bell((x - 0.5) / 0.25);
                    base += (std::abs(floor) + (0.5 * body.arc_height)) * profile;
                    uplift += 0.5 * body.uplift_share * closing * profile;
                } else {
                    base -= body.trench_depth * bell(x / 0.25);
                }
                break;
            case Meeting::Rift:
                base -= light * body.rift_depth * bell(x / 0.4);
                break;
            case Meeting::None:
            case Meeting::Ridge:
            case Meeting::Transform:
                break;
            }
        }
        crust.base[point] = base;
        crust.uplift[point] = uplift;
    }
    return crust;
}

} // namespace hog::formation

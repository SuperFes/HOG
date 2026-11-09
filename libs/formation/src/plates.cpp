#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>
#include <hog/formation/fields.hpp>
#include <hog/formation/plates.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <queue>
#include <span>
#include <tuple>
#include <utility>

namespace hog::formation {

namespace {

constexpr std::uint16_t unclaimed = 0xFFFF;
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

// One step of boundary on the layout, between neighbouring points on different plates.
struct Step {
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    DVec3 middle{};
    DVec3 across{}; // from `from`'s plate towards `to`'s
};

struct Steps {
    std::vector<Step> steps;
    std::map<std::uint32_t, std::vector<std::size_t>> by_pair;
};

Steps boundary_steps(const Plates& plates) {
    const Sphere& layout = plates.layout();
    Steps found;
    for (std::uint32_t point = 0; point < layout.size(); ++point) {
        for (const std::uint32_t other : layout.neighbors(point)) {
            if (other < point || plates.plate(point) == plates.plate(other)) {
                continue;
            }
            const DVec3 middle = glm::normalize(layout.point(point) + layout.point(other));
            const DVec3 step = layout.point(other) - layout.point(point);
            found.by_pair[pair_key(plates.plate(point), plates.plate(other))].push_back(
                found.steps.size());
            found.steps.push_back(
                {.from = point,
                 .to = other,
                 .middle = middle,
                 .across = glm::normalize(step - (glm::dot(step, middle) * middle))});
        }
    }
    return found;
}

// Which way a stretch of boundary runs and how much light crust lies either side, taken over a
// reach of the same boundary, so the kind of meeting holds along it rather than flickering with
// the layout's steps.
struct Sides {
    DVec3 across{};
    double light_a = 0.0;
    double light_b = 0.0;
};

Sides sides_of(const Plates& plates, const Steps& found, std::span<const std::size_t> members,
               const Step& step) {
    const double reach = 2.5 * plates.layout().spacing();
    const std::uint16_t a = plates.plate(step.from);
    Sides sides;
    for (const std::size_t near : members) {
        const Step& other = found.steps[near];
        if (angle(step.middle, other.middle) > reach) {
            continue;
        }
        const bool same_way = plates.plate(other.from) == a;
        sides.across += same_way ? other.across : -other.across;
        sides.light_a += plates.light(same_way ? other.from : other.to) ? 1.0 : 0.0;
        sides.light_b += plates.light(same_way ? other.to : other.from) ? 1.0 : 0.0;
    }
    sides.across -= glm::dot(sides.across, step.middle) * step.middle;
    sides.across = glm::length(sides.across) > 0.0 ? glm::normalize(sides.across) : step.across;
    return sides;
}

Boundary classify(const Plates& plates, const Body& body, const Step& step, const Sides& sides) {
    const std::uint16_t a = plates.plate(step.from);
    const std::uint16_t b = plates.plate(step.to);
    const DVec3 relative = plates.velocity(b, step.middle) - plates.velocity(a, step.middle);
    const double closing = -glm::dot(relative, sides.across);
    const double total = glm::length(relative);
    const bool even = sides.light_a == sides.light_b;
    const bool lit_a = sides.light_a > sides.light_b || (even && sides.light_a > 0.0);
    const bool lit_b = sides.light_b > sides.light_a || (even && sides.light_b > 0.0);

    Boundary boundary{
        .where = step.middle, .a = a, .b = b, .closing = closing, .width = belt_width(body)};
    if (total <= 0.0 || std::abs(closing) < 0.35 * total) {
        boundary.meeting = Meeting::Transform;
        return boundary;
    }
    if (closing < 0.0) {
        boundary.meeting = (lit_a || lit_b) ? Meeting::Rift : Meeting::Ridge;
        return boundary;
    }
    const double shortening = closing * plates.age(a, b);
    if (std::min(sides.light_a, sides.light_b) > 0.0) {
        // Crust thickens over the belt's width; past twice its own thickness it spreads sideways
        // instead, widening the belt into a plateau.
        boundary.meeting = Meeting::Collision;
        const double piled = body.thickening * shortening / std::sqrt(std::numbers::pi);
        boundary.width = std::max(belt_width(body), piled);
        boundary.thickened = piled * body.crust_thickness / boundary.width;
    } else if (lit_a || lit_b) {
        boundary.meeting = Meeting::Subduction;
        boundary.over = lit_a ? a : b;
        const double piled = 0.5 * body.thickening * shortening / std::sqrt(std::numbers::pi);
        boundary.thickened =
            std::min(piled * body.crust_thickness / belt_width(body), 0.6 * body.crust_thickness);
    } else {
        boundary.meeting = Meeting::IslandArc;
        boundary.over = plates.rides_over(a, b) ? a : b;
    }
    return boundary;
}

std::vector<Boundary> find_boundaries(const Plates& plates, const Body& body) {
    const Steps found = boundary_steps(plates);
    std::vector<Boundary> boundaries;
    boundaries.reserve(found.steps.size());
    for (const auto& [key, members] : found.by_pair) {
        for (const std::size_t index : members) {
            const Step& step = found.steps[index];
            boundaries.push_back(
                classify(plates, body, step, sides_of(plates, found, members, step)));
        }
    }
    return boundaries;
}

// Of a triangle's corners, the plate holding the most weight.
std::uint16_t plate_most_held(const Plates& plates, const Sphere::Spot& spot) {
    std::uint16_t best = plates.plate(spot.corners[0]);
    double best_weight = -1.0;
    for (std::size_t corner = 0; corner < 3; ++corner) {
        const std::uint16_t plate = plates.plate(spot.corners[corner]);
        double weight = 0.0;
        for (std::size_t other = 0; other < 3; ++other) {
            if (plates.plate(spot.corners[other]) == plate) {
                weight += spot.weights[static_cast<glm::length_t>(other)];
            }
        }
        if (weight > best_weight) {
            best_weight = weight;
            best = plate;
        }
    }
    return best;
}

// Plates and light crust read off the layout through a warp, so their edges wander rather than
// follow the layout's triangles; coasts get finer wiggles than the layout holds.
void read_layout(const Sphere& sphere, const Plates& plates, const Body& body, Crust& crust) {
    const Sphere& layout = plates.layout();
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
    const std::vector<double> wiggle =
        sphere_noise(sphere, hash_values(body.seed, 11), coarse, fine);
    const double warp = 1.5 * layout.spacing();
    std::vector<double> lightness(sphere.size());
    std::uint32_t start = 0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const DVec3& here = sphere.point(point);
        const auto [east, north] = tangents(here);
        const DVec3 warped = glm::normalize(
            here + (warp * ((warp_east[point] * east) + (warp_north[point] * north))));
        const Sphere::Spot spot = layout.locate(warped, start);
        start = spot.corners[0];
        crust.plate[point] = plate_most_held(plates, spot);
        lightness[point] = layout.sample(light_at_layout, spot) + (0.3 * wiggle[point]) - 0.5;
    }
    // Light crust thins to the sea floor across its margin, as wide as the lid bends.
    std::vector<Source> edge;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        for (const std::uint32_t other : sphere.neighbors(point)) {
            const double a = lightness[point];
            const double b = lightness[other];
            if (other < point || (a > 0.0) == (b > 0.0)) {
                continue;
            }
            const double t = a / (a - b);
            edge.push_back(
                {.where = glm::normalize(sphere.point(point) +
                                         (t * (sphere.point(other) - sphere.point(point)))),
                 .point = point});
        }
    }
    const Nearest from_edge = nearest_sources(sphere, edge);
    const double half = 0.5 * belt_width(body);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const double inside = lightness[point] > 0.0 ? 1.0 : -1.0;
        crust.light[point] = from_edge.source[point] == Nearest::none
                                 ? (inside > 0.0 ? 1.0 : 0.0)
                                 : smoothstep(-half, half, inside * from_edge.distance[point]);
    }
}

// Finds, for a fine stretch of boundary, the nearest of the layout's between the same two plates.
class KindFinder {
public:
    KindFinder(const Sphere& sphere, std::span<const Boundary> boundaries)
        : boundaries_{boundaries} {
        std::vector<Source> sources;
        std::uint32_t start = 0;
        for (std::uint32_t index = 0; index < boundaries.size(); ++index) {
            start = sphere.nearest(boundaries[index].where, start);
            sources.push_back({.where = boundaries[index].where, .point = start});
            by_pair_[pair_key(boundaries[index].a, boundaries[index].b)].push_back(index);
        }
        near_ = nearest_sources(sphere, sources);
    }

    [[nodiscard]] const Boundary* find(std::uint32_t point, const DVec3& middle, std::uint16_t a,
                                       std::uint16_t b) const {
        const std::uint32_t near = near_.source[point];
        if (near != Nearest::none &&
            pair_key(boundaries_[near].a, boundaries_[near].b) == pair_key(a, b)) {
            return &boundaries_[near];
        }
        const auto found = by_pair_.find(pair_key(a, b));
        if (found == by_pair_.end()) {
            return &sliding_;
        }
        const Boundary* closest = &sliding_;
        double distance = std::numeric_limits<double>::infinity();
        for (const std::uint32_t index : found->second) {
            const double here = angle(middle, boundaries_[index].where);
            if (here < distance) {
                distance = here;
                closest = &boundaries_[index];
            }
        }
        return closest;
    }

private:
    std::span<const Boundary> boundaries_;
    std::map<std::uint32_t, std::vector<std::uint32_t>> by_pair_;
    Nearest near_;
    Boundary sliding_{.meeting = Meeting::Transform};
};

// Fine stretches of boundary, each taking after the nearest of the layout's between the same two
// plates; distances are kept from the fine ones.
struct FineBoundaries {
    std::vector<Source> sources;
    std::vector<const Boundary*> kinds;
    std::vector<Source> ridges;
    std::vector<double> opening; // radians a million years each ridge pulls apart
};

FineBoundaries fine_boundaries(const Sphere& sphere, const KindFinder& finder, const Crust& crust) {
    FineBoundaries fine;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        for (const std::uint32_t other : sphere.neighbors(point)) {
            const std::uint16_t a = crust.plate[point];
            const std::uint16_t b = crust.plate[other];
            if (other < point || a == b) {
                continue;
            }
            const DVec3 middle = glm::normalize(sphere.point(point) + sphere.point(other));
            const Boundary* kind = finder.find(point, middle, a, b);
            fine.sources.push_back({.where = middle, .point = point});
            fine.kinds.push_back(kind);
            if (kind->meeting == Meeting::Ridge) {
                fine.ridges.push_back({.where = middle, .point = point});
                fine.opening.push_back(-kind->closing);
            }
        }
    }
    return fine;
}

// What a boundary does to the crust at a point near it: metres added to how it stands, and
// metres a year of rock pushed up.
struct Shaped {
    double base = 0.0;
    double uplift = 0.0;
};

Shaped shaped_by(const Boundary& kind, double distance, double light, bool riding, double floor,
                 const Body& body) {
    const double x = distance / belt_width(body);
    const double closing = kind.closing * body.radius / 1.0e6;
    switch (kind.meeting) {
    case Meeting::Collision:
        // Thickened crust spreads over the whole widened belt; rock is pushed up hardest at the
        // active front.
        return {.base = light * body.crust_rise * kind.thickened * bell(distance / kind.width),
                .uplift = body.uplift_share * closing * bell(x)};
    case Meeting::Subduction:
        if (riding) {
            return {.base = light * ((body.arc_height * bell((x - 0.6) / 0.3)) +
                                     (body.crust_rise * kind.thickened * bell(x / 1.2))),
                    .uplift = body.uplift_share * closing * bell((x - 0.5) / 0.6)};
        }
        return {.base = -body.trench_depth * bell(x / 0.25)};
    case Meeting::IslandArc:
        if (riding) {
            const double profile = bell((x - 0.5) / 0.25);
            return {.base = (std::abs(floor) + (0.5 * body.arc_height)) * profile,
                    .uplift = 0.5 * body.uplift_share * closing * profile};
        }
        return {.base = -body.trench_depth * bell(x / 0.25)};
    case Meeting::Rift:
        return {.base = -light * body.rift_depth * bell(x / 0.4)};
    case Meeting::None:
    case Meeting::Ridge:
    case Meeting::Transform:
        break;
    }
    return {};
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
    boundaries_ = find_boundaries(*this, body);
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

double bending_reach(const Body& body) {
    constexpr double poisson = 0.25;
    const double lifting = std::max(body.mantle_density - body.liquid_density, 1.0) * body.gravity;
    const double thick = std::max(body.lid_thickness, 0.0);
    return std::sqrt(std::sqrt(body.lid_stiffness * thick * thick * thick /
                               (3.0 * (1.0 - (poisson * poisson)) * lifting)));
}

double belt_width(const Body& body) {
    return std::numbers::pi * bending_reach(body) / std::max(body.radius, 1.0);
}

Crust crust_on(const Sphere& sphere, const Plates& plates, const Body& body) {
    const std::uint32_t size = sphere.size();
    Crust crust;
    crust.plate.resize(size);
    crust.light.resize(size);
    crust.base.resize(size);
    crust.uplift.resize(size);
    crust.meeting.resize(size, Meeting::None);
    crust.boundary.resize(size);
    crust.floor_age.resize(size);
    read_layout(sphere, plates, body, crust);

    const KindFinder finder{sphere, plates.boundaries()};
    const FineBoundaries fine = fine_boundaries(sphere, finder, crust);
    const Nearest nearest = nearest_sources(sphere, fine.sources);
    const Nearest ridge = nearest_sources(sphere, fine.ridges);
    for (std::uint32_t point = 0; point < size; ++point) {
        const double light = crust.light[point];
        double age = oldest_floor;
        if (ridge.source[point] != Nearest::none) {
            const double half_rate = 0.5 * fine.opening[ridge.source[point]];
            age = std::min(oldest_floor, ridge.distance[point] / std::max(half_rate, 1e-9));
        }
        crust.floor_age[point] = age;
        const double floor =
            std::max(body.abyss_depth, body.ridge_depth - (body.floor_sinking * std::sqrt(age)));
        crust.base[point] = (light * body.continent_height) + ((1.0 - light) * floor);
        crust.uplift[point] = light * 1.0e-5;
        if (nearest.source[point] == Nearest::none) {
            continue;
        }
        const Boundary& kind = *fine.kinds[nearest.source[point]];
        const double distance = nearest.distance[point];
        crust.boundary[point] = distance;
        crust.meeting[point] = distance < 3.0 * belt_width(body) ? kind.meeting : Meeting::None;
        const Shaped shaped =
            shaped_by(kind, distance, light, kind.over == crust.plate[point], floor, body);
        crust.base[point] += shaped.base;
        crust.uplift[point] += shaped.uplift;
    }
    crust.sea = open_sea(sphere, crust.base, body.smallest_sea);
    return crust;
}

} // namespace hog::formation

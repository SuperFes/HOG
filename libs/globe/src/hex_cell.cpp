#include <hog/core/assert.hpp>
#include <hog/globe/hex_cell.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <unordered_set>

namespace hog::globe {
namespace {

// Icosahedron vertices: the north pole, five in an upper ring, five in a lower ring offset half a
// step, and the south pole.
constexpr int north = 0;
constexpr int south = 11;
constexpr int upper(int k) {
    return 1 + (((k % 5) + 5) % 5);
}
constexpr int lower(int k) {
    return 6 + (((k % 5) + 5) % 5);
}

// A diamond is two triangles about its diagonal O–C: (O, A, C) holds i >= j and (O, B, C) holds
// j >= i, with A at (n, 0), B at (0, n) and C at (n, n). It holds its corner O and the edges O–A
// and O–B; the edges at C belong to the diamonds beside it.
struct Diamond {
    int o;
    int a;
    int b;
    int c;
};

constexpr std::array<Diamond, 10> diamonds = [] {
    std::array<Diamond, 10> all{};
    for (std::size_t k = 0; k < 5; ++k) {
        const auto at = static_cast<int>(k);
        all[k] = {.o = upper(at), .a = north, .b = lower(at), .c = upper(at + 1)};
        all[k + 5] = {.o = lower(at), .a = upper(at + 1), .b = south, .c = lower(at + 1)};
    }
    return all;
}();

using Face = std::array<int, 3>;
constexpr std::array<Face, 20> faces = [] {
    std::array<Face, 20> all{};
    for (std::size_t at = 0; at < all.size(); at += 4) {
        const auto k = static_cast<int>(at / 4);
        all[at] = {north, upper(k), upper(k + 1)};
        all[at + 1] = {upper(k), lower(k), upper(k + 1)};
        all[at + 2] = {lower(k), lower(k + 1), upper(k + 1)};
        all[at + 3] = {south, lower(k + 1), lower(k)};
    }
    return all;
}();

const std::array<DVec3, 12>& vertices() {
    static const std::array<DVec3, 12> all = [] {
        std::array<DVec3, 12> made{};
        const double ring = std::atan(0.5);
        const auto at = [](double latitude, double longitude) {
            return DVec3{std::cos(latitude) * std::cos(longitude),
                         std::cos(latitude) * std::sin(longitude), std::sin(latitude)};
        };
        made[north] = {0.0, 0.0, 1.0};
        made[south] = {0.0, 0.0, -1.0};
        for (int k = 0; k < 5; ++k) {
            const double step = 2.0 * std::numbers::pi / 5.0;
            made[static_cast<std::size_t>(upper(k))] = at(ring, step * k);
            made[static_cast<std::size_t>(lower(k))] = at(-ring, step * (k + 0.5));
        }
        return made;
    }();
    return all;
}

// A point of the split icosahedron as whole-numbered weights on a face's corners, summing to the
// splits along an edge.
struct Point {
    Face vertex;
    std::array<std::int64_t, 3> weight;
};

std::int64_t side_of(int level) {
    return std::int64_t{1} << level;
}

Point point_of(HexCell cell) {
    const std::int64_t n = side_of(cell.level);
    if (cell.is_pole()) {
        const int pole = cell.diamond == HexCell::north_pole ? north : south;
        return {.vertex = {pole, pole == north ? upper(0) : lower(0),
                           pole == north ? upper(1) : lower(1)},
                .weight = {n, 0, 0}};
    }
    const Diamond& d = diamonds[cell.diamond];
    const auto i = static_cast<std::int64_t>(cell.i);
    const auto j = static_cast<std::int64_t>(cell.j);
    if (i >= j) {
        return {.vertex = {d.o, d.a, d.c}, .weight = {n - i, i - j, j}};
    }
    return {.vertex = {d.o, d.b, d.c}, .weight = {n - j, j - i, i}};
}

// The weight `point` puts on `vertex`, or nothing if it puts weight on a vertex outside `among`.
std::optional<std::array<std::int64_t, 3>> weights_on(const Point& point, const Face& among) {
    std::array<std::int64_t, 3> on{};
    for (std::size_t k = 0; k < 3; ++k) {
        if (point.weight[k] == 0) {
            continue;
        }
        const auto* found = std::ranges::find(among, point.vertex[k]);
        if (found == among.end()) {
            return std::nullopt;
        }
        on[static_cast<std::size_t>(found - among.begin())] += point.weight[k];
    }
    return on;
}

HexCell locate(int level, const Point& point) {
    const std::int64_t n = side_of(level);
    const auto level8 = static_cast<std::uint8_t>(level);
    for (std::size_t k = 0; k < 3; ++k) {
        if (point.weight[k] == n) {
            const int vertex = point.vertex[k];
            if (vertex == north || vertex == south) {
                return HexCell::pole(level, vertex == north);
            }
        }
    }
    for (std::size_t at = 0; at < diamonds.size(); ++at) {
        const Diamond& d = diamonds[at];
        const auto index = static_cast<std::uint8_t>(at);
        if (const auto on = weights_on(point, {d.o, d.a, d.c})) {
            const std::int64_t i = (*on)[1] + (*on)[2];
            const std::int64_t j = (*on)[2];
            if (i < n && j < n) {
                return {.level = level8,
                        .diamond = index,
                        .i = static_cast<std::uint32_t>(i),
                        .j = static_cast<std::uint32_t>(j)};
            }
        }
        if (const auto on = weights_on(point, {d.o, d.b, d.c})) {
            const std::int64_t i = (*on)[2];
            const std::int64_t j = (*on)[1] + (*on)[2];
            if (i < n && j < n) {
                return {.level = level8,
                        .diamond = index,
                        .i = static_cast<std::uint32_t>(i),
                        .j = static_cast<std::uint32_t>(j)};
            }
        }
    }
    HOG_ASSERT(false, "every point of the split icosahedron belongs to a diamond or a pole");
    return {};
}

double angle_between(DVec3 a, DVec3 b) {
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

// The unit direction along the sphere at `from` toward `to`.
DVec3 toward(DVec3 from, DVec3 to) {
    return glm::normalize(to - (from * glm::dot(from, to)));
}

std::array<DVec3, 3> corners_of(const Face& face) {
    return {vertices()[static_cast<std::size_t>(face[0])],
            vertices()[static_cast<std::size_t>(face[1])],
            vertices()[static_cast<std::size_t>(face[2])]};
}

// A flat face is equilateral about the origin, its corners a unit from it, in the order the face
// lists them.
DVec2 flat_corner(std::size_t k) {
    constexpr double half_root3 = 0.8660254037844386;
    constexpr std::array<DVec2, 3> all{DVec2{0.0, 1.0}, DVec2{-half_root3, -0.5},
                                       DVec2{half_root3, -0.5}};
    return all[k];
}

// The equal-area projection between a flat face and its patch of sphere. Both are cut into six
// right triangles, each from the middle to a corner and the midpoint of an edge beside it. A point
// keeps the share of its triangle's area that the fan from the middle sweeps on the way to it from
// the corner, and its share of that fan's area out from the middle.
constexpr double fan_area = 4.0 * std::numbers::pi / 120.0;

// The spherical triangle's sides and angle at its corner, the same on every face.
struct Fan {
    double half_tan; // tan of half the arc from the corner to the middle
    double sin_corner;
    double cos_corner;
};

const Fan& fan() {
    static const Fan made = [] {
        const std::array<DVec3, 3> v = corners_of(faces[0]);
        const DVec3 middle = glm::normalize(v[0] + v[1] + v[2]);
        const DVec3 edge = glm::normalize(v[0] + v[1]);
        const double corner = angle_between(toward(v[0], middle), toward(v[0], edge));
        return Fan{.half_tan = std::tan(angle_between(v[0], middle) / 2.0),
                   .sin_corner = std::sin(corner),
                   .cos_corner = std::cos(corner)};
    }();
    return made;
}

// The corner a point is nearest, and the nearer of the other two, which together pick its fan.
std::array<std::size_t, 2> fan_of(const std::array<double, 3>& nearness) {
    std::size_t corner = 0;
    for (std::size_t k = 1; k < 3; ++k) {
        if (nearness[k] > nearness[corner]) {
            corner = k;
        }
    }
    const std::size_t one = (corner + 1) % 3;
    const std::size_t other = (corner + 2) % 3;
    return {corner, nearness[one] >= nearness[other] ? one : other};
}

// Shares of a flat face's corners making `at`.
std::array<double, 3> shares_at(DVec2 at) {
    std::array<double, 3> shares{};
    for (std::size_t k = 0; k < 3; ++k) {
        shares[k] = (1.0 + (2.0 * glm::dot(at, flat_corner(k)))) / 3.0;
    }
    return shares;
}

std::array<double, 3> to_flat(const Face& face, DVec3 point) {
    const std::array<DVec3, 3> v = corners_of(face);
    const DVec3 middle = glm::normalize(v[0] + v[1] + v[2]);
    const double z = angle_between(middle, point);
    if (z == 0.0) {
        return {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    }
    const auto [k, m] =
        fan_of({glm::dot(v[0], point), glm::dot(v[1], point), glm::dot(v[2], point)});
    const DVec3 edge = glm::normalize(v[k] + v[m]);
    // Where the arc from the middle through the point meets the fan's edge.
    DVec3 meet = glm::normalize(glm::cross(glm::cross(middle, point), glm::cross(v[k], edge)));
    if (glm::dot(meet, point) < 0.0) {
        meet = -meet;
    }
    const double t = std::tan(angle_between(v[k], meet) / 2.0) * fan().half_tan;
    const double share =
        2.0 * std::atan2(t * fan().sin_corner, 1.0 + (t * fan().cos_corner)) / fan_area;
    const DVec2 corner = flat_corner(k);
    const DVec2 meet_flat = corner + (share * (((corner + flat_corner(m)) * 0.5) - corner));
    const double q = angle_between(middle, meet);
    return shares_at(meet_flat * (std::sin(z / 2.0) / std::sin(q / 2.0)));
}

DVec3 from_flat(const Face& face, const std::array<double, 3>& shares) {
    const std::array<DVec3, 3> v = corners_of(face);
    const DVec3 middle = glm::normalize(v[0] + v[1] + v[2]);
    DVec2 at{0.0};
    for (std::size_t k = 0; k < 3; ++k) {
        at += shares[k] * flat_corner(k);
    }
    const double rho = glm::length(at);
    if (rho == 0.0) {
        return middle;
    }
    const auto [k, m] = fan_of(shares);
    const DVec2 corner = flat_corner(k);
    const DVec2 along = ((corner + flat_corner(m)) * 0.5) - corner;
    // Where the ray from the middle through `at` meets the fan's flat edge.
    const double det = (along.x * at.y) - (at.x * along.y);
    const double share = std::clamp(((at.x * corner.y) - (at.y * corner.x)) / det, 0.0, 1.0);
    const DVec2 meet_flat = corner + (share * along);
    // The arc from the corner that sweeps that share of the fan's area.
    const double e = std::tan(share * fan_area / 2.0);
    const double t = e / (fan().half_tan * (fan().sin_corner - (e * fan().cos_corner)));
    const double x = 2.0 * std::atan(t);
    const DVec3 edge = glm::normalize(v[k] + v[m]);
    const DVec3 meet = (std::cos(x) * v[k]) + (std::sin(x) * toward(v[k], edge));
    const double q = angle_between(middle, meet);
    const double z =
        2.0 * std::asin(std::min(1.0, rho / glm::length(meet_flat) * std::sin(q / 2.0)));
    return (std::cos(z) * middle) + (std::sin(z) * toward(middle, meet));
}

DVec3 position_of(const Point& point, std::int64_t n) {
    std::array<double, 3> shares{};
    for (std::size_t k = 0; k < 3; ++k) {
        shares[k] = static_cast<double>(point.weight[k]) / static_cast<double>(n);
    }
    return from_flat(point.vertex, shares);
}

// The face whose patch of sphere holds `direction`: the one whose middle is nearest it.
const Face& face_under(DVec3 direction) {
    const Face* nearest = faces.data();
    double best = -2.0;
    for (const Face& face : faces) {
        const std::array<DVec3, 3> v = corners_of(face);
        const double along = glm::dot(glm::normalize(v[0] + v[1] + v[2]), direction);
        if (along > best) {
            best = along;
            nearest = &face;
        }
    }
    return *nearest;
}
} // namespace

std::uint64_t index_of(HexCell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const auto n = static_cast<std::uint64_t>(side_of(cell.level));
    if (cell.is_pole()) {
        return (10 * n * n) + (cell.diamond == HexCell::north_pole ? 0U : 1U);
    }
    return (cell.diamond * n * n) + (std::uint64_t{cell.j} * n) + cell.i;
}

HexCell hex_cell_at_index(int level, std::uint64_t index) {
    HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
    HOG_ASSERT(index < cell_count(level), "index out of range for the level");
    const auto n = static_cast<std::uint64_t>(side_of(level));
    const std::uint64_t per = n * n;
    if (index >= 10 * per) {
        return HexCell::pole(level, index == 10 * per);
    }
    const std::uint64_t within = index % per;
    return {.level = static_cast<std::uint8_t>(level),
            .diamond = static_cast<std::uint8_t>(index / per),
            .i = static_cast<std::uint32_t>(within % n),
            .j = static_cast<std::uint32_t>(within / n)};
}

bool is_pentagon(HexCell cell) {
    const Point point = point_of(cell);
    return std::ranges::find(point.weight, side_of(cell.level)) != point.weight.end();
}

HexNeighbors neighbors(HexCell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const Point point = point_of(cell);
    HexNeighbors found;
    // Every face the point lies on lends it the six steps along its edges that stay on it.
    for (const Face& face : faces) {
        const auto on = weights_on(point, face);
        if (!on) {
            continue;
        }
        for (std::size_t from = 0; from < 3; ++from) {
            for (std::size_t to = 0; to < 3; ++to) {
                if (from == to || (*on)[from] == 0) {
                    continue;
                }
                Point step{.vertex = face, .weight = *on};
                --step.weight[from];
                ++step.weight[to];
                const HexCell next = locate(cell.level, step);
                const auto known = found.all();
                if (std::ranges::find(known, next) == known.end()) {
                    HOG_ASSERT(found.count < found.cells.size(), "at most six neighbours");
                    found.cells[found.count++] = next;
                }
            }
        }
    }
    return found;
}

DVec3 center(HexCell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    return position_of(point_of(cell), side_of(cell.level));
}

HexCell cell_at(DVec3 direction, int level) {
    HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
    const Face& face = face_under(direction);
    const std::array<double, 3> shares = to_flat(face, direction);
    // The cell is the flat hexagon about the nearest lattice point, which is a corner of the
    // little triangle holding the point. On the flat face, distance squared is 3/2 the sum of the
    // shares' differences squared.
    const std::int64_t n = side_of(level);
    std::array<double, 3> scaled{};
    std::array<std::int64_t, 3> floor{};
    for (std::size_t k = 0; k < 3; ++k) {
        scaled[k] = shares[k] * static_cast<double>(n);
        floor[k] = static_cast<std::int64_t>(std::floor(scaled[k]));
    }
    Point best{.vertex = face, .weight = {}};
    double nearest = std::numeric_limits<double>::max();
    for (std::int64_t a = -1; a <= 1; ++a) {
        for (std::int64_t b = -1; b <= 1; ++b) {
            const std::array<std::int64_t, 3> weight{floor[0] + a, floor[1] + b,
                                                     n - floor[0] - a - floor[1] - b};
            if (std::ranges::any_of(weight, [n](std::int64_t w) { return w < 0 || w > n; })) {
                continue;
            }
            double apart = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                const double d = static_cast<double>(weight[k]) - scaled[k];
                apart += d * d;
            }
            if (apart < nearest) {
                nearest = apart;
                best.weight = weight;
            }
        }
    }
    return locate(level, best);
}

double solid_angle(HexCell cell) {
    const auto n = static_cast<double>(side_of(cell.level));
    const double hexagon = 4.0 * std::numbers::pi / (10.0 * n * n);
    return is_pentagon(cell) ? hexagon * 5.0 / 6.0 : hexagon;
}

double prism_volume(HexCell cell, double inner, double outer) {
    return solid_angle(cell) * ((outer * outer * outer) - (inner * inner * inner)) / 3.0;
}

HexRing ring(HexCell cell) {
    const HexNeighbors around = neighbors(cell);
    HexRing made;
    made.count = around.count;
    std::ranges::copy(around.all(), made.neighbors.begin());
    // Anticlockwise seen from outside.
    const DVec3 up = center(cell);
    const DVec3 first = toward(up, center(made.neighbors[0]));
    const DVec3 second = glm::cross(up, first);
    std::array<double, 6> turn{};
    for (std::size_t k = 0; k < made.count; ++k) {
        const DVec3 to = center(made.neighbors[k]);
        turn[k] = std::atan2(glm::dot(to, second), glm::dot(to, first));
    }
    std::array<std::size_t, 6> order{0, 1, 2, 3, 4, 5};
    std::ranges::sort(std::span{order}.first(made.count),
                      [&](std::size_t a, std::size_t b) { return turn[a] < turn[b]; });
    std::array<HexCell, 6> sorted{};
    for (std::size_t k = 0; k < made.count; ++k) {
        sorted[k] = made.neighbors[order[k]];
    }
    made.neighbors = sorted;

    // Each corner is the middle of the little flat triangle of the cell and two neighbours in turn.
    const std::int64_t n = side_of(cell.level);
    const Point here = point_of(cell);
    for (std::size_t k = 0; k < made.count; ++k) {
        const Point one = point_of(made.neighbors[k]);
        const Point two = point_of(made.neighbors[(k + 1) % made.count]);
        for (const Face& face : faces) {
            const auto a = weights_on(here, face);
            const auto b = weights_on(one, face);
            const auto c = weights_on(two, face);
            if (!a || !b || !c) {
                continue;
            }
            std::array<double, 3> shares{};
            for (std::size_t w = 0; w < 3; ++w) {
                shares[w] = static_cast<double>((*a)[w] + (*b)[w] + (*c)[w]) /
                            (3.0 * static_cast<double>(n));
            }
            made.corners[k] = from_flat(face, shares);
            break;
        }
    }
    return made;
}

HexShares children(HexCell cell) {
    HOG_ASSERT(cell.level < max_level, "the finest level has no children");
    const int finer = cell.level + 1;
    const Point here = point_of(cell);
    HexShares made;
    Point middle = here;
    for (std::int64_t& weight : middle.weight) {
        weight *= 2;
    }
    made.cells[0] = locate(finer, middle);
    made.halves[0] = 2;
    made.count = 1;
    // Halfway to a neighbour is their two points added on a face both lie on.
    const HexNeighbors around = neighbors(cell);
    for (const HexCell next : around.all()) {
        const Point there = point_of(next);
        for (const Face& face : faces) {
            const auto a = weights_on(here, face);
            const auto b = weights_on(there, face);
            if (!a || !b) {
                continue;
            }
            Point halfway{.vertex = face, .weight = {}};
            for (std::size_t k = 0; k < 3; ++k) {
                halfway.weight[k] = (*a)[k] + (*b)[k];
            }
            made.cells[made.count] = locate(finer, halfway);
            made.halves[made.count] = 1;
            ++made.count;
            break;
        }
    }
    return made;
}

HexShares parents(HexCell cell) {
    HOG_ASSERT(cell.level > 0, "the coarsest level has no parents");
    const int coarser = cell.level - 1;
    const Point here = point_of(cell);
    std::array<std::size_t, 3> odd{};
    std::size_t odds = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        if (here.weight[k] % 2 != 0) {
            odd[odds++] = k;
        }
    }
    HexShares made;
    if (odds == 0) {
        Point whole = here;
        for (std::int64_t& weight : whole.weight) {
            weight /= 2;
        }
        made.cells[0] = locate(coarser, whole);
        made.halves[0] = 2;
        made.count = 1;
        return made;
    }
    // The weights add up to an even number, so two are odd: the cell lies halfway along the edge
    // between the coarse points a unit either way along them.
    HOG_ASSERT(odds == 2, "a point has no odd weights or two");
    for (const std::int64_t way : {1, -1}) {
        Point end = here;
        end.weight[odd[0]] += way;
        end.weight[odd[1]] -= way;
        for (std::int64_t& weight : end.weight) {
            weight /= 2;
        }
        made.cells[made.count] = locate(coarser, end);
        made.halves[made.count] = 1;
        ++made.count;
    }
    return made;
}

std::array<std::int64_t, 7> share_out(std::int64_t amount, const HexShares& shares) {
    std::array<std::int64_t, 7> dealt{};
    std::int64_t halves = 0;
    for (std::size_t k = 0; k < shares.count; ++k) {
        halves += shares.halves[k];
    }
    HOG_ASSERT(halves > 0, "nothing to share out over");
    // Rounded towards minus infinity, so the remainder is never negative.
    const auto floor_div = [](std::int64_t a, std::int64_t b) {
        return (a / b) - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
    };
    const std::int64_t each = floor_div(amount, halves);
    std::int64_t left = amount - (each * halves);
    for (std::size_t k = 0; k < shares.count; ++k) {
        dealt[k] = each * shares.halves[k];
    }
    for (std::size_t k = 0; left > 0; k = (k + 1) % shares.count) {
        ++dealt[k];
        --left;
    }
    return dealt;
}

std::vector<HexCell> disc(HexCell centre, int rings) {
    HOG_ASSERT(rings >= 0, "a disc reaches out, not in");
    std::vector<HexCell> cells{centre};
    std::unordered_set<std::uint64_t> seen{index_of(centre)};
    std::size_t ring_begin = 0;
    for (int ring = 0; ring < rings; ++ring) {
        const std::size_t ring_end = cells.size();
        for (std::size_t k = ring_begin; k < ring_end; ++k) {
            const HexNeighbors around = neighbors(cells[k]);
            for (const HexCell next : around.all()) {
                if (seen.insert(index_of(next)).second) {
                    cells.push_back(next);
                }
            }
        }
        ring_begin = ring_end;
    }
    return cells;
}

} // namespace hog::globe

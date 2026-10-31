#include <hog/core/assert.hpp>
#include <hog/globe/hex_cell.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

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

} // namespace

std::uint64_t index_of(HexCell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const auto n = static_cast<std::uint64_t>(side_of(cell.level));
    if (cell.is_pole()) {
        return (10 * n * n) + (cell.diamond == HexCell::north_pole ? 0U : 1U);
    }
    return (cell.diamond * n * n) + (std::uint64_t{cell.j} * n) + cell.i;
}

HexCell cell_at_index(int level, std::uint64_t index) {
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

DVec3 center(HexCell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const Point point = point_of(cell);
    DVec3 sum{0.0};
    for (std::size_t k = 0; k < 3; ++k) {
        sum += static_cast<double>(point.weight[k]) *
               vertices()[static_cast<std::size_t>(point.vertex[k])];
    }
    return glm::normalize(sum);
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

HexCell cell_at(DVec3 direction, int level) {
    HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
    // A face's patch of sphere is the set of directions nearer its middle than any other's.
    const Face* nearest = faces.data();
    double best = -2.0;
    for (const Face& face : faces) {
        const DVec3 middle = vertices()[static_cast<std::size_t>(face[0])] +
                             vertices()[static_cast<std::size_t>(face[1])] +
                             vertices()[static_cast<std::size_t>(face[2])];
        const double along = glm::dot(glm::normalize(middle), direction);
        if (along > best) {
            best = along;
            nearest = &face;
        }
    }
    const Face& face = *nearest;
    const glm::dmat3 corners{vertices()[static_cast<std::size_t>(face[0])],
                             vertices()[static_cast<std::size_t>(face[1])],
                             vertices()[static_cast<std::size_t>(face[2])]};
    DVec3 share = glm::inverse(corners) * direction;
    share /= share.x + share.y + share.z;

    // The lattice point at the floor of each share, with what's left given to the largest
    // remainders, then walked to the nearest centre on the sphere.
    const std::int64_t n = side_of(level);
    Point start{.vertex = face, .weight = {}};
    std::array<double, 3> left{};
    std::int64_t given = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        const double scaled =
            std::clamp(share[static_cast<glm::length_t>(k)], 0.0, 1.0) * static_cast<double>(n);
        start.weight[k] = static_cast<std::int64_t>(std::floor(scaled));
        left[k] = scaled - static_cast<double>(start.weight[k]);
        given += start.weight[k];
    }
    while (given < n) {
        const auto most = static_cast<std::size_t>(std::ranges::max_element(left) - left.begin());
        ++start.weight[most];
        left[most] = -1.0;
        ++given;
    }
    while (given > n) {
        const auto most =
            static_cast<std::size_t>(std::ranges::max_element(start.weight) - start.weight.begin());
        --start.weight[most];
        --given;
    }

    HexCell here = locate(level, start);
    double nearness = glm::dot(center(here), direction);
    for (;;) {
        HexCell closer = here;
        const HexNeighbors around = neighbors(here);
        for (const HexCell next : around.all()) {
            const double along = glm::dot(center(next), direction);
            if (along > nearness) {
                nearness = along;
                closer = next;
            }
        }
        if (closer == here) {
            return here;
        }
        here = closer;
    }
}

} // namespace hog::globe

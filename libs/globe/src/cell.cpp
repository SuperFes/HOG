#include <hog/core/assert.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/maths.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <utility>

namespace hog::globe {
namespace {

// A face's outward axis, and its two own axes, right-handed seen from outside.
struct Frame {
    int normal;
    int sign;
    int u;
    int v;
};

Frame frame_of(int face) {
    const int k = face % 3;
    return face < 3 ? Frame{.normal = k, .sign = 1, .u = (k + 1) % 3, .v = (k + 2) % 3}
                    : Frame{.normal = k, .sign = -1, .u = (k + 2) % 3, .v = (k + 1) % 3};
}

// DVec3 indexes by glm's own length type.
glm::length_t along(int axis) {
    return static_cast<glm::length_t>(axis);
}

int face_of(int axis, int sign) {
    return sign > 0 ? axis : axis + 3;
}

// A cell's middle on the cube's surface in integers: its face's axis at ±n, the other two odd
// (zero at level 0) and inside (-n, n), two to a cell, so stepping off a face lands exactly on the
// next one's edge row.
using Surface = std::array<std::int64_t, 3>;

Surface surface_of(Cell cell) {
    const std::int64_t n = std::int64_t{1} << cell.level;
    const Frame frame = frame_of(cell.face);
    Surface point{};
    point.at(static_cast<std::size_t>(frame.normal)) = frame.sign * n;
    point.at(static_cast<std::size_t>(frame.u)) = (2 * std::int64_t{cell.i}) + 1 - n;
    point.at(static_cast<std::size_t>(frame.v)) = (2 * std::int64_t{cell.j}) + 1 - n;
    return point;
}

Cell cell_of(const Surface& point, int level) {
    const std::int64_t n = std::int64_t{1} << level;
    for (int axis = 0; axis < 3; ++axis) {
        const std::int64_t at = point.at(static_cast<std::size_t>(axis));
        if (std::abs(at) != n) {
            continue;
        }
        const Frame frame = frame_of(face_of(axis, at > 0 ? 1 : -1));
        return {.level = static_cast<std::uint8_t>(level),
                .face = static_cast<std::uint8_t>(face_of(axis, at > 0 ? 1 : -1)),
                .i = static_cast<std::uint32_t>(
                    (point.at(static_cast<std::size_t>(frame.u)) + n - 1) / 2),
                .j = static_cast<std::uint32_t>(
                    (point.at(static_cast<std::size_t>(frame.v)) + n - 1) / 2)};
    }
    HOG_ASSERT(false, "point is off the cube's surface");
    return {};
}

// `by` cells along `axis`, straight on over onto the next face if it runs off this one; less than
// a face across.
Surface step(Surface point, int axis, int by, std::int64_t n) {
    std::int64_t& moved = point.at(static_cast<std::size_t>(axis));
    moved += 2 * std::int64_t{by};
    if (std::abs(moved) > n) {
        // What's left past the edge runs on down the next face, away from it.
        const std::int64_t past = std::abs(moved) - n;
        for (std::int64_t& other : point) {
            if (&other != &moved && std::abs(other) == n) {
                other -= other > 0 ? past : -past;
            }
        }
        moved = moved > 0 ? n : -n;
    }
    return point;
}

// The face's own axes that aren't its outward one, where `point` lies.
std::array<int, 2> in_plane(const Surface& point, std::int64_t n) {
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(point.at(static_cast<std::size_t>(axis))) == n) {
            return {(axis + 1) % 3, (axis + 2) % 3};
        }
    }
    HOG_ASSERT(false, "point is off the cube's surface");
    return {};
}

// The tangent mapping: even steps on the cube's face are even turns of angle out from its middle.
double warped(double linear) {
    return maths::tan(std::numbers::pi / 4.0 * linear);
}

double unwarped(double tangent) {
    return maths::atan(tangent) * 4.0 / std::numbers::pi;
}

DVec3 direction_at(const Frame& frame, double u, double v) {
    DVec3 point{0.0};
    point[along(frame.normal)] = frame.sign;
    point[along(frame.u)] = warped(u);
    point[along(frame.v)] = warped(v);
    return glm::normalize(point);
}

void add(CellNeighbors& found, Cell cell) {
    if (std::ranges::find(found.all(), cell) == found.all().end()) {
        found.cells.at(found.count++) = cell;
    }
}

// The solid angle of the spherical triangle a, b, c (Van Oosterom and Strackee).
double triangle_angle(const DVec3& a, const DVec3& b, const DVec3& c) {
    const double over = std::abs(glm::dot(a, glm::cross(b, c)));
    const double under = 1.0 + glm::dot(a, b) + glm::dot(b, c) + glm::dot(c, a);
    return 2.0 * maths::atan2(over, under);
}

} // namespace

std::uint64_t index_of(Cell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const std::uint64_t n = std::uint64_t{1} << cell.level;
    return (std::uint64_t{cell.face} * n * n) + (std::uint64_t{cell.j} * n) + cell.i;
}

Cell cell_at_index(int level, std::uint64_t index) {
    HOG_ASSERT(level >= 0 && level <= Cell::max_level, "level out of range");
    HOG_ASSERT(index < cells_at(level), "index out of range for the level");
    const std::uint64_t n = std::uint64_t{1} << level;
    const std::uint64_t within = index % (n * n);
    return {.level = static_cast<std::uint8_t>(level),
            .face = static_cast<std::uint8_t>(index / (n * n)),
            .i = static_cast<std::uint32_t>(within % n),
            .j = static_cast<std::uint32_t>(within / n)};
}

DVec3 center(Cell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const double n = std::ldexp(1.0, cell.level);
    return direction_at(frame_of(cell.face), (((2.0 * cell.i) + 1.0) / n) - 1.0,
                        (((2.0 * cell.j) + 1.0) / n) - 1.0);
}

Cell cell_under(DVec3 direction, int level) {
    return place_under(direction, level).cell;
}

CellPlace place_under(DVec3 direction, int level) {
    HOG_ASSERT(level >= 0 && level <= Cell::max_level, "level out of range");
    HOG_ASSERT(glm::length(direction) > 0.0, "no direction");
    int normal = 0;
    for (int axis = 1; axis < 3; ++axis) {
        if (std::abs(direction[along(axis)]) > std::abs(direction[along(normal)])) {
            normal = axis;
        }
    }
    const int face = face_of(normal, direction[along(normal)] > 0.0 ? 1 : -1);
    const Frame frame = frame_of(face);
    const double n = std::ldexp(1.0, level);
    const double out = std::abs(direction[along(normal)]);
    const auto across = [&](double offset) { return (unwarped(offset / out) + 1.0) / 2.0 * n; };
    const double u = across(direction[along(frame.u)]);
    const double v = across(direction[along(frame.v)]);
    const auto index = [&](double at) { return std::clamp(std::floor(at), 0.0, n - 1.0); };
    const double i = index(u);
    const double j = index(v);
    return {.cell = {.level = static_cast<std::uint8_t>(level),
                     .face = static_cast<std::uint8_t>(face),
                     .i = static_cast<std::uint32_t>(i),
                     .j = static_cast<std::uint32_t>(j)},
            .i = std::clamp(u - i, 0.0, 1.0) - 0.5,
            .j = std::clamp(v - j, 0.0, 1.0) - 0.5};
}

std::array<DVec3, 4> corners(Cell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const Frame frame = frame_of(cell.face);
    const double n = std::ldexp(1.0, cell.level);
    const double u0 = ((2.0 * cell.i) / n) - 1.0;
    const double u1 = ((2.0 * (cell.i + 1.0)) / n) - 1.0;
    const double v0 = ((2.0 * cell.j) / n) - 1.0;
    const double v1 = ((2.0 * (cell.j + 1.0)) / n) - 1.0;
    return {direction_at(frame, u0, v0), direction_at(frame, u1, v0), direction_at(frame, u1, v1),
            direction_at(frame, u0, v1)};
}

double solid_angle(Cell cell) {
    const auto corner = corners(cell);
    return triangle_angle(corner[0], corner[1], corner[2]) +
           triangle_angle(corner[0], corner[2], corner[3]);
}

double prism_volume(Cell cell, double inner, double outer) {
    return solid_angle(cell) * ((outer * outer * outer) - (inner * inner * inner)) / 3.0;
}

CellNeighbors sides(Cell cell, int apart) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const std::int64_t n = std::int64_t{1} << cell.level;
    HOG_ASSERT(apart > 0 && apart < std::max<std::int64_t>(n, 2), "less than a face across");
    const Surface middle = surface_of(cell);
    CellNeighbors found;
    for (const int axis : in_plane(middle, n)) {
        for (const int by : {-apart, apart}) {
            add(found, cell_of(step(middle, axis, by, n), cell.level));
        }
    }
    return found;
}

CellNeighbors around(Cell cell) {
    CellNeighbors found = sides(cell);
    const std::int64_t n = std::int64_t{1} << cell.level;
    const Surface middle = surface_of(cell);
    const auto [a, b] = in_plane(middle, n);
    const CellNeighbors beside = found;
    for (const int by_a : {-1, 1}) {
        for (const int by_b : {-1, 1}) {
            // Both ways round; at the cube's corners each lands back beside it, as three cells
            // meet there and none is only at the corner.
            for (const Cell diagonal :
                 {cell_of(step(step(middle, a, by_a, n), b, by_b, n), cell.level),
                  cell_of(step(step(middle, b, by_b, n), a, by_a, n), cell.level)}) {
                if (diagonal != cell &&
                    std::ranges::find(beside.all(), diagonal) == beside.all().end()) {
                    add(found, diagonal);
                }
            }
        }
    }
    return found;
}

std::optional<Cell> next_to(Cell cell, int di, int dj) {
    const std::int64_t n = std::int64_t{1} << cell.level;
    HOG_ASSERT(cell.is_valid() && std::abs(di) < std::max<std::int64_t>(n, 2) &&
                   std::abs(dj) < std::max<std::int64_t>(n, 2),
               "less than a face across");
    const auto off = [&](std::uint32_t at, int by) {
        const std::int64_t to = std::int64_t{at} + by;
        return to < 0 || to >= n;
    };
    if (off(cell.i, di) && off(cell.j, dj)) {
        return std::nullopt;
    }
    const Frame frame = frame_of(cell.face);
    Surface point = surface_of(cell);
    // Along the face's own axes, which stay in the next face's plane along the edge crossed.
    if (di != 0) {
        point = step(point, frame.u, di, n);
    }
    if (dj != 0) {
        point = step(point, frame.v, dj, n);
    }
    return cell_of(point, cell.level);
}

std::array<Cell, 4> children(Cell cell) {
    HOG_ASSERT(cell.is_valid() && cell.level < Cell::max_level, "cell has no finer level");
    const auto level = static_cast<std::uint8_t>(cell.level + 1);
    const std::uint32_t i = 2 * cell.i;
    const std::uint32_t j = 2 * cell.j;
    return {Cell{.level = level, .face = cell.face, .i = i, .j = j},
            Cell{.level = level, .face = cell.face, .i = i + 1, .j = j},
            Cell{.level = level, .face = cell.face, .i = i, .j = j + 1},
            Cell{.level = level, .face = cell.face, .i = i + 1, .j = j + 1}};
}

Cell parent(Cell cell) {
    HOG_ASSERT(cell.is_valid() && cell.level > 0, "cell has no coarser level");
    return {.level = static_cast<std::uint8_t>(cell.level - 1),
            .face = cell.face,
            .i = cell.i / 2,
            .j = cell.j / 2};
}

} // namespace hog::globe

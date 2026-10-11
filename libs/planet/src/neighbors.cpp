#include <hog/core/assert.hpp>
#include <hog/planet/cube_face.hpp>
#include <hog/planet/neighbors.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace hog::planet {

namespace {

using Point = std::array<std::int64_t, 3>;

[[nodiscard]] std::int64_t& at(Point& point, Axis axis) {
    return point[static_cast<std::size_t>(axis.index)];
}

// Cell centres in doubled units on a cube of half-size n = 2^level, so every centre is integral:
// the normal coordinate is +-n and the other two lie strictly between -n and n.
[[nodiscard]] Point centre(const FaceIJ& cell, std::int64_t n) {
    const FaceFrame frame = face_frame(cell.face);
    Point point{};
    at(point, frame.normal) = frame.normal.sign * n;
    at(point, frame.u) = frame.u.sign * ((2 * std::int64_t{cell.i}) + 1 - n);
    at(point, frame.v) = frame.v.sign * ((2 * std::int64_t{cell.j}) + 1 - n);
    return point;
}

[[nodiscard]] CellId from_centre(const Point& point, std::int64_t n, int level) {
    int face = -1;
    for (int axis = 0; axis < 3; ++axis) {
        const std::int64_t value = point[static_cast<std::size_t>(axis)];
        if (value == n || value == -n) {
            face = face_from_normal({.index = axis, .sign = value > 0 ? 1 : -1});
        }
    }
    HOG_ASSERT(face >= 0, "point is not a cell centre");
    const FaceFrame frame = face_frame(face);
    const auto to_index = [&](Axis axis) {
        const std::int64_t s = axis.sign * point[static_cast<std::size_t>(axis.index)];
        return static_cast<std::uint32_t>((s + n - 1) / 2);
    };
    return CellId::from_face_ij(face, to_index(frame.u), to_index(frame.v), level);
}

// Moves du cells along u and dv along v. A centre pushed past a face edge is folded over it onto
// the neighbouring face; past two edges at once it would be off a cube corner, where no cell is.
[[nodiscard]] std::optional<CellId> step(CellId cell, int du, int dv) {
    const FaceIJ ij = cell.to_face_ij();
    const std::int64_t n = std::int64_t{1} << ij.level;
    const FaceFrame frame = face_frame(ij.face);

    Point point = centre(ij, n);
    at(point, frame.u) += std::int64_t{2} * frame.u.sign * du;
    at(point, frame.v) += std::int64_t{2} * frame.v.sign * dv;

    const bool past_u = at(point, frame.u) > n || at(point, frame.u) < -n;
    const bool past_v = at(point, frame.v) > n || at(point, frame.v) < -n;
    if (past_u && past_v) {
        return std::nullopt;
    }
    if (past_u || past_v) {
        std::int64_t& over = at(point, past_u ? frame.u : frame.v);
        over = over > 0 ? n : -n;
        at(point, frame.normal) = frame.normal.sign * (n - 1);
    }
    return from_centre(point, n, ij.level);
}

[[nodiscard]] CellId edge_step(CellId cell, int du, int dv) {
    const auto neighbor = step(cell, du, dv);
    HOG_ASSERT(neighbor.has_value(), "an edge step crosses at most one face edge");
    return *neighbor;
}

} // namespace

std::array<CellId, 4> edge_neighbors(CellId cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    return {edge_step(cell, -1, 0), edge_step(cell, 1, 0), edge_step(cell, 0, -1),
            edge_step(cell, 0, 1)};
}

void VertexNeighbors::push_back(CellId cell) {
    HOG_ASSERT(size_ < cells_.size(), "a cell has at most eight neighbours");
    cells_.at(size_++) = cell;
}

VertexNeighbors vertex_neighbors(CellId cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    VertexNeighbors neighbors;
    for (int dv = -1; dv <= 1; ++dv) {
        for (int du = -1; du <= 1; ++du) {
            if (du == 0 && dv == 0) {
                continue;
            }
            if (const auto neighbor = step(cell, du, dv)) {
                neighbors.push_back(*neighbor);
            }
        }
    }
    return neighbors;
}

} // namespace hog::planet

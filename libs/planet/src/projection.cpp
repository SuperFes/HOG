#include <hog/core/assert.hpp>
#include <hog/planet/cube_face.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace hog::planet {

namespace {

[[nodiscard]] double& component(DVec3& v, Axis axis) {
    return v[static_cast<DVec3::length_type>(axis.index)];
}

[[nodiscard]] double signed_component(const DVec3& v, Axis axis) {
    return axis.sign * v[static_cast<DVec3::length_type>(axis.index)];
}

// Cell index along one face axis for a warped coordinate, clamped against rounding at the edges.
[[nodiscard]] std::uint32_t st_to_index(double s, int level) {
    const double size = std::ldexp(1.0, level);
    const double scaled = std::clamp(std::floor(s * size), 0.0, size - 1.0);
    return static_cast<std::uint32_t>(scaled);
}

[[nodiscard]] double index_to_st(std::uint32_t i, int level) {
    return std::ldexp(static_cast<double>(i), -level);
}

} // namespace

double st_to_uv(double s) noexcept {
    if (s >= 0.5) {
        return (1.0 / 3.0) * ((4.0 * s * s) - 1.0);
    }
    return (1.0 / 3.0) * (1.0 - (4.0 * (1.0 - s) * (1.0 - s)));
}

double uv_to_st(double u) noexcept {
    if (u >= 0.0) {
        return 0.5 * std::sqrt(1.0 + (3.0 * u));
    }
    return 1.0 - (0.5 * std::sqrt(1.0 - (3.0 * u)));
}

DVec3 face_uv_to_xyz(int face, double u, double v) {
    const FaceFrame frame = face_frame(face);
    DVec3 point{0.0};
    component(point, frame.normal) = frame.normal.sign;
    component(point, frame.u) = frame.u.sign * u;
    component(point, frame.v) = frame.v.sign * v;
    return point;
}

FaceUV xyz_to_face_uv(DVec3 direction) {
    DVec3::length_type axis = 0;
    for (DVec3::length_type candidate = 1; candidate < 3; ++candidate) {
        if (std::abs(direction[candidate]) > std::abs(direction[axis])) {
            axis = candidate;
        }
    }
    HOG_ASSERT(direction[axis] != 0.0, "direction must be non-zero");

    const int face =
        face_from_normal({.index = static_cast<int>(axis), .sign = direction[axis] > 0.0 ? 1 : -1});
    const FaceFrame frame = face_frame(face);
    const double depth = signed_component(direction, frame.normal);
    return {.face = face,
            .u = signed_component(direction, frame.u) / depth,
            .v = signed_component(direction, frame.v) / depth};
}

DVec3 cell_center(CellId cell) {
    const FaceIJ ij = cell.to_face_ij();
    // The centre sits half a cell in, which is the corner of the first child.
    const double half = std::ldexp(1.0, -(ij.level + 1));
    const double s = index_to_st(ij.i, ij.level) + half;
    const double t = index_to_st(ij.j, ij.level) + half;
    return glm::normalize(face_uv_to_xyz(ij.face, st_to_uv(s), st_to_uv(t)));
}

std::array<DVec3, 4> cell_vertices(CellId cell) {
    const FaceIJ ij = cell.to_face_ij();
    const double u0 = st_to_uv(index_to_st(ij.i, ij.level));
    const double u1 = st_to_uv(index_to_st(ij.i + 1, ij.level));
    const double v0 = st_to_uv(index_to_st(ij.j, ij.level));
    const double v1 = st_to_uv(index_to_st(ij.j + 1, ij.level));
    return {glm::normalize(face_uv_to_xyz(ij.face, u0, v0)),
            glm::normalize(face_uv_to_xyz(ij.face, u1, v0)),
            glm::normalize(face_uv_to_xyz(ij.face, u1, v1)),
            glm::normalize(face_uv_to_xyz(ij.face, u0, v1))};
}

double cell_area(CellId cell) {
    // Two spherical triangles (Van Oosterom and Strackee).
    const auto triangle = [](DVec3 a, DVec3 b, DVec3 c) {
        const double numerator = glm::dot(a, glm::cross(b, c));
        const double denominator = 1.0 + glm::dot(a, b) + glm::dot(b, c) + glm::dot(c, a);
        return 2.0 * std::atan2(std::abs(numerator), denominator);
    };
    const auto v = cell_vertices(cell);
    return triangle(v[0], v[1], v[2]) + triangle(v[0], v[2], v[3]);
}

CellId cell_at(DVec3 direction, int level) {
    HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
    const FaceUV uv = xyz_to_face_uv(direction);
    return CellId::from_face_ij(uv.face, st_to_index(uv_to_st(uv.u), level),
                                st_to_index(uv_to_st(uv.v), level), level);
}

WorldPosition surface_position(CellId cell, double radius) {
    HOG_ASSERT(radius > 0.0, "radius must be positive");
    return WorldPosition{cell_center(cell) * radius};
}

CellId cell_at(WorldPosition position, int level) {
    return cell_at(position.metres(), level);
}

int level_for(double cell_size, double planet_radius) {
    HOG_ASSERT(cell_size > 0.0 && planet_radius > 0.0, "sizes must be positive");
    // A face spans a quarter of a great circle, halved at each level.
    double across = std::numbers::pi / 2.0 * planet_radius;
    int level = 0;
    while (across > cell_size && level < max_level) {
        across /= 2.0;
        ++level;
    }
    return level;
}

} // namespace hog::planet

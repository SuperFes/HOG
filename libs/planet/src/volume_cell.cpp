#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/volume_cell.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hog::planet {

namespace {

[[nodiscard]] double shell_size(int shell_level) {
    return std::ldexp(volume_span, -shell_level);
}

[[nodiscard]] double volume_bottom(double radius) {
    return radius - (volume_span / 2.0);
}

} // namespace

ShellBounds shell_bounds(VolumeCell cell, double radius) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const double size = shell_size(cell.shell_level());
    const double inner = volume_bottom(radius) + (static_cast<double>(cell.shell()) * size);
    return {.inner = inner, .outer = inner + size};
}

WorldPosition volume_center(VolumeCell cell, double radius) {
    const ShellBounds bounds = shell_bounds(cell, radius);
    return WorldPosition{cell_center(cell.column()) * ((bounds.inner + bounds.outer) / 2.0)};
}

VolumeCell volume_cell_at(WorldPosition position, double radius, int level, int shell_level) {
    HOG_ASSERT(shell_level >= 0 && shell_level <= max_shell_level, "shell level out of range");
    const double from_bottom = glm::length(position.metres()) - volume_bottom(radius);
    if (from_bottom < 0.0 || from_bottom >= volume_span) {
        return {};
    }
    const auto shell =
        static_cast<std::uint32_t>(std::floor(from_bottom / shell_size(shell_level)));
    return VolumeCell::from(cell_at(position, level), shell_level, shell);
}

double exit_distance(VolumeCell cell, double radius, WorldPosition from, DVec3 direction) {
    const DVec3 at = from.metres();
    const ShellBounds bounds = shell_bounds(cell, radius);
    // Along the ray, |at + t direction|^2 = t^2 + 2 b t + c - r^2.
    const double b = glm::dot(at, direction);
    const double c = glm::dot(at, at);
    double exit = (-b) + std::sqrt(std::max((b * b) - c + (bounds.outer * bounds.outer), 0.0));
    const double inner = (b * b) - c + (bounds.inner * bounds.inner);
    if (b < 0.0 && inner >= 0.0) {
        exit = std::min(exit, (-b) - std::sqrt(inner));
    }
    const auto corners = cell_vertices(cell.column());
    const DVec3 middle = cell_center(cell.column());
    for (std::size_t index = 0; index < corners.size(); ++index) {
        DVec3 side = glm::cross(corners[index], corners[(index + 1) % corners.size()]);
        side = glm::dot(side, middle) < 0.0 ? -side : side;
        const double toward = glm::dot(side, direction);
        if (toward < 0.0) {
            exit = std::min(exit, -glm::dot(side, at) / toward);
        }
    }
    return std::max(exit, 0.0);
}

std::array<VolumeCell, side_count> face_neighbors(VolumeCell cell) {
    HOG_ASSERT(cell.is_valid(), "invalid cell");
    const int level = cell.shell_level();
    const std::uint32_t shell = cell.shell();
    std::array<VolumeCell, side_count> around{};
    const auto across = edge_neighbors(cell.column());
    for (std::size_t side = 0; side < across.size(); ++side) {
        around[side] = VolumeCell::from(across[side], level, shell);
    }
    if (shell > 0) {
        around[static_cast<std::size_t>(Side::Down)] =
            VolumeCell::from(cell.column(), level, shell - 1);
    }
    if (shell + 1 < (1U << level)) {
        around[static_cast<std::size_t>(Side::Up)] =
            VolumeCell::from(cell.column(), level, shell + 1);
    }
    return around;
}

Side side_toward(VolumeCell neighbor, VolumeCell cell) {
    const auto around = face_neighbors(neighbor);
    const auto side = std::ranges::distance(around.begin(), std::ranges::find(around, cell));
    HOG_ASSERT(side < side_count, "not face neighbours of the same size");
    return static_cast<Side>(side);
}

} // namespace hog::planet

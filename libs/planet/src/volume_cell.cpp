#include <hog/planet/projection.hpp>
#include <hog/planet/volume_cell.hpp>

#include <cmath>

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

} // namespace hog::planet

#pragma once

#include <hog/core/assert.hpp>
#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>

#include <array>
#include <bit>
#include <cstdint>

namespace hog::planet {

// The volume reaches half its span below the planet's radius, through the lithosphere, and as far
// above, past the weather. Shells halve it at each level; a power of two keeps every shell's
// bounds exact.
inline constexpr double volume_span = 262'144.0; // metres
inline constexpr int max_shell_level = 22;       // shells a sixteenth of a metre thick

// One cell of the volume: a column of the cube-sphere quadtree and a shell along it. Columns split
// across into four and shells outward into two, each on its own, so cells can be flat or tall.
//
// The shell is packed like a CellId's position, (shell << 1 | 1) shifted up to the finest level,
// so a cell's descendants lie in one range and, sorted, a column's cells run from the bottom up.
class VolumeCell {
public:
    constexpr VolumeCell() noexcept = default;

    // Shells are counted from the bottom of the volume.
    [[nodiscard]] static constexpr VolumeCell from(CellId column, int shell_level,
                                                   std::uint32_t shell) {
        HOG_ASSERT(column.is_valid(), "invalid column");
        HOG_ASSERT(shell_level >= 0 && shell_level <= max_shell_level, "shell level out of range");
        HOG_ASSERT(shell >> shell_level == 0, "shell out of range for the level");
        return VolumeCell{column, ((shell << 1) | 1U) << (max_shell_level - shell_level)};
    }

    // The whole volume over a column.
    [[nodiscard]] static constexpr VolumeCell from(CellId column) { return from(column, 0, 0); }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return column_.is_valid() && radial_ != 0 && radial_ >> (max_shell_level + 1) == 0;
    }

    [[nodiscard]] constexpr CellId column() const noexcept { return column_; }
    [[nodiscard]] constexpr int shell_level() const {
        HOG_ASSERT(is_valid(), "invalid cell");
        return max_shell_level - std::countr_zero(radial_);
    }
    [[nodiscard]] constexpr std::uint32_t shell() const {
        HOG_ASSERT(is_valid(), "invalid cell");
        return radial_ >> (std::countr_zero(radial_) + 1);
    }

    [[nodiscard]] constexpr VolumeCell across_parent() const {
        return VolumeCell{column_.parent(), radial_};
    }
    [[nodiscard]] constexpr VolumeCell shell_parent() const {
        HOG_ASSERT(shell_level() > 0, "the whole volume's shell has no parent");
        const std::uint32_t marker = lowest_bit() << 1;
        return VolumeCell{column_, (radial_ & ~(marker - 1)) | marker};
    }
    // Children across are numbered in the column's curve order; outward, 0 is the lower.
    [[nodiscard]] constexpr VolumeCell across_child(int index) const {
        return VolumeCell{column_.child(index), radial_};
    }
    [[nodiscard]] constexpr VolumeCell shell_child(int index) const {
        HOG_ASSERT(shell_level() < max_shell_level, "the finest shell has no children");
        HOG_ASSERT(index == 0 || index == 1, "child index out of range");
        const std::uint32_t half = lowest_bit() >> 1;
        return VolumeCell{column_, index == 0 ? radial_ - half : radial_ + half};
    }

    [[nodiscard]] constexpr bool contains(VolumeCell other) const noexcept {
        const std::uint32_t reach = lowest_bit() - 1;
        return column_.contains(other.column_) && other.radial_ >= radial_ - reach &&
               other.radial_ <= radial_ + reach;
    }

    friend constexpr auto operator<=>(VolumeCell, VolumeCell) noexcept = default;

private:
    constexpr VolumeCell(CellId column, std::uint32_t radial) noexcept
        : column_(column), radial_(radial) {}

    [[nodiscard]] constexpr std::uint32_t lowest_bit() const noexcept {
        return radial_ & (~radial_ + 1);
    }

    CellId column_;
    std::uint32_t radial_ = 0;
};

// Metres from the planet's centre to a cell's lower and upper faces.
struct ShellBounds {
    double inner = 0.0;
    double outer = 0.0;
};
[[nodiscard]] ShellBounds shell_bounds(VolumeCell cell, double radius);

// Midway along the cell's column, between its lower and upper faces.
[[nodiscard]] WorldPosition volume_center(VolumeCell cell, double radius);

// The cell of the given levels holding a position, or an invalid cell if it's outside the volume.
[[nodiscard]] VolumeCell volume_cell_at(WorldPosition position, double radius, int level,
                                        int shell_level);

// A cell's sides: across its column as edge_neighbors orders them, then down and up.
enum class Side : std::uint8_t { MinusU, PlusU, MinusV, PlusV, Down, Up };
inline constexpr int side_count = 6;

// The cells of the same size across each side, in Side order; an invalid one below the volume's
// bottom or above its top.
[[nodiscard]] std::array<VolumeCell, side_count> face_neighbors(VolumeCell cell);

// Which of `neighbor`'s sides touches `cell`. Across a cube face's edge u and v turn, so it isn't
// always the opposite of the side `neighbor` lies on.
// Precondition: they're face neighbours of the same size.
[[nodiscard]] Side side_toward(VolumeCell neighbor, VolumeCell cell);

} // namespace hog::planet

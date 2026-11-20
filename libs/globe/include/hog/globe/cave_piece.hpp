#pragma once

#include <hog/core/math.hpp>

#include <optional>
#include <utility>

namespace hog::globe {

// A piece of a cave's open space, in metres from the centre: a stretch of passage, the capsule
// round the line from `from` to `to`, or a chamber, the ball round a point, both ends the same.
struct CavePiece {
    DVec3 from{0.0};
    DVec3 to{0.0};
    double radius = 0.0;

    // Metres from `point` to its line, and to its open space at `width` across the line, negative
    // inside.
    [[nodiscard]] double to_line(DVec3 point) const noexcept;
    [[nodiscard]] double distance(DVec3 point, double width) const noexcept {
        return to_line(point) - width;
    }
    // The radii from the centre between which the line through it along the unit `direction`
    // may come within `reach` of its line; none where it can't.
    [[nodiscard]] std::optional<std::pair<double, double>> along(DVec3 direction,
                                                                 double reach) const noexcept;

    friend bool operator==(const CavePiece&, const CavePiece&) noexcept = default;
};

// A cave's width as detail `lod` shows it: narrower than the detail, it closes up smoothly.
[[nodiscard]] double shown_radius(double radius, double lod) noexcept;

} // namespace hog::globe

#pragma once

#include <hog/core/math.hpp>

namespace hog::viewer {

// The part of the flat map on show, in map coordinates: 0 to 1 across and down.
struct MapRect {
    DVec2 low{0.0, 0.0};
    DVec2 high{1.0, 1.0};

    friend bool operator==(const MapRect&, const MapRect&) = default;
};

// The map point under a point of the shown area, which also runs 0 to 1 across and down.
[[nodiscard]] DVec2 map_point(const MapRect& shown, DVec2 at) noexcept;
// The other way: where a map point is in the shown area, outside 0 to 1 when it's off screen.
[[nodiscard]] DVec2 shown_point(const MapRect& shown, DVec2 point) noexcept;

// The map point under the centre of a pixel of an image of the shown area.
[[nodiscard]] DVec2 pixel_point(const MapRect& shown, int width, int height, int x, int y) noexcept;
// The unit direction from the planet's centre under a map point: latitude down, longitude across.
[[nodiscard]] DVec3 map_direction(DVec2 point) noexcept;

// Zooming and panning, never past the edges of the map.
class MapView {
public:
    static constexpr double most_zoom = 64.0;

    [[nodiscard]] double zoom() const noexcept { return zoom_; }
    [[nodiscard]] MapRect shown() const noexcept;

    // Zooms in by `factor` (out, below 1), keeping the map point under `at` in place.
    void zoom_by(double factor, DVec2 at) noexcept;
    // Moves by a share of the shown area; positive moves the map right and down under the eye.
    void pan(DVec2 by) noexcept;
    void reset() noexcept { *this = MapView{}; }

private:
    // Keeps the shown area on the map.
    void hold() noexcept;

    double zoom_ = 1.0;
    DVec2 centre_{0.5, 0.5};
};

} // namespace hog::viewer

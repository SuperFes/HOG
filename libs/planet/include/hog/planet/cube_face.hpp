#pragma once

#include <hog/core/assert.hpp>
#include <hog/planet/cell_id.hpp>

namespace hog::planet {

// A signed coordinate axis: index 0, 1 or 2 for x, y or z; sign +1 or -1.
struct Axis {
    int index = 0;
    int sign = 1;

    friend constexpr bool operator==(Axis, Axis) = default;
};

// Faces 0-2 face +x, +y, +z and faces 3-5 face -x, -y, -z. On each face, i runs along u and j
// along v, and (u, v, normal) is right-handed, so cells wind counter-clockwise seen from outside.
struct FaceFrame {
    Axis normal;
    Axis u;
    Axis v;
};

[[nodiscard]] constexpr FaceFrame face_frame(int face) {
    HOG_ASSERT(face >= 0 && face < face_count, "face out of range");
    const int axis = face % 3;
    const int next = (axis + 1) % 3;
    const int after = (axis + 2) % 3;
    if (face < 3) {
        return {.normal = {.index = axis, .sign = 1},
                .u = {.index = next, .sign = 1},
                .v = {.index = after, .sign = 1}};
    }
    return {.normal = {.index = axis, .sign = -1},
            .u = {.index = after, .sign = 1},
            .v = {.index = next, .sign = 1}};
}

[[nodiscard]] constexpr int face_from_normal(Axis normal) {
    HOG_ASSERT(normal.index >= 0 && normal.index < 3, "axis out of range");
    HOG_ASSERT(normal.sign == 1 || normal.sign == -1, "axis sign must be +1 or -1");
    return normal.sign > 0 ? normal.index : normal.index + 3;
}

} // namespace hog::planet

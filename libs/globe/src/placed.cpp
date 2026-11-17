#include <hog/globe/placed.hpp>

#include <algorithm>

namespace hog::globe {

double Placed::distance(DVec3 point) const noexcept {
    const DVec3 offset = point - middle;
    const DVec3 local{glm::dot(offset, faces[0]), glm::dot(offset, faces[1]),
                      glm::dot(offset, faces[2])};
    // A box `rounding` smaller every way, grown by `rounding` all round.
    const DVec3 beyond = glm::abs(local) - (half - rounding);
    const double outside = glm::length(glm::max(beyond, DVec3{0.0}));
    const double inside = std::min(std::max({beyond.x, beyond.y, beyond.z}), 0.0);
    return outside + inside - rounding;
}

Placed placed_rock(const Rock& rock) noexcept {
    return {.kind = Placed::Kind::Rock,
            .key = rock.key,
            .material = rock.material,
            .middle = rock.middle,
            .faces = {rock.across, glm::cross(rock.up, rock.across), rock.up},
            .half = rock.axes,
            .rounding = rock.edge};
}

} // namespace hog::globe

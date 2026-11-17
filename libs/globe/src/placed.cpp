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

Placed placed_crag(const Crag& crag) noexcept {
    return {.kind = Placed::Kind::Crag,
            .key = crag.key,
            .material = crag.material,
            .middle = crag.middle,
            .faces = crag.faces,
            .half = crag.half,
            .rounding = crag.rounding};
}

Placed placed_vent(const VentSite& site) noexcept {
    return {.kind = site.pool ? Placed::Kind::Pool : Placed::Kind::Vent,
            .key = site.key,
            .material = Material::Air,
            .middle = site.direction * site.ground,
            .faces = {},
            .half = DVec3{0.0},
            .rounding = 0.0};
}

} // namespace hog::globe

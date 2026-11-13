#include <hog/client/sonar.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>

namespace hog::client {

std::vector<Blip> blips(std::span<const globe::Pocket> pockets, const render::Camera& camera,
                        const terrain::Bend& bend, DVec2 size, double margin) {
    const glm::dmat4 clip_from_eye{render::view_projection(camera, size.x / size.y)};
    std::vector<Blip> found;
    for (const globe::Pocket& pocket : pockets.first(std::min(pockets.size(), ping_shown))) {
        const DVec3 from_eye = pocket.at - camera.eye.metres();
        const double distance = glm::length(from_eye);
        // Up is positive: the pocket's depth under the eye reads as "down".
        const double rise = glm::length(pocket.at) - glm::length(camera.eye.metres());
        const glm::dvec4 clip = clip_from_eye * glm::dvec4{terrain::bent(bend, from_eye), 1.0};
        // Where it lies on the picture; behind the eye only the way it lies from the middle is
        // known, the side the eye should turn to.
        DVec2 ndc = clip.w > 0.0 ? DVec2{clip.x, clip.y} / clip.w : DVec2{clip.x, clip.y};
        const bool behind = clip.w <= 0.0;
        const bool inside = !behind && std::abs(ndc.x) <= 1.0 && std::abs(ndc.y) <= 1.0;
        std::string arrow;
        if (!inside) {
            if (behind && glm::length(ndc) < 1e-9) {
                ndc = {0.0, -1.0};
            }
            const double outward = std::max(std::abs(ndc.x), std::abs(ndc.y));
            ndc /= outward;
            // Held in from the edge by the margin.
            ndc.x *= 1.0 - (2.0 * margin / size.x);
            ndc.y *= 1.0 - (2.0 * margin / size.y);
            if (std::abs(ndc.x) * size.x > std::abs(ndc.y) * size.y) {
                arrow = ndc.x < 0.0 ? "<- " : "-> ";
            } else {
                arrow = ndc.y < 0.0 ? "^ " : "v ";
            }
        }
        const DVec2 drawn = ((DVec2{ndc.x, ndc.y}) + 1.0) / 2.0;
        found.push_back({.text = std::format("{}cave {:.0f} m, {:.0f} m {}", arrow, distance,
                                             std::abs(rise), rise < 0.0 ? "down" : "up"),
                         .at = drawn * size,
                         .on_screen = inside,
                         .distance = distance});
    }
    std::ranges::sort(found, std::ranges::greater{}, &Blip::distance);
    return found;
}

} // namespace hog::client

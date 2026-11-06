#include <hog/flow/creep.hpp>

#include <algorithm>
#include <cmath>

namespace hog::flow {

double Creep::total() const noexcept {
    double sum = 0.0;
    for (const Node& node : nodes_) {
        sum += node.cover;
    }
    return sum;
}

double Creep::step(double years) {
    constexpr std::size_t faces = 2;
    const std::size_t count = nodes_.size();
    // Flux through each column's +x and +y faces, positive towards that side.
    std::vector<std::array<double, faces>> flux(count);
    double stiffest = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        for (std::size_t f = 0; f < faces; ++f) {
            const int other = nodes_[n].next[f];
            if (other == none) {
                continue;
            }
            const auto o = static_cast<std::size_t>(other);
            const double drop = (surface(n) - surface(o)) / law_.cell;
            // The column the cover leaves supplies it, so a bare one gives none.
            const double donor = nodes_[drop > 0.0 ? n : o].cover;
            if (donor <= 0.0) {
                continue;
            }
            // Slope over the steepest cover holds: flux grows without end as it nears it.
            const double ratio = std::min(std::abs(drop) / law_.steepest, 0.99);
            const double stiffness = law_.diffusivity / (1.0 - (ratio * ratio));
            stiffest = std::max(stiffest, stiffness);
            flux[n][f] = stiffness * drop;
        }
    }
    const double dt = std::min(
        years, stiffest > 0.0 ? 0.2 * law_.cell * law_.cell / stiffest : years);
    for (std::size_t n = 0; n < count; ++n) {
        for (std::size_t f = 0; f < faces; ++f) {
            const int other = nodes_[n].next[f];
            if (other == none) {
                continue;
            }
            const std::size_t o = static_cast<std::size_t>(other);
            const double donor = nodes_[flux[n][f] > 0.0 ? n : o].cover;
            const double gap = std::abs(surface(n) - surface(o));
            const double most = law_.share * std::min(donor, gap) * law_.cell / dt;
            flux[n][f] = std::clamp(flux[n][f], -most, most);
        }
    }
    for (std::size_t n = 0; n < count; ++n) {
        Node& node = nodes_[n];
        double net = 0.0;
        for (std::size_t f = 0; f < faces; ++f) {
            const int before = node.next[f + faces];
            if (before != none) {
                net += flux[static_cast<std::size_t>(before)][f];
            }
            net -= flux[n][f];
        }
        node.cover = std::max(node.cover + (dt * net / law_.cell), 0.0);
    }
    return dt;
}

void Creep::weather(double years) {
    for (Node& node : nodes_) {
        node.cover += law_.production * years * std::exp(-node.cover / law_.deep);
    }
}

} // namespace hog::flow

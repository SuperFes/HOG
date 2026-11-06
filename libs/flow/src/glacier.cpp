#include <hog/flow/glacier.hpp>

#include <algorithm>
#include <cmath>

namespace hog::flow {

double Glacier::total() const noexcept {
    double sum = 0.0;
    for (const Node& node : nodes_) {
        sum += node.ice;
    }
    return sum;
}

std::vector<Glacier::Hang> Glacier::hangs(double least) const {
    std::vector<Hang> found;
    for (std::size_t n = 0; n < nodes_.size(); ++n) {
        if (nodes_[n].ice <= 0.0) {
            continue;
        }
        for (std::size_t side = 0; side < sides; ++side) {
            const int other = nodes_[n].next[side];
            if (other == none || nodes_[static_cast<std::size_t>(other)].ice > 0.0) {
                continue;
            }
            const double drop = surface(n) - surface(static_cast<std::size_t>(other));
            if (drop >= least) {
                found.push_back({.node = n, .side = side, .drop = drop});
            }
        }
    }
    return found;
}

double Glacier::step(double years) {
    constexpr std::size_t faces = 2;
    const std::size_t count = nodes_.size();
    // Flux through each column's +x and +y faces, positive towards that side, and debris with it.
    std::vector<std::array<double, faces>> flux(count);
    std::vector<std::array<double, faces>> carried(count);
    double stiffest = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        for (std::size_t f = 0; f < faces; ++f) {
            const int other = nodes_[n].next[f];
            if (other == none) {
                continue;
            }
            const auto o = static_cast<std::size_t>(other);
            const double drop = (surface(n) - surface(o)) / law_.cell;
            // The column ice leaves supplies the flux, so a bare one gives none.
            const double thick = nodes_[drop > 0.0 ? n : o].ice;
            const double stiffness = law_.flow * std::pow(thick, 5.0) * drop * drop;
            stiffest = std::max(stiffest, stiffness);
            flux[n][f] = stiffness * drop;
        }
    }
    const double dt = std::min(
        years, std::max(stiffest > 0.0 ? 0.2 * law_.cell / stiffest : years, law_.shortest));
    for (std::size_t n = 0; n < count; ++n) {
        for (std::size_t f = 0; f < faces; ++f) {
            const int other = nodes_[n].next[f];
            if (other == none) {
                continue;
            }
            const Node& donor = nodes_[flux[n][f] > 0.0 ? n : static_cast<std::size_t>(other)];
            const double most = law_.share * donor.ice * law_.cell / dt;
            flux[n][f] = std::clamp(flux[n][f], -most, most);
            carried[n][f] = donor.ice > 0.0 ? flux[n][f] * donor.debris / donor.ice : 0.0;
        }
    }
    for (std::size_t n = 0; n < count; ++n) {
        Node& node = nodes_[n];
        double in = 0.0;
        double debris_in = 0.0;
        double out = 0.0;
        double debris_out = 0.0;
        double worn = 0.0;
        for (std::size_t f = 0; f < faces; ++f) {
            // The face from the -x or -y side comes in here, the other goes out.
            const int before = node.next[f + faces];
            if (before != none) {
                const auto b = static_cast<std::size_t>(before);
                in += flux[b][f];
                debris_in += carried[b][f];
                worn += std::abs(flux[b][f]);
            }
            out += flux[n][f];
            debris_out += carried[n][f];
        }
        worn += std::abs(out);
        node.ice = std::max(node.ice + (dt * (in - out) / law_.cell), 0.0);
        node.debris = std::max(node.debris + (dt * (debris_in - debris_out) / law_.cell), 0.0);
        node.rock -= node.ice > 0.0 ? law_.wear * dt * worn : 0.0;
    }
    return dt;
}

} // namespace hog::flow

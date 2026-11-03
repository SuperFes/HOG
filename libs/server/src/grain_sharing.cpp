#include <hog/server/grain_sharing.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace hog::server {

protocol::Grains GrainSharing::near(std::span<const flow::Parcel> parcels, WorldPosition at,
                                    std::uint32_t tick) {
    struct Seen {
        double distance = 0.0;
        const flow::Parcel* parcel = nullptr;
    };
    std::vector<Seen> seen;
    for (const flow::Parcel& parcel : parcels) {
        const double distance = glm::length(parcel.at - at.metres());
        if (distance <= reach) {
            seen.push_back({.distance = distance, .parcel = &parcel});
        }
    }
    if (seen.size() > protocol::most_grains) {
        std::ranges::nth_element(seen, seen.begin() + protocol::most_grains,
                                 [](const Seen& a, const Seen& b) { return a.distance < b.distance; });
        seen.resize(protocol::most_grains);
    }
    std::ranges::sort(seen,
                      [](const Seen& a, const Seen& b) { return a.parcel->id < b.parcel->id; });

    protocol::Grains grains{.tick = tick, .flying = {}};
    grains.flying.reserve(seen.size());
    for (const Seen& one : seen) {
        const flow::Parcel& parcel = *one.parcel;
        protocol::Flying flying{.id = protocol::NetId{parcel.id},
                                .at = protocol::to_millimetres(WorldPosition{parcel.at})};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            flying.velocity[axis] = static_cast<std::int16_t>(
                std::clamp(std::lround(parcel.velocity[axis] * 100.0), -32'768L,
                           32'767L));
        }
        flying.material = static_cast<std::uint8_t>(parcel.material);
        flying.amount = static_cast<std::uint32_t>(std::min<std::int64_t>(parcel.amount, 0xFFFF'FFFF));
        grains.flying.push_back(flying);
    }
    return grains;
}

} // namespace hog::server

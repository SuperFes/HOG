#pragma once

#include <hog/core/math.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/elevation.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hog::terrain {

// The depth of the sea round the eye, which shapes the waves near a shore: grids of it in layers,
// each the same size and coarser than the last, laid on the plane touching the planet at an anchor
// fixed to the world, so they only change when the eye moves far enough to be nearer another.
inline constexpr int shore_size = 256;                           // texels a side
inline constexpr std::array<double, 2> shore_spacing{8.0, 64.0}; // metres between texels
// Anchors lie at the centres of cells about this wide.
inline constexpr double shore_anchoring = 512.0; // metres
// Depths are kept from as high as this above the sea, which is only told apart from land.
inline constexpr float shore_highest = -10.0F; // metres
// The sea rises no higher than this over its mean, with the tide in and the waves running up the
// shore.
inline constexpr float shore_reach = 5.0F; // metres

struct ShoreFrame {
    DVec3 anchor{0.0, 0.0, 1.0}; // unit, from the planet's centre
    DVec3 east{1.0, 0.0, 0.0};   // unit, along the plane
    DVec3 north{0.0, 1.0, 0.0};

    friend bool operator==(const ShoreFrame&, const ShoreFrame&) = default;
};

// The frame anchored nearest the unit direction `eye` on a planet of `radius`.
[[nodiscard]] ShoreFrame shore_frame(DVec3 eye, double radius);

// The unit direction to the point `along` metres east and north of the anchor, on its plane.
[[nodiscard]] DVec3 shore_direction(const ShoreFrame& frame, double radius, DVec2 along);

// Metres east and north on the frame's plane of where the unit direction `direction` meets it.
[[nodiscard]] DVec2 shore_along(const ShoreFrame& frame, double radius, DVec3 direction);

// Metres of sea at each texel's centre of a layer, rows from the south and texels from the west:
// positive under the sea, as high as shore_highest over land.
[[nodiscard]] std::vector<float> shore_depths(const planet::Elevation& terrain,
                                              const ShoreFrame& frame, double radius,
                                              double spacing);

// Of depths in layers as ShoreStream works them out, keeps as high as shore_highest whatever the
// open sea can't reach: ground behind higher ground, and hollows and water cut off by it, so the
// tide and the waves' run-up flood none of them. The open sea is what's deep, or what's at the
// coarsest layer's edge; each finer layer's is where the next coarser one's reaches.
void keep_to_sea_reach(std::span<float> depths);

// The depths round an anchor, each layer in turn, finest first.
struct ShoreDepths {
    ShoreFrame frame;
    std::vector<float> depths;
};

// Works out the depths round the eye in the background on the pool, anew whenever the eye comes
// nearer another anchor, for the thread that made it. One set at a time is in flight; the terrain
// must outlive it.
class ShoreStream {
public:
    ShoreStream(const planet::Elevation& terrain, double radius, jobs::JobSystem& jobs) noexcept
        : terrain_(&terrain), jobs_(&jobs), radius_(radius) {}

    // Wants the depths round the unit direction `eye`.
    void follow(DVec3 eye);
    [[nodiscard]] bool in_flight() const noexcept { return working_ != nullptr; }
    // The depths worked out since the last call, if any.
    [[nodiscard]] std::optional<ShoreDepths> collect();
    // Waits for those in flight, and for those wanted after them, and hands over the last.
    [[nodiscard]] std::optional<ShoreDepths> finish();

private:
    void start(const ShoreFrame& frame);

    const planet::Elevation* terrain_;
    jobs::JobSystem* jobs_;
    double radius_;
    std::optional<ShoreFrame> wanted_;
    // What's in flight was started for, or what was last handed over.
    std::optional<ShoreFrame> started_;
    std::unique_ptr<jobs::Async<ShoreDepths>> working_;
};

} // namespace hog::terrain

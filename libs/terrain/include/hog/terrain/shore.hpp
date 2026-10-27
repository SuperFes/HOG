#pragma once

#include <hog/core/math.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/world.hpp>

#include <array>
#include <cstdint>
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

// The ground at each texel's centre of a layer, rows from the south and texels from the west, in
// metres above sea level, as changed; the level of the lake over it, or sea level where there's
// none; and whether the sea stands over it. What stands over the ground is told from the ground as
// generated, so ground dug below the water lets it in only where the sea reaches through.
struct ShoreGround {
    std::vector<float> elevations;
    std::vector<float> lakes;
    std::vector<std::uint8_t> sea;
};

[[nodiscard]] ShoreGround shore_ground(const planet::World& world, const ShoreFrame& frame,
                                       double radius, double spacing);

// Metres of sea at each texel of `ground`: positive under the sea, as high as shore_highest over
// land and lakes' beds, which the sea never floods.
[[nodiscard]] std::vector<float> shore_depths(const ShoreGround& ground);

// Of depths in layers as ShoreStream works them out, and where the sea stands over them, `sea`,
// keeps as high as shore_highest whatever the open sea can't reach: ground behind higher ground,
// and hollows and water cut off by it, so the tide and the waves' run-up flood none of them. The
// open sea is the sea where it's deep or at the coarsest layer's edge; each finer layer's is where
// the next coarser one's reaches. Returns the depths of the sea it cut off, still water, as high
// as shore_highest elsewhere: low ground the sea neither stands over nor reaches lies dry.
std::vector<float> keep_to_sea_reach(std::span<float> depths, std::span<const std::uint8_t> sea);

// A lake's level is kept over the ground round it out to this many texels from its shore, as far
// as the corners of the water's surface lie apart where a layer is drawn, so none of its surface
// drawn towards them sinks below its level.
inline constexpr int lake_margin = 24;

// Of layers of ground as shore_ground has them, and of still water as keep_to_sea_reach keeps it,
// `still`: puts each lake's water into `still`, as deep as it lies under the lake's level, and
// over the ground lake_margin finds it kept over, as deep as that ground stands above it, negative,
// or where it's lower, as dry.
// Returns the level of the water each texel is kept to: the lake's, or sea level.
std::vector<float> keep_lakes(std::span<const float> elevations, std::span<const float> lakes,
                              std::span<float> still);

// Of layers of ground as shore_ground has them, of depths and still water as keep_to_sea_reach
// keeps them, and of the levels keep_lakes keeps, `levels`: keeps the level of low ground that
// neither the sea nor any still water covers as far under its ground as depths are kept above it,
// so water drawn at that level is hidden under the ground, however the texels between are blended.
void keep_dry(std::span<const float> elevations, std::span<const float> depths,
              std::span<const float> still, std::span<float> levels);

// Fetch, how far water stretches upwind, is measured for winds blowing towards each of these
// headings in turn: east first, then each an eighth of a turn anticlockwise.
inline constexpr int fetch_headings = 8;
// Water stretching beyond what's known is taken as this far, the furthest kept.
inline constexpr float longest_fetch = 50'000.0F; // metres

// Of layers of depths of the open sea, `depths`, and of still water, `still`, as ShoreDepths has
// them: metres of either kind of water upwind of each texel, none over land. Layer by layer, a
// plane of texels for each heading in turn; each finer layer's fetch runs on into the next
// coarser one's.
[[nodiscard]] std::vector<float> shore_fetches(std::span<const float> depths,
                                               std::span<const float> still);

// The depths round an anchor, each layer in turn, finest first: of the sea where the open sea
// reaches, of still water where it doesn't, the level still water is kept to, as keep_lakes and
// keep_dry have it, and the fetch over all of it, as shore_fetches has it.
struct ShoreDepths {
    ShoreFrame frame;
    std::vector<float> depths;
    std::vector<float> still;
    std::vector<float> levels;
    std::vector<float> fetches;
};

// The levels of the lakes in the finest layer of `shore`, lowest first, each once.
[[nodiscard]] std::vector<float> lake_levels(const ShoreDepths& shore);

// Of `levels` as lake_levels has them, the highest no higher than `height` metres above sea level;
// or sea level, if none is.
[[nodiscard]] double water_below(std::span<const float> levels, double height);

// Works out the depths round the anchor of `frame`.
[[nodiscard]] ShoreDepths work_out_shore(const planet::World& world, const ShoreFrame& frame,
                                         double radius);

// Works out the depths round the eye in the background on the pool, anew whenever the eye comes
// nearer another anchor, for the thread that made it. One set at a time is in flight; the world
// must outlive it, unchanged while one is.
class ShoreStream {
public:
    ShoreStream(const planet::World& world, double radius, jobs::JobSystem& jobs) noexcept
        : world_(&world), jobs_(&jobs), radius_(radius) {}

    // Wants the depths round the unit direction `eye`.
    void follow(DVec3 eye);
    [[nodiscard]] bool in_flight() const noexcept { return working_ != nullptr; }
    // The depths worked out since the last call, if any.
    [[nodiscard]] std::optional<ShoreDepths> collect();
    // Waits for those in flight, and for those wanted after them, and hands over the last.
    [[nodiscard]] std::optional<ShoreDepths> finish();

private:
    void start(const ShoreFrame& frame);

    const planet::World* world_;
    jobs::JobSystem* jobs_;
    double radius_;
    std::optional<ShoreFrame> wanted_;
    // What's in flight was started for, or what was last handed over.
    std::optional<ShoreFrame> started_;
    std::unique_ptr<jobs::Async<ShoreDepths>> working_;
};

} // namespace hog::terrain

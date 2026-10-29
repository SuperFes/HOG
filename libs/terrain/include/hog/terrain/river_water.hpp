#pragma once

#include <hog/core/math.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/rivers.hpp>
#include <hog/planet/world.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::terrain {

// Rivers' water is laid out this far round the eye, and anew once the eye has gone this far from
// where it last was.
inline constexpr double river_reach = 30'000.0; // metres
inline constexpr double river_relay = 1'000.0;  // metres
// The water is laid out over its banks either side by the larger of this and half its width,
// for drawing to show as much of it as ground drawn coarsely needs.
inline constexpr double river_overlap = 4.0; // metres
// Pieces along a river are about as long as it's wide, but no shorter than this, nor than this
// share of how far they lie from where the water's laid round, so far rivers take few.
inline constexpr double river_piece = 4.0;         // metres
inline constexpr double river_piece_angle = 0.004; // radians
// The water's origin lies on a lattice this many metres apart along each axis, so a pattern drawn
// from it that repeats as often stays put however the water's laid out anew.
inline constexpr double river_pattern = 256.0; // metres

// A point of a river's surface.
struct RiverVertex {
    Vec3 position{0.0F}; // metres from the water's origin
    // Metres out past the water's nearer edge, over its bank; negative within it.
    float beyond = 0.0F;
    Vec3 flow{0.0F}; // m/s, in the planet's frame
    // From 0 where the water runs smoothly to 1 where it runs as steeply as it falls.
    float rough = 0.0F;
    // Which way across the river it lies from the middle, in the planet's frame, none in the
    // middle; and how many metres wide half the river runs there, so drawing can widen it.
    Vec3 outward{0.0F};
    float half = 0.0F;
    // From 0 in its channel to 1 where it's mixed into the sea or lake it runs into.
    float mixed = 0.0F;

    friend bool operator==(const RiverVertex&, const RiverVertex&) = default;
};

static_assert(sizeof(RiverVertex) == 52);

// Past the shore, a river's water mixes into the sea or lake it runs into over so many of its
// widths, fanning out by so many times its width more as it does.
constexpr double river_mixing = 4.0;
constexpr double river_fan = 1.0;

// The surface of the rivers round a point, in strips of triangles, each three indices.
struct RiverWater {
    WorldPosition origin;
    std::vector<RiverVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// How fast a reach's water runs, m/s, as its flow over the channel it fills.
[[nodiscard]] double river_speed(const planet::Reach& reach);
// How rough a reach's water runs on a planet of `radius`, as RiverVertex::rough has it.
[[nodiscard]] double river_rough(const planet::Reach& reach, double radius);

// How many pieces a reach's water is laid in along it on a planet of `radius`, its middle `away`
// metres from where the water's laid round.
[[nodiscard]] int river_pieces(const planet::Reach& reach, double radius, double away);

// The rivers' surface within river_reach of the unit direction `around` on a planet of `radius`,
// where it lies over land, as the world's terrain has it: each reach's course, shifted as the
// meanders shift the ground, at the level of its water there.
[[nodiscard]] RiverWater river_water(const planet::World& world, DVec3 around, double radius);

// Lays the rivers' surface out round the eye in the background on the pool, anew whenever the eye
// has gone river_relay from where it was last laid round, for the thread that made it. One is in
// flight at a time; the world must outlive it, unchanged while one is.
class RiverStream {
public:
    RiverStream(const planet::World& world, double radius, jobs::JobSystem& jobs) noexcept
        : world_(&world), jobs_(&jobs), radius_(radius) {}

    // Wants the water round the unit direction `eye`.
    void follow(DVec3 eye);
    [[nodiscard]] bool in_flight() const noexcept { return working_ != nullptr; }
    // The water laid out since the last call, if any.
    [[nodiscard]] std::optional<RiverWater> collect();
    // Waits for what's in flight, and for what's wanted after it, and hands over the last.
    [[nodiscard]] std::optional<RiverWater> finish();

private:
    void start(DVec3 around);

    const planet::World* world_;
    jobs::JobSystem* jobs_;
    double radius_;
    std::optional<DVec3> wanted_;
    // What's in flight was started round, or what was last handed over.
    std::optional<DVec3> started_;
    std::unique_ptr<jobs::Async<RiverWater>> working_;
};

} // namespace hog::terrain

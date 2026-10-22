#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/drifting_grid.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::render {

class Device;
struct SeaBindings;

// Ripples round the eye are kept on a DriftingGrid of ripple_size texels a side, ripple_spacing
// metres apart. As shaders/ripples.slang has them.
inline constexpr std::uint32_t ripple_size = 512;
inline constexpr double ripple_spacing = 0.125; // metres
// They're stepped on no more than so many seconds at a time, but over no more than so many steps
// a frame; a longer frame takes longer steps.
inline constexpr double ripple_step = 1.0 / 30.0;
inline constexpr int ripple_most_steps = 4;
// Within so many texels of the grid's edge, ripples are soaked up, as fast as this at the very
// edge, 1/s, so they run off the grid rather than back onto it from the other side.
inline constexpr int ripple_edge = 48;
inline constexpr double ripple_soaking = 6.0;
// Water shallower than this, metres, doesn't ripple; ripples stop at the shore.
inline constexpr double ripple_shallowest = 0.05;
// A body crossing the surface pushes the water down by so much, metres, round where it crosses
// it, as though pressing on it; only so many bodies do so at once.
inline constexpr double ripple_push = 0.1;
inline constexpr std::size_t ripple_most_bodies = 16;

// A body as a capsule standing on its feet, laid on the ripples' grid.
struct RippleBody {
    // Under the feet, texels from the grid's first's corner.
    glm::vec2 feet{0.0F};
    // Metres the feet stand over sea level.
    float above = 0.0F;
    float radius = 0.3F; // metres
    // From the feet to the top, metres along the grid's rows, its columns and up.
    glm::vec3 tall{0.0F, 0.0F, 1.8F};
};

// The ripples on the water round the eye, which run as waves on deep water do, each as fast as
// its length has it: how high the water stands on each texel of the grid, metres, and how fast it
// rises there, as the potential the water at the surface moves by, m²/s.
class RippleMap {
public:
    RippleMap() = default;
    // With no ripples on it, stepped as any of `frames` frames in flight.
    [[nodiscard]] static Result<RippleMap> create(Device& device, std::uint32_t frames = 1);

    // Records stepping the ripples on as `step` has the grid move, over the sea as `sea` has it,
    // its waves laying the grid as a GridPlacing does, as the frame `frame`; pushed by `bodies`
    // where they cross the surface, those past ripple_most_bodies left out. Leaves them ready for
    // any shader to sample.
    [[nodiscard]] Result<> step(VkCommandBuffer commands, const GridStep& step,
                                const SeaBindings& sea, std::uint32_t frame = 0,
                                std::span<const RippleBody> bodies = {});

    // What's kept from the last step, ready to sample: x how high the water stands, y its
    // potential.
    [[nodiscard]] const Image& kept() const noexcept { return state_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_.handle(); }

private:
    Pipeline rows_;
    Pipeline columns_;
    Pipeline settle_;
    Image state_;
    Image spectrum_;
    TrackedImage state_use_;
    TrackedImage spectrum_use_;
    Sampler sampler_;
    std::vector<Buffer> bodies_;

    void clear(VkCommandBuffer commands);
    void record(VkCommandBuffer commands, glm::ivec2 shift, double seconds, const SeaBindings& sea,
                VkBuffer bodies);
};

} // namespace hog::render

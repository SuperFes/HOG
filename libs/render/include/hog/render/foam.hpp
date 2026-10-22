#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/drifting_grid.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/vulkan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace hog::render {

class Device;
struct SeaBindings;

// The foam left on the sea round the eye is kept on a DriftingGrid of foam_size texels a side,
// foam_spacing metres apart. As shaders/foam.slang has them.
inline constexpr std::uint32_t foam_size = 512;
inline constexpr double foam_spacing = 1.0; // metres
// Foam left fades to 1/e of itself over so many seconds, as long as hog::waves takes it to last;
// in the surf, longer; and stranded on the ground as the swash draws back, longer still.
inline constexpr double foam_lasting = 3.0;
inline constexpr double foam_lasting_surf = 10.0;
inline constexpr double foam_lasting_stranded = 20.0;
// Of the foam on water drawing back off the ground, the share left stranded there.
inline constexpr double foam_stranding = 0.3;
// Where nothing's kept, the foam's caught up over so many steps of so many seconds before now.
inline constexpr int foam_catch_up = 48;
inline constexpr double foam_catch_up_step = 0.25;

// The foam left on the sea round the eye: how much lies on each texel of the grid, fading as it
// ages; how much of what would have left it there the grid saw, which is less where it's only just
// come over it; and how much the water left stranded on the ground there, which stays with the
// ground as the water drifts on, till the water comes back for it.
class FoamMap {
public:
    FoamMap() = default;
    // With no foam on it.
    [[nodiscard]] static Result<FoamMap> create(Device& device);

    // Records stepping the foam on as `step` has the grid move, over the sea as `sea` has it, its
    // waves laying the grid as a GridPlacing does, the water at the surface drifting at `drift`,
    // m/s. Foam is left as fast as the waves break; where nothing's kept, if `catch_up`, as long
    // before as it takes to cover as much as it would, else none is known. Leaves it ready for any
    // shader to sample.
    void step(VkCommandBuffer commands, const GridStep& step, const SeaBindings& sea, DVec3 drift,
              bool catch_up);

    // What's kept from the last step, ready to sample.
    [[nodiscard]] const Image& kept() const noexcept { return images_[latest_]; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_.handle(); }

private:
    Pipeline pipeline_;
    std::array<Image, 2> images_;
    std::array<TrackedImage, 2> uses_;
    Sampler sampler_;
    std::size_t latest_ = 0;

    void record(VkCommandBuffer commands, const GridStep& step, const SeaBindings& sea, DVec3 drift,
                double ago);
};

} // namespace hog::render

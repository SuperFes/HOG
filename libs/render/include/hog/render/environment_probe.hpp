#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::render {

class Atmosphere;
class Device;

// What a probe takes in: the sky as the frame's view has it, seen from its camera's eye. Only
// the camera's eye is taken; each face turns its own way. The ground round the eye isn't drawn
// into it yet.
struct ProbeView {
    SkyView sky;
};

// The cube's faces, as Vulkan orders them, laid out by the ground's frame at the eye: x east,
// y up, z north, which mirrors them as a camera sees. The one looking straight down is never
// drawn.
inline constexpr std::uint32_t probe_faces = 6;
inline constexpr std::uint32_t probe_down = 3;

// The camera drawing a probe's face `face` from `eye`, `up` and `east` the ground's frame there:
// as a cube samples it along probe_direction().
[[nodiscard]] Camera probe_face(std::uint32_t face, WorldPosition eye, DVec3 up, DVec3 east);
// Where a cube laid out by the ground's frame of `up` and `east` holds `direction`.
[[nodiscard]] DVec3 probe_direction(DVec3 direction, DVec3 up, DVec3 east);

// What's out of the water all round a point, drawn into a small cube: the sky. What it shows is
// seen from under the water through the bent light of its surface, which blurs it anyway.
class EnvironmentProbe {
public:
    static constexpr std::uint32_t default_size = 128;

    // Drawn into `colour_format` and `depth_format`, as the sky draws.
    [[nodiscard]] static Result<EnvironmentProbe> create(const Device& device,
                                                         VkFormat colour_format,
                                                         VkFormat depth_format,
                                                         std::uint32_t size = default_size);

    // Records drawing every face but the one looking down: the sky through the air as seen that
    // way. `frame`'s sky tables must have been recorded. Leaves the cube to be sampled.
    void record(VkCommandBuffer commands, std::uint32_t frame, const Atmosphere& atmosphere,
                const Sky& sky, const ProbeView& view) const;

    // A cube, laid out as probe_direction() has it.
    [[nodiscard]] const Image& cube() const noexcept { return colour_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }

private:
    EnvironmentProbe() = default;

    Image colour_;
    Image depth_;
    std::vector<Image> aerial_; // a face each
    std::uint32_t size_ = 0;
};

} // namespace hog::render

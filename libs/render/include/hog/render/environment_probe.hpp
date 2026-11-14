#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/clouds.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hog::render {

class Atmosphere;
class Device;

// What a probe takes in: the sky as the frame's view has it, seen from its camera's eye, and
// whatever else stands round it, which `around` draws over the sky into each face as seen by
// that face's camera; then the clouds `clouds` marches, if given, as `clouds_view` has them but
// seen from each face, through its own air. Only the camera's eye is taken; each face turns its
// own way.
struct ProbeView {
    SkyView sky;
    std::function<Result<>(VkCommandBuffer, const Camera&, VkExtent2D)> around;
    const Clouds* clouds = nullptr;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    CloudsView clouds_view = {};
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

// What's round a point, drawn into a small cube: the sky and what stands under it, for water to
// mirror and to be seen out of from under it; both blur it anyway.
class EnvironmentProbe {
public:
    static constexpr std::uint32_t default_size = 128;

    // Drawn into `colour_format` and `depth_format`, as the sky draws.
    [[nodiscard]] static Result<EnvironmentProbe> create(const Device& device,
                                                         VkFormat colour_format,
                                                         VkFormat depth_format,
                                                         std::uint32_t size = default_size);

    // Records drawing faces but the one looking down: the sky through the air as seen that way,
    // and what stands round the eye. Every face the first time, then one a time in turn, the
    // others kept as last drawn: what's round the eye changes slowly next to how often it's
    // drawn, and blurred as it's read. `frame`'s sky tables must have been recorded. Leaves the
    // cube to be sampled.
    [[nodiscard]] Result<> record(VkCommandBuffer commands, std::uint32_t frame,
                                  const Atmosphere& atmosphere, const Sky& sky,
                                  const ProbeView& view);

    // A cube, laid out as probe_direction() has it, with mips each blurred from the one before,
    // for what's mirrored rough or far to be read soft.
    [[nodiscard]] const Image& cube() const noexcept { return colour_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t levels() const noexcept { return levels_; }

private:
    EnvironmentProbe() = default;
    // The air between the eye and what's seen each way, for the faces `drawn`.
    void record_air(VkCommandBuffer commands, std::uint32_t frame, const Atmosphere& atmosphere,
                    const Sky& sky, const std::array<SkyView, probe_faces>& skies,
                    const std::array<bool, probe_faces>& drawn) const;
    // Blurs each mip from the one before, and leaves the cube to be sampled.
    void shrink(VkCommandBuffer commands) const;
    // Records drawing `face`, seen as `seen` has it: the sky, what stands round, and the clouds.
    Result<> record_face(VkCommandBuffer commands, std::uint32_t frame, std::uint32_t face,
                         const Atmosphere& atmosphere, const Sky& sky, const SkyView& seen,
                         const ProbeView& view);
    // Records marching the clouds as seen from `face` and drawing them over it, its colour and
    // depth drawn and left as targets.
    Result<> record_clouds(VkCommandBuffer commands, std::uint32_t face, const ProbeView& view,
                           const Camera& camera);

    const Device* device_ = nullptr;
    Image colour_;
    Image depth_;
    // What the clouds are marched into, as Clouds::march_once() has them, made as they're first
    // marched.
    Image marched_;
    Image distances_;
    std::vector<Image> aerial_; // a face each
    std::uint32_t size_ = 0;
    std::uint32_t levels_ = 1;
    bool whole_ = false;                  // every face drawn once
    std::uint32_t next_ = 0;              // the face drawn last, or next once all are
    VkFilter shrink_ = VK_FILTER_NEAREST; // linear where the format blits so
};

} // namespace hog::render

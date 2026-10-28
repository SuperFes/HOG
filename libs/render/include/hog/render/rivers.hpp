#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/river_water.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::render {

class Device;

// Where rivers are seen from and lit by.
struct RiverView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
    // What's drawn beneath the water, as copied, to be seen through it; without it, none's drawn.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::optional<SeaBehind> behind = {};
    // Seconds since any epoch, for the water's flow.
    double time = 0.0;
};

// Rivers' water, as terrain::RiverStream lays it out round the eye: lit through the air, it
// reflects the sky and the sun, ripples with its flow and breaks white where it runs rough, over
// what's drawn beneath it, as copied, seen through the water between. It hides what's drawn after
// it below.
class RiverSurface {
public:
    // `frames` in flight, each with its own copy of the water.
    [[nodiscard]] static Result<RiverSurface>
    create(Device& device, VkFormat colour_format, VkFormat depth_format, std::uint32_t frames = 1);

    // The water drawn from now on; each frame copies it as it's next drawn.
    void show(std::shared_ptr<const terrain::RiverWater> water);

    // Records drawing into the colour and depth images being rendered to, of `extent`, over what's
    // drawn there. The frame's last submit must have finished.
    [[nodiscard]] Result<> draw(VkCommandBuffer commands, VkExtent2D extent, const RiverView& view,
                                std::uint32_t frame);

private:
    RiverSurface() = default;

    // A frame's copy of the water.
    struct Copy {
        Buffer vertices;
        Buffer indices;
        std::uint32_t count = 0;
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        WorldPosition origin = {};
        std::uint64_t shown = 0;
    };

    [[nodiscard]] Result<> copy(Copy& copy) const;

    const Device* device_ = nullptr;
    Pipeline pipeline_;
    Sampler sampler_;
    std::vector<Copy> copies_;
    std::shared_ptr<const terrain::RiverWater> water_;
    std::uint64_t shown_ = 0;
};

} // namespace hog::render

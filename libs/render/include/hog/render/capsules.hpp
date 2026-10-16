#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>

#include <span>

namespace hog::render {

class Device;

// A capsule standing on its feet, upright along `up`.
struct Capsule {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition feet = {};
    DVec3 up{0.0, 0.0, 1.0}; // unit
    double height = 1.8;     // metres, feet to the top
    double radius = 0.3;     // metres
    Vec3 colour{0.5F};       // linear
};

// Where capsules are seen from and lit by.
struct CapsuleView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    // How bright the side away from the sun stays, from 0 to 1, unless lit through the air.
    double night = 0.2;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // The frame's air, for capsules lit through it.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
};

// Capsules, solid and lit, hiding and hidden by what's drawn by depth.
class Capsules {
public:
    // Lit through the air, they're lit by the sun's light through it and the sky's, seen through
    // the air and tone mapped, as the ground is; otherwise by the sun alone.
    [[nodiscard]] static Result<Capsules> create(const Device& device, VkFormat colour_format,
                                                 VkFormat depth_format, bool air = false);

    // Records drawing into the colour and depth images being rendered to, of `extent`, each bent
    // to stand where the ground under it is drawn.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const CapsuleView& view,
              std::span<const Capsule> capsules) const;

private:
    Capsules() = default;

    Pipeline pipeline_;
    bool air_ = false;
};

} // namespace hog::render

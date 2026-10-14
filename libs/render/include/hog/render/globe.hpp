#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/vulkan.hpp>

#include <cstdint>

namespace hog::render {

class Device;

// Where a globe is seen from and how it's lit, about the planet's centre.
struct GlobeView {
    Camera camera;
    double radius = 1.0;      // metres
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    // How bright the night side stays, from 0 to 1.
    double night_light = 0.0;
    // Radians below the horizon the sun fades over; at least just above zero.
    double twilight = 0.1;
    // Radii the ground rises per unit of the relief's heights; zero for a smooth sphere.
    double relief = 0.0;
};

// What a globe is drawn from: images with a layer per cube face, ready to sample.
struct GlobeImages {
    // Colours, laid out as the mesh's face points.
    const Image* faces = nullptr;
    // Heights in one float channel, with (n + 1)² texels per face at s and t of 0, 1 / n, ... 1, so
    // neighbouring faces share their edges. They should agree there, as heights taken by direction
    // do.
    const Image* relief = nullptr;
};

// A planet drawn as a sphere with a texture per cube face, lit by a far sun.
class Globe {
public:
    // A mesh with `cells` across each face, drawing into `colour_format` and testing against
    // `depth_format`.
    [[nodiscard]] static Result<Globe> create(const Device& device, VkFormat colour_format,
                                              VkFormat depth_format, int cells);

    // Records drawing into the colour and depth images being rendered to, of `extent`, leaving the
    // rest of them as they were. Both images must be set.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const GlobeImages& images,
              const GlobeView& view) const noexcept;

private:
    Globe() = default;

    Pipeline pipeline_;
    Sampler sampler_;
    Buffer vertices_;
    Buffer indices_;
    std::uint32_t index_count_ = 0;
};

} // namespace hog::render

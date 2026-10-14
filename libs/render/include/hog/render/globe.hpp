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
};

// A planet drawn as a sphere with a texture per cube face, lit by a far sun.
class Globe {
public:
    // A mesh with `cells` across each face, drawing into `colour_format`.
    [[nodiscard]] static Result<Globe> create(const Device& device, VkFormat colour_format,
                                              int cells);

    // Records drawing into the colour image being rendered to, of `extent`, leaving the rest of
    // it as it was. `faces` holds a layer per cube face, laid out as the mesh's face points, and
    // must be ready to sample.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const Image& faces,
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

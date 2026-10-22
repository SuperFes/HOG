#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/vulkan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hog::render {

class Device;

// The foam left on the sea round the eye is kept on a grid of foam_size texels a side, foam_spacing
// metres apart, on a plane touching sea level near the eye. The grid drifts with the water at the
// surface, so what's kept on it needn't move, and steps a whole texel at a time to stay centred
// under the eye. As shaders/foam.slang has them.
inline constexpr std::uint32_t foam_size = 512;
inline constexpr double foam_spacing = 1.0; // metres
// Foam left fades to 1/e of itself over so many seconds.
inline constexpr double foam_lasting = 4.0;
// Nothing's kept over a longer step, or once the eye is further than this from where the plane
// touches sea level, where the plane strays from it.
inline constexpr double foam_longest_step = 2.0;    // seconds
inline constexpr double foam_plane_reach = 2'000.0; // metres

// How the grid moved since the step before.
struct FoamStep {
    // What lay at texel t + shift now lies at t.
    glm::ivec2 shift{0};
    double seconds = 0.0;
    // Whether anything kept before is kept on.
    bool kept = false;
};

// Where the grid lies from the eye: where its plane touches sea level, the ways its rows and
// columns run along it, and its first texel's corner, metres along those from where it touches.
struct FoamPlacing {
    DVec3 anchor{0.0};
    DVec3 east{1.0, 0.0, 0.0};
    DVec3 north{0.0, 1.0, 0.0};
    DVec2 corner{0.0};
};

class FoamGrid {
public:
    // Moves the grid on to an eye over sea level at `up` on a planet of `radius`, `seconds` after
    // the step before, the water at the surface drifting at `drift`, m/s.
    FoamStep follow(DVec3 up, double radius, DVec3 drift, double seconds);
    // Where it lies from an eye `height` metres above sea level at `up`.
    [[nodiscard]] FoamPlacing placing(DVec3 up, double radius, double height) const;

private:
    struct Plane {
        DVec3 touching;
        DVec3 east;
        DVec3 north;
    };
    std::optional<Plane> plane_;
    // How far the water's drifted along the plane since it was laid, metres.
    DVec2 drifted_{0.0};
    // The texel the grid's centred on, counted from where the plane touches, as the water's
    // drifted.
    glm::ivec2 centre_{0};
};

// Where `rest`, a point of sea level from the eye, lies on the grid as `placing` lays it, in
// texels from its first's corner.
[[nodiscard]] DVec2 foam_texel(const FoamPlacing& placing, DVec3 rest) noexcept;

// The foam left on the sea round the eye: how much lies on each texel of the grid, fading as it
// ages.
class FoamMap {
public:
    FoamMap() = default;
    // With no foam on it.
    [[nodiscard]] static Result<FoamMap> create(Device& device);

    // Records stepping the foam on as `step` has the grid move, leaving it ready for any shader to
    // sample.
    void step(VkCommandBuffer commands, const FoamStep& step);

    // What's kept from the last step, ready to sample.
    [[nodiscard]] const Image& kept() const noexcept { return images_[latest_]; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_.handle(); }

private:
    Pipeline pipeline_;
    std::array<Image, 2> images_;
    std::array<TrackedImage, 2> uses_;
    Sampler sampler_;
    std::size_t latest_ = 0;
};

} // namespace hog::render

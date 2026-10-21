#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/waves/waves.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::render {

class Device;

// The most waves the sea is drawn with. As shaders/waves.slang has it.
inline constexpr std::size_t drawn_waves = 48;
// The plane waves summed for the pattern of its foam. Likewise.
inline constexpr std::size_t foam_waves = 26;

// The waves the sea is drawn with, as shaders/waves.slang takes them.
struct DrawnWaves {
    struct Wave {
        glm::vec4 heading_wavenumber{0.0F};
        glm::vec4 amplitude_phase_slope_length{0.0F};
        glm::vec4 along{0.0F};
        glm::vec4 breaking{0.0F};
    };
    struct FoamWave {
        glm::vec4 wavenumber_phase{0.0F};
        glm::vec4 amplitude_length{0.0F};
    };
    glm::vec4 count_left_radius{0.0F};
    glm::vec4 up_height{0.0F};
    glm::vec4 first{0.0F};
    glm::vec4 second{0.0F};
    glm::vec4 foam{0.0F};
    glm::vec4 foam_left{0.0F};
    std::array<Wave, drawn_waves> waves{};
    std::array<FoamWave, foam_waves> foam_pattern{};
};

// The strongest of the waves round an eye at `eye`, bent by `bend`, at `time`, as
// waves::strongest picks them, each as it runs under the eye; and where `sea`, which raised them,
// is white with foam, carried with the water at its surface, which has drifted `drifted` metres
// since the epoch.
[[nodiscard]] DrawnWaves drawn_waves_at(std::span<const waves::Train> trains,
                                        std::span<const waves::Wave> waves,
                                        const waves::SeaState& sea, WorldPosition eye,
                                        const terrain::Bend& bend, GameTime time, DVec3 drifted);

// Where the sea is seen from and what it reflects.
struct OceanView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0};      // unit, towards the sun
    Vec3 sky{0.35F, 0.55F, 0.85F}; // unless lit through the air
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // The frame's air, for the sea lit through it.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
    // A calm sea unless any.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    DrawnWaves waves = {};
};

// The sea's surface, moved by its waves about sea level, out to the horizon round the point under
// the eye. It reflects the sky and the sun over what's drawn beneath it, more the more glancing the
// view, and hides what's drawn after it below.
class Ocean {
public:
    // Lit through the air, it reflects the sky as the eye sees it and the sun's light through the
    // air, is seen through the air and is tone mapped; otherwise it reflects a sky of one colour.
    // `frames` in flight, each with its own waves.
    [[nodiscard]] static Result<Ocean> create(const Device& device, VkFormat colour_format,
                                              VkFormat depth_format, bool air = false,
                                              std::uint32_t frames = 1);

    // Records drawing into the colour and depth images being rendered to, of `extent`, over what's
    // already drawn there. The frame's last submit must have finished.
    Result<> draw(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                  std::uint32_t frame = 0) const;
    // Lit through the air and seen from above the sea, records the water beneath the surface as
    // the ground's shading has it where it lies too deep to show, behind all else: drawn before the
    // ground, so ground under the sea left undrawn looks as it would.
    Result<> draw_deep(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                       std::uint32_t frame = 0) const;

private:
    Ocean() = default;

    Result<> record(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                    std::uint32_t frame, const Pipeline& pipeline) const;

    Pipeline pipeline_;
    Pipeline deep_;
    Buffer vertices_;
    Buffer indices_;
    std::vector<Buffer> waves_;
    std::uint32_t index_count_ = 0;
    bool air_ = false;
};

} // namespace hog::render

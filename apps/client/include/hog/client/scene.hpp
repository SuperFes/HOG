#pragma once

#include <hog/atmosphere/model.hpp>
#include <hog/client/ground.hpp>
#include <hog/client/picture.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/billboard.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/presenter.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/terrain.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/baker.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/stream.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hog::client {

inline constexpr double vertical_fov = 1.0; // radians
inline constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
// Stills are sRGB, as the window shows them.
inline constexpr VkFormat still_format = VK_FORMAT_R8G8B8A8_SRGB;

// Metres above sea level of the ground under a position.
[[nodiscard]] double ground_under(const planet::World& world, WorldPosition at);

// What a frame shows, and when.
struct Shot {
    render::Camera camera;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    GameTime time;
    double seconds = 0.0; // since the frame before, for the eye to adapt to the light over
    // Whoever flies along, where there's someone to draw.
    std::optional<render::Standing> companion;
};

// The planet as the client draws it: the ground baked and streamed in round the eye, the sea, the
// air lit by the sun at the planet's time, and whoever flies along.
class Scene {
public:
    // Drawn into colour images of `colour_format`, `frames` in flight, with the sun and seasons
    // from `calendar`.
    [[nodiscard]] static Result<Scene>
    create(render::Device& device, const planet::World& world, jobs::JobSystem& jobs,
           const sim::Calendar& calendar, VkFormat colour_format, std::uint32_t frames,
           std::optional<render::Billboard> companion = std::nullopt);

    Scene(const Scene&) = delete;
    Scene(Scene&&) noexcept = default;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) noexcept = default;
    // Waits for the bakes in flight, which read the world. The GPU must have finished with it.
    ~Scene();

    // Records drawing the shot into the frame's target, leaving it a colour target. The frame's
    // slot must have finished its last submit.
    Result<> record(render::Frame& frame, const Shot& shot);

    // Waits for the bakes in flight, so they arrive with the next frame.
    void finish_bakes();

    // The last frame recorded wanted nothing more baked, and all it wanted had arrived.
    [[nodiscard]] bool settled() const noexcept { return settled_; }

    [[nodiscard]] const atmosphere::Model& air() const noexcept { return model_; }
    [[nodiscard]] const sim::Calendar& calendar() const noexcept { return calendar_; }

private:
    Scene(const render::Device& device, const planet::World& world, jobs::JobSystem& jobs,
          const sim::Calendar& calendar, render::Terrain terrain, render::Ocean ocean,
          render::TerrainPool pool, render::Atmosphere atmosphere, render::Sky sky,
          std::optional<render::Billboard> companion, std::uint32_t frames);

    Result<const render::Image*> depth_for(const render::Frame& frame);
    render::SkyView sky_view(const Shot& shot);

    const render::Device* device_;
    terrain::PatchBaker baker_;
    terrain::PatchStream stream_;
    render::Terrain terrain_;
    render::Ocean ocean_;
    render::TerrainPool pool_;
    GroundSlots slots_;
    std::vector<std::optional<render::Image>> depths_;
    std::optional<render::Billboard> companion_;
    atmosphere::Model model_;
    render::Atmosphere atmosphere_;
    render::Sky sky_;
    sim::Calendar calendar_;
    std::optional<double> exposure_;
    bool settled_ = false;
};

// The shot drawn into a picture of `extent` once the scene has settled: frame after frame, with the
// bakes finished between, up to `most_frames`. The scene must draw into still_format.
[[nodiscard]] Result<Picture> draw_still(render::Device& device, Scene& scene, const Shot& shot,
                                         VkExtent2D extent, int most_frames = 500);

} // namespace hog::client

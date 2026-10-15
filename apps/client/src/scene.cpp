#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/scene.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/selection.hpp>

#include <glm/glm.hpp>

#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace hog::client {

namespace {

// Behind the sky, which covers it.
constexpr VkClearColorValue cleared{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}};

std::unexpected<Error> failed(std::string_view doing, const Error& error) {
    return std::unexpected(
        Error{.code = error.code, .message = std::format("can't {}: {}", doing, error.message)});
}

} // namespace

double ground_under(const planet::World& world, WorldPosition at) {
    return static_cast<double>(world.state(planet::cell_at(at)).elevation) /
           static_cast<double>(planet::fixed_one);
}

Result<Scene> Scene::create(render::Device& device, const planet::World& world,
                            jobs::JobSystem& jobs, const sim::Calendar& calendar,
                            VkFormat colour_format, std::uint32_t frames,
                            std::optional<render::Billboard> companion) {
    const terrain::StreamSettings settings;
    const std::vector<terrain::MaterialImage> materials = biome_materials();
    auto terrain = render::Terrain::create(device, colour_format, depth_format,
                                           {.materials = materials, .sea = true, .air = true});
    if (!terrain) {
        return failed("draw the ground", terrain.error());
    }
    auto ocean = render::Ocean::create(device, colour_format, depth_format, true);
    if (!ocean) {
        return failed("draw the sea", ocean.error());
    }
    auto pool = render::TerrainPool::create(
        device, {.patches = settings.capacity, .frames = frames, .uploads = settings.uploads});
    if (!pool) {
        return failed("hold the ground", pool.error());
    }
    auto atmosphere = render::Atmosphere::create(device);
    if (!atmosphere) {
        return failed("draw the air", atmosphere.error());
    }
    auto sky = render::Sky::create(device, colour_format, depth_format, frames);
    if (!sky) {
        return failed("draw the sky", sky.error());
    }
    return Scene{device,
                 world,
                 jobs,
                 calendar,
                 std::move(terrain).value(),
                 std::move(ocean).value(),
                 std::move(pool).value(),
                 std::move(atmosphere).value(),
                 std::move(sky).value(),
                 std::move(companion),
                 frames};
}

Scene::Scene(const render::Device& device, const planet::World& world, jobs::JobSystem& jobs,
             const sim::Calendar& calendar, render::Terrain terrain, render::Ocean ocean,
             render::TerrainPool pool, render::Atmosphere atmosphere, render::Sky sky,
             std::optional<render::Billboard> companion, std::uint32_t frames)
    : device_{&device}, baker_{world, jobs}, terrain_{std::move(terrain)}, ocean_{std::move(ocean)},
      pool_{std::move(pool)}, depths_(frames), companion_{std::move(companion)},
      atmosphere_{std::move(atmosphere)}, sky_{std::move(sky)}, calendar_{calendar} {}

Scene::~Scene() {
    (void)baker_.finish();
}

void Scene::finish_bakes() {
    for (terrain::Patch& patch : baker_.finish()) {
        stream_.baked(std::move(patch));
    }
}

Result<const render::Image*> Scene::depth_for(const render::Frame& frame) {
    std::optional<render::Image>& depth = depths_.at(frame.slot);
    if (!depth || depth->desc().extent.width != frame.extent.width ||
        depth->desc().extent.height != frame.extent.height) {
        // This slot's last frame has finished, so nothing still draws with it.
        auto made =
            render::make_image(*device_, {.format = depth_format,
                                          .extent = frame.extent,
                                          .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                          .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        depth = std::move(made).value();
    }
    return &*depth;
}

render::SkyView Scene::sky_view(const Shot& shot) {
    const DVec3 sun = calendar_.sun_direction(shot.time);
    const EyeLight light =
        light_at_eye(model_, shot.bend.eye_height * render::air_scale(model_.air(), shot.bend),
                     glm::dot(sun, shot.bend.up));
    const double wanted = exposure_for(light);
    exposure_ = exposure_ ? adapted(*exposure_, wanted, shot.seconds) : wanted;
    return {.camera = shot.camera,
            .sun = sun,
            .bend = shot.bend,
            .exposure = *exposure_,
            .sky_light = light.sky};
}

Result<> Scene::record(render::Frame& frame, const Shot& shot) {
    for (terrain::Patch& patch : baker_.collect()) {
        stream_.baked(std::move(patch));
    }
    terrain::Viewpoint seen = viewpoint(shot.camera, frame.extent);
    seen.bend = shot.bend;
    const std::vector<terrain::SelectedPatch> selected =
        terrain::select_patches(seen, shot.bend.radius, {}, stream_.known_heights());
    const terrain::StreamFrame streamed = stream_.update(selected);
    for (const planet::CellId cell : streamed.bakes) {
        baker_.start(cell);
    }
    settled_ = streamed.bakes.empty() && streamed.arrived.empty() && streamed.evicted.empty() &&
               baker_.in_flight() == 0;
    if (auto taken = slots_.take(pool_, frame.slot, streamed); !taken) {
        return taken;
    }
    if (auto recorded = pool_.record_uploads(frame.commands, frame.slot); !recorded) {
        return recorded;
    }
    const auto depth = depth_for(frame);
    if (!depth) {
        return std::unexpected(depth.error());
    }
    const render::SkyView sky = sky_view(shot);
    if (auto recorded =
            sky_.record_frame(frame.commands, frame.extent, frame.slot, atmosphere_, sky);
        !recorded) {
        return recorded;
    }
    const render::AirBindings through = sky_.bindings(frame.slot, atmosphere_);

    frame.target.use_as(frame.commands, render::image_use::colour_target);
    render::transition(frame.commands, (*depth)->handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                       render::image_use::discard, render::image_use::depth_target);
    render::begin_rendering(frame.commands, frame.view, frame.extent, cleared,
                            {.view = (*depth)->view()});
    sky_.draw(frame.commands, frame.extent, frame.slot, atmosphere_, sky);
    terrain_.draw(frame.commands, frame.extent, pool_, slots_.draws(streamed.drawn),
                  {.camera = shot.camera, .sun = sky.sun, .bend = shot.bend, .air = through});
    ocean_.draw(frame.commands, frame.extent,
                {.camera = shot.camera, .sun = sky.sun, .bend = shot.bend, .air = through});
    if (companion_ && shot.companion) {
        companion_->draw(frame.commands, frame.extent, shot.camera, *shot.companion, shot.bend);
    }
    vkCmdEndRendering(frame.commands);
    return {};
}

Result<Picture> draw_still(render::Device& device, Scene& scene, const Shot& shot,
                           VkExtent2D extent, int most_frames) {
    auto target = render::make_image(
        device, {.format = still_format,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    if (!target) {
        return std::unexpected(std::move(target).error());
    }
    for (int drawn = 0; drawn < most_frames && !scene.settled(); ++drawn) {
        Result<> recorded;
        if (auto submitted = device.submit_now([&](VkCommandBuffer commands) {
                render::Frame frame{.commands = commands,
                                    .target = {target->handle(), VK_IMAGE_ASPECT_COLOR_BIT},
                                    .view = target->view(),
                                    .format = still_format,
                                    .extent = extent,
                                    .slot = 0,
                                    .image = 0};
                recorded = scene.record(frame, shot);
            });
            !submitted) {
            return std::unexpected(std::move(submitted).error());
        }
        if (!recorded) {
            return std::unexpected(std::move(recorded).error());
        }
        scene.finish_bakes();
    }
    if (!scene.settled()) {
        return make_error(
            ErrorCode::OutOfRange,
            std::format("the ground was still arriving after {} frames", most_frames));
    }
    auto pixels = render::read_pixels(device, *target, render::image_use::colour_target);
    if (!pixels) {
        return std::unexpected(std::move(pixels).error());
    }
    const auto bytes = std::as_bytes(std::span{*pixels});
    return Picture{
        .width = extent.width, .height = extent.height, .texels = {bytes.begin(), bytes.end()}};
}

} // namespace hog::client

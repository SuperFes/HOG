#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/scene.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/render_graph.hpp>
#include <hog/sim/weather.hpp>
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

protocol::CellWeather weather_at_eye(const Shot& shot) {
    if (shot.forecast == nullptr || !shot.forecast->known()) {
        return {};
    }
    return shot.forecast->at(shot.bend.up, shot.now);
}

DVec3 drift_at(DVec3 up, const protocol::CellWeather& weather) {
    return planet::tangent_frame(up).to_world(
        {double{weather.drift.x}, double{weather.drift.y}, 0.0});
}

double falling_at_eye(const terrain::Bend& bend, const protocol::CellWeather& weather) noexcept {
    return bend.eye_height < cloud_base ? double{weather.falling} : 0.0;
}

render::CloudShade shade_at_eye(const terrain::Bend& bend, const protocol::CellWeather& weather) {
    if (bend.eye_height >= cloud_base) {
        return {};
    }
    return render::cloud_shade(double{sim::cloud_cover(weather.cloud)}, double{weather.cloud});
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
    auto others = render::Capsules::create(device, colour_format, depth_format, true);
    if (!others) {
        return failed("draw other players", others.error());
    }
    auto clouds = render::Clouds::create(device, colour_format, weather_map_size, frames);
    if (!clouds) {
        return failed("draw the clouds", clouds.error());
    }
    auto precipitation = render::Precipitation::create(device, colour_format, depth_format);
    if (!precipitation) {
        return failed("draw the rain", precipitation.error());
    }
    auto glare = render::Glare::create(device, colour_format);
    if (!glare) {
        return failed("draw the sun's glare", glare.error());
    }
    auto timer = render::GpuTimer::create(device, frames);
    if (!timer) {
        return failed("time the GPU", timer.error());
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
                 std::move(others).value(),
                 std::move(clouds).value(),
                 std::move(precipitation).value(),
                 std::move(glare).value(),
                 std::move(timer).value(),
                 std::move(companion),
                 frames};
}

Scene::Scene(const render::Device& device, const planet::World& world, jobs::JobSystem& jobs,
             const sim::Calendar& calendar, render::Terrain terrain, render::Ocean ocean,
             render::TerrainPool pool, render::Atmosphere atmosphere, render::Sky sky,
             render::Capsules others, render::Clouds clouds, render::Precipitation precipitation,
             render::Glare glare, render::GpuTimer timer,
             std::optional<render::Billboard> companion, std::uint32_t frames)
    : baker_{world, jobs}, terrain_{std::move(terrain)}, ocean_{std::move(ocean)},
      pool_{std::move(pool)}, transients_{device, frames}, others_{std::move(others)},
      companion_{std::move(companion)}, atmosphere_{std::move(atmosphere)}, sky_{std::move(sky)},
      clouds_{std::move(clouds)}, precipitation_{std::move(precipitation)},
      glare_{std::move(glare)}, timer_{std::move(timer)}, calendar_{calendar} {}

Scene::~Scene() {
    (void)baker_.finish();
}

void Scene::finish_bakes() {
    for (terrain::Patch& patch : baker_.finish()) {
        stream_.baked(std::move(patch));
    }
}

render::SkyView Scene::sky_view(const Shot& shot, render::CloudShade shade) {
    const DVec3 sun = calendar_.sun_direction(shot.time);
    const EyeLight light =
        light_at_eye(model_, shot.bend.eye_height * render::air_scale(model_.air(), shot.bend),
                     glm::dot(sun, shot.bend.up));
    const double wanted = exposure_for(under_cloud(light, shade));
    exposure_ = exposure_ ? adapted(*exposure_, wanted, shot.seconds) : wanted;
    return {.camera = shot.camera,
            .sun = sun,
            .bend = shot.bend,
            .exposure = *exposure_,
            .sky_light = light.sky,
            .shade = shade};
}

Result<> Scene::record(render::Frame& frame, const Shot& shot) {
    const std::span<const render::GpuSpan> timed = timer_.read(frame.commands, frame.slot);
    timings_.assign(timed.begin(), timed.end());
    for (terrain::Patch& patch : baker_.collect()) {
        stream_.baked(std::move(patch));
    }
    terrain::Viewpoint seen = viewpoint(shot.camera, frame.extent);
    seen.bend = shot.bend;
    // All round the eye, so the ground is baked before it's turned to.
    const std::vector<terrain::SelectedPatch> selected = terrain::select_patches(
        seen, shot.bend.radius, {.only_in_view = false}, stream_.known_heights());
    const terrain::StreamFrame streamed = stream_.update(selected);
    for (const planet::CellId cell : streamed.bakes) {
        baker_.start(cell);
    }
    settled_ = streamed.bakes.empty() && streamed.arrived.empty() && streamed.evicted.empty() &&
               baker_.in_flight() == 0;
    if (auto taken = slots_.take(pool_, frame.slot, streamed); !taken) {
        return taken;
    }
    const protocol::CellWeather here = weather_at_eye(shot);
    const render::CloudShade shade = shade_at_eye(shot.bend, here);
    const DVec3 drift = drift_at(shot.bend.up, here);
    const double falling = falling_at_eye(shot.bend, here);
    drifted_ = render::within_period(drifted_ + (drift * shot.seconds));
    falling_for_ += shot.seconds;
    const bool new_weather = shot.forecast != nullptr && shot.forecast->known() &&
                             weather_taken_ != shot.forecast->received();
    const bool cloudy = shot.forecast != nullptr && shot.forecast->known();
    const render::SkyView sky = sky_view(shot, shade);
    const render::AirBindings through = sky_.bindings(frame.slot, atmosphere_);

    render::RenderGraph graph;
    const render::GraphImage target = graph.import(frame.target, frame.view, frame.extent);
    const render::GraphImage depth = graph.transient(
        {.format = depth_format,
         .extent = frame.extent,
         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
         .layers = 1,
         .levels = 1});
    // Made afresh each frame, so what the slot's last frame left in them isn't needed.
    const render::Image& sky_table = sky_.table(frame.slot);
    const render::Image& aerial_volume = sky_.aerial_volume(frame.slot);
    render::TrackedImage tracked_table{sky_table.handle(), VK_IMAGE_ASPECT_COLOR_BIT};
    render::TrackedImage tracked_aerial{aerial_volume.handle(), VK_IMAGE_ASPECT_COLOR_BIT};
    const render::GraphImage table =
        graph.import(tracked_table, sky_table.view(), sky_table.desc().extent);
    const render::GraphImage aerial =
        graph.import(tracked_aerial, aerial_volume.view(), aerial_volume.desc().extent);
    const std::vector<render::GraphUse> through_air{
        {.image = table, .use = render::image_use::sampled},
        {.image = aerial, .use = render::image_use::sampled}};
    const render::GraphTargets drawn{.colour = {.image = target, .clear = std::nullopt},
                                     .depth = {{.image = depth, .clear = std::nullopt}}};

    graph.add({.name = "uploads", .uses = {}, .targets = std::nullopt},
              [&](VkCommandBuffer commands) -> Result<> {
                  if (new_weather) {
                      if (auto updated =
                              clouds_.update(commands, frame.slot,
                                             std::as_bytes(std::span{shot.forecast->before()}),
                                             std::as_bytes(std::span{shot.forecast->after()}));
                          !updated) {
                          return updated;
                      }
                  }
                  return pool_.record_uploads(commands, frame.slot);
              });
    graph.add({.name = "sky tables",
               .uses = {{.image = table, .use = render::image_use::storage},
                        {.image = aerial, .use = render::image_use::storage}},
               .targets = std::nullopt},
              [&](VkCommandBuffer commands) {
                  return sky_.record_tables(commands, frame.extent, frame.slot, atmosphere_, sky);
              });
    graph.add({.name = "sky",
               .uses = {{.image = table, .use = render::image_use::sampled}},
               .targets = {{.colour = {.image = target, .clear = cleared},
                            .depth = {{.image = depth, .clear = 0.0F}}}}},
              [&](VkCommandBuffer commands) {
                  sky_.draw(commands, frame.extent, frame.slot, atmosphere_, sky);
              });
    graph.add({.name = "ground", .uses = through_air, .targets = drawn},
              [&](VkCommandBuffer commands) {
                  terrain_.draw(
                      commands, frame.extent, pool_, slots_.draws(streamed.drawn),
                      {.camera = shot.camera, .sun = sky.sun, .bend = shot.bend, .air = through});
              });
    graph.add(
        {.name = "sea", .uses = through_air, .targets = drawn}, [&](VkCommandBuffer commands) {
            ocean_.draw(commands, frame.extent,
                        {.camera = shot.camera, .sun = sky.sun, .bend = shot.bend, .air = through});
        });
    if (!shot.others.empty()) {
        graph.add(
            {.name = "others", .uses = through_air, .targets = drawn},
            [&](VkCommandBuffer commands) {
                others_.draw(
                    commands, frame.extent,
                    {.camera = shot.camera, .sun = sky.sun, .bend = shot.bend, .air = through},
                    shot.others);
            });
    }
    if (companion_ && shot.companion) {
        graph.add(
            {.name = "companion", .uses = {}, .targets = drawn}, [&](VkCommandBuffer commands) {
                companion_->draw(commands, frame.extent, shot.camera, *shot.companion, shot.bend);
            });
    }
    if (render::precipitation_count(falling, double{here.snow}) > 0) {
        graph.add({.name = "rain", .uses = {}, .targets = drawn}, [&](VkCommandBuffer commands) {
            precipitation_.draw(commands, frame.extent,
                                {.camera = shot.camera,
                                 .air = through,
                                 .up = shot.bend.up,
                                 .wind = drift * surface_wind,
                                 .falling = falling,
                                 .snow = double{here.snow},
                                 .seconds = falling_for_});
        });
    }
    // The clouds and glare read what's drawn in front of them from depth, so they're drawn
    // without it.
    if (cloudy) {
        graph.add({.name = "clouds",
                   .uses = {{.image = aerial, .use = render::image_use::sampled},
                            {.image = depth, .use = render::image_use::sampled}},
                   .targets = {{.colour = {.image = target, .clear = std::nullopt},
                                .depth = std::nullopt}}},
                  [&](VkCommandBuffer commands) {
                      clouds_.draw(commands, frame.extent, graph.view(depth),
                                   {.camera = shot.camera,
                                    .bend = shot.bend,
                                    .air = through,
                                    .base = cloud_base,
                                    .faded = shot.forecast->faded(shot.now),
                                    .moved = drifted_});
                  });
    }
    const double height = shot.bend.eye_height * render::air_scale(model_.air(), shot.bend);
    graph.add(
        {.name = "glare",
         .uses = {{.image = depth, .use = render::image_use::sampled}},
         .targets = {{.colour = {.image = target, .clear = std::nullopt}, .depth = std::nullopt}}},
        [&](VkCommandBuffer commands) {
            glare_.draw(
                commands, frame.extent, graph.view(depth),
                {.camera = shot.camera,
                 .towards = sky.sun,
                 .irradiance =
                     model_.transmittance(height, glm::dot(sky.sun, shot.bend.up)) * shade.direct,
                 .exposure = sky.exposure});
        });
    if (new_weather) {
        weather_taken_ = shot.forecast->received();
    }
    return graph.execute(frame.commands,
                         {.slot = frame.slot, .timer = &timer_, .transients = &transients_});
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
    // At least once, though the scene settled drawing another shot.
    for (int drawn = 0; drawn < most_frames && (drawn == 0 || !scene.settled()); ++drawn) {
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

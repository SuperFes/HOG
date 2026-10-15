#include <hog/client/flight.hpp>
#include <hog/client/ground.hpp>
#include <hog/client/options.hpp>
#include <hog/client/picture.hpp>
#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/billboard.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/presenter.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/terrain.hpp>
#include <hog/terrain/baker.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/selection.hpp>
#include <hog/terrain/stream.hpp>
#include <hog/window/window.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace hog;

constexpr double vertical_fov = 1.0;       // radians
constexpr double start_altitude = 1'500.0; // metres over the starting islands
constexpr double start_pitch = -0.2;       // radians
constexpr double fast = 5.0;               // times as fast with Shift held
constexpr double speed_step = 1.25;        // per notch of the mouse wheel
constexpr double slowest_speed = 0.01;     // heights a second
constexpr double fastest_speed = 10.0;
// A frame's movement is held to this many seconds, so a stall doesn't fling the eye away.
constexpr double longest_frame = 0.1;
// How long to wait between looks at a minimised window.
constexpr Uint32 idle_ms = 50;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr VkClearColorValue sky{.float32 = {0.35F, 0.55F, 0.85F, 1.0F}};
// Someone 3 m tall flying along 8 m ahead, 2 m below the eye, so they land in front of it.
constexpr double companion_height = 3.0;
constexpr double companion_ahead = 8.0;
constexpr double companion_below = 2.0;
// Light from the sky on ground the sun doesn't reach.
constexpr double night_light = 0.15;

// What the person flying or walking asks for between frames.
struct Controls {
    DVec2 look{};
    double speed = client::FlySettings{}.speed;
    bool walking = false;
    bool quit = false;
};

void handle(const SDL_Event& event, SDL_Window* window, Controls& controls) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
        controls.quit = true;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event.key.key == SDLK_ESCAPE) {
            controls.quit = true;
        } else if (event.key.key == SDLK_F && !event.key.repeat) {
            controls.walking = !controls.walking;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_RIGHT) {
            SDL_SetWindowRelativeMouseMode(window, event.button.down);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (SDL_GetWindowRelativeMouseMode(window)) {
            controls.look += DVec2{static_cast<double>(event.motion.xrel),
                                   static_cast<double>(event.motion.yrel)};
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (controls.walking) {
            break;
        }
        controls.speed =
            std::clamp(controls.speed * std::pow(speed_step, static_cast<double>(event.wheel.y)),
                       slowest_speed, fastest_speed);
        break;
    default:
        break;
    }
}

// The keys held down, as movement.
DVec3 movement() {
    int count = 0;
    const std::span held{SDL_GetKeyboardState(&count), static_cast<std::size_t>(count)};
    const auto axis = [&](SDL_Scancode more, SDL_Scancode less) {
        return (held[more] ? 1.0 : 0.0) - (held[less] ? 1.0 : 0.0);
    };
    return {axis(SDL_SCANCODE_D, SDL_SCANCODE_A), axis(SDL_SCANCODE_SPACE, SDL_SCANCODE_C),
            axis(SDL_SCANCODE_W, SDL_SCANCODE_S)};
}

client::Flight start(const client::Options& options, const planet::World& world) {
    const double radius = world.geology().planet_radius();
    client::Flight flight;
    if (options.start) {
        constexpr double degree = std::numbers::pi / 180.0;
        flight = client::start_flight(planet::direction_at(options.start->latitude * degree,
                                                           options.start->longitude * degree),
                                      radius, options.start->altitude, planet::north_pole);
    } else {
        const planet::Archipelago& islands = world.geology().terrain().archipelago();
        flight = client::start_flight(islands.center(), radius, start_altitude,
                                      islands.islands().front().center);
    }
    flight.pitch = start_pitch;
    return flight;
}

// Metres above sea level of the ground under the eye.
double ground_under(const planet::World& world, WorldPosition eye) {
    return static_cast<double>(world.state(planet::cell_at(eye)).elevation) /
           static_cast<double>(planet::fixed_one);
}

// The ground as it's baked, streamed and drawn.
struct Ground {
    terrain::PatchBaker baker;
    terrain::PatchStream stream;
    render::Terrain terrain;
    render::Ocean ocean;
    render::TerrainPool pool;
    client::GroundSlots slots;
    std::array<std::optional<render::Image>, render::frames_in_flight> depths;
    render::TerrainView lighting;
    std::optional<render::Billboard> companion;
};

Result<const render::Image*> depth_for(Ground& ground, const render::Device& device,
                                       const render::Frame& frame) {
    std::optional<render::Image>& depth = ground.depths.at(frame.slot);
    if (!depth || depth->desc().extent.width != frame.extent.width ||
        depth->desc().extent.height != frame.extent.height) {
        // This slot's last frame has finished, so nothing still draws with it.
        auto made =
            render::make_image(device, {.format = depth_format,
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

// Records copying what's been drawn so far this frame, to save once the GPU has finished.
Result<render::Buffer> start_capture(const render::Device& device,
                                     const render::Presenter& presenter, render::Frame& frame) {
    if ((presenter.swapchain().usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        return make_error(ErrorCode::Unsupported, "the window's images can't be read");
    }
    auto buffer = render::make_readback(device, frame.extent);
    if (buffer) {
        render::record_readback(frame.commands, frame.target, frame.extent, *buffer);
    }
    return buffer;
}

Result<void> save_capture(const render::Device& device, const render::Buffer& buffer,
                          const render::Frame& frame, const std::filesystem::path& path) {
    if (const VkResult waited = vkDeviceWaitIdle(device.handle()); waited != VK_SUCCESS) {
        return render::vulkan_error(waited, "waiting for the frame to capture");
    }
    const auto pixels = render::read_back(buffer);
    if (!pixels) {
        return std::unexpected(pixels.error());
    }
    auto rgb = render::to_rgb(*pixels, frame.format);
    if (!rgb) {
        return std::unexpected(rgb.error());
    }
    return mapgen::write_png(path, mapgen::Image{.width = static_cast<int>(frame.extent.width),
                                                 .height = static_cast<int>(frame.extent.height),
                                                 .pixels = std::move(rgb).value()});
}

// Draws a frame of the ground from the flight, saving it to `capture` if given; false when
// there's nothing to draw into.
Result<bool> draw(Ground& ground, const client::Flight& flight, double planet_radius,
                  const render::Standing& companion, const terrain::Bend& bend,
                  const render::Device& device, render::Presenter& presenter, SDL_Window* window,
                  const std::optional<std::filesystem::path>& capture) {
    auto begun = presenter.begin(window::drawable(window));
    if (!begun) {
        return std::unexpected(std::move(begun).error());
    }
    if (!begun->has_value()) {
        return false;
    }
    render::Frame& frame = begun->value();
    const render::Camera camera = client::flight_camera(flight, vertical_fov);

    for (terrain::Patch& patch : ground.baker.collect()) {
        ground.stream.baked(std::move(patch));
    }
    terrain::Viewpoint seen = client::viewpoint(camera, frame.extent);
    seen.bend = bend;
    const std::vector<terrain::SelectedPatch> selected =
        terrain::select_patches(seen, planet_radius, {}, ground.stream.known_heights());
    const terrain::StreamFrame streamed = ground.stream.update(selected);
    for (const planet::CellId cell : streamed.bakes) {
        ground.baker.start(cell);
    }
    if (auto taken = ground.slots.take(ground.pool, frame.slot, streamed); !taken) {
        return std::unexpected(std::move(taken).error());
    }
    if (auto recorded = ground.pool.record_uploads(frame.commands, frame.slot); !recorded) {
        return std::unexpected(std::move(recorded).error());
    }
    const auto depth = depth_for(ground, device, frame);
    if (!depth) {
        return std::unexpected(depth.error());
    }

    frame.target.use_as(frame.commands, render::image_use::colour_target);
    render::transition(frame.commands, (*depth)->handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                       render::image_use::discard, render::image_use::depth_target);
    render::begin_rendering(frame.commands, frame.view, frame.extent, sky,
                            {.view = (*depth)->view()});
    render::TerrainView view = ground.lighting;
    view.camera = camera;
    view.bend = bend;
    ground.terrain.draw(frame.commands, frame.extent, ground.pool,
                        ground.slots.draws(streamed.drawn), view);
    ground.ocean.draw(frame.commands, frame.extent,
                      {.camera = camera,
                       .sun = view.sun,
                       .sky = Vec3{sky.float32[0], sky.float32[1], sky.float32[2]},
                       .bend = bend});
    if (ground.companion) {
        ground.companion->draw(frame.commands, frame.extent, camera, companion, bend);
    }
    vkCmdEndRendering(frame.commands);

    std::optional<Result<render::Buffer>> captured;
    if (capture) {
        captured = start_capture(device, presenter, frame);
    }
    if (auto ended = presenter.end(frame); !ended) {
        return std::unexpected(std::move(ended).error());
    }
    if (capture) {
        const Result<void> saved = captured->has_value()
                                       ? save_capture(device, captured->value(), frame, *capture)
                                       : std::unexpected(captured->error());
        if (!saved) {
            std::cerr << std::format("hog-client: can't capture the window: {}\n", saved.error());
        }
    }
    return true;
}

// Frames a second and height, in the window's title once a second.
class TitleCounter {
public:
    void count(SDL_Window* window, double height) {
        ++frames_;
        const MonotonicClock::time_point now = MonotonicClock::now();
        const std::chrono::duration<double> passed = now - since_;
        if (passed.count() >= 1.0) {
            const std::string title =
                std::format("HOG - {:.0f} fps, {:.0f} m up",
                            static_cast<double>(frames_) / passed.count(), height);
            SDL_SetWindowTitle(window, title.c_str());
            frames_ = 0;
            since_ = now;
        }
    }

private:
    MonotonicClock::time_point since_ = MonotonicClock::now();
    int frames_ = 0;
};

// Flies or walks over the world until the window closes or it's time to quit.
Result<void> fly(Ground& ground, const planet::World& world, client::Flight flight,
                 const client::Options& options, const render::Device& device,
                 render::Presenter& presenter, SDL_Window* window) {
    const double radius = world.geology().planet_radius();
    Controls controls;
    TitleCounter title;
    const MonotonicClock::time_point started = MonotonicClock::now();
    MonotonicClock::time_point last = started;
    while (!controls.quit) {
        controls.look = {};
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handle(event, window, controls);
        }
        const MonotonicClock::time_point now = MonotonicClock::now();
        const double seconds =
            std::min(std::chrono::duration<double>(now - last).count(), longest_frame);
        last = now;
        const bool faster = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        const client::FlyInput input{.move = movement(), .look = controls.look, .seconds = seconds};
        flight = controls.walking
                     ? client::walk(flight, input, faster, radius,
                                    [&](WorldPosition at) { return ground_under(world, at); })
                     : client::fly(flight, input, radius, ground_under(world, flight.eye),
                                   {.speed = controls.speed * (faster ? fast : 1.0)});
        const double ground_height = ground_under(world, flight.eye);
        title.count(window, glm::length(flight.eye.metres()) - radius - ground_height);

        if (options.quit_after && now - started >= *options.quit_after) {
            controls.quit = true;
        }
        const DVec3 companion_up = client::companion_direction(flight, companion_ahead);
        const double ground_ahead = ground_under(world, WorldPosition{companion_up * radius});
        const render::Standing companion{
            .feet = controls.walking
                        ? WorldPosition{companion_up * (radius + ground_ahead)}
                        : client::companion_feet(flight, companion_ahead, companion_below, radius,
                                                 ground_ahead),
            .up = companion_up,
            .height = companion_height};
        const terrain::Bend bend = terrain::bend_from(flight.eye, radius, ground_height);
        const auto drawn = draw(ground, flight, radius, companion, bend, device, presenter, window,
                                controls.quit ? options.capture : std::nullopt);
        if (!drawn) {
            return std::unexpected(drawn.error());
        }
        if (!*drawn) {
            SDL_Delay(idle_ms);
        }
    }
    return {};
}

// The companion as a billboard, if there's one to fly with and they can be drawn.
std::optional<render::Billboard> companion(const client::Options& options, render::Device& device,
                                           VkFormat colour_format) {
    if (!options.companion) {
        return std::nullopt;
    }
    auto picture = client::load_picture(*options.companion);
    if (!picture) {
        std::cerr << std::format("hog-client: flying alone: {}\n", picture.error());
        return std::nullopt;
    }
    client::lift_ink(*picture);
    auto billboard = render::Billboard::create(device, colour_format, depth_format,
                                               {.width = picture->width, .height = picture->height},
                                               picture->texels);
    if (!billboard) {
        std::cerr << std::format("hog-client: flying alone: {}\n", billboard.error());
        return std::nullopt;
    }
    return std::move(billboard).value();
}

int show(const client::Options& options, const planet::World& world, render::Device& device,
         render::Presenter& presenter, SDL_Window* window) {
    const terrain::StreamSettings settings;
    const std::vector<terrain::MaterialImage> materials = client::biome_materials();
    auto terrain = render::Terrain::create(device, presenter.swapchain().format(), depth_format,
                                           {.materials = materials, .sea = true});
    if (!terrain) {
        std::cerr << std::format("hog-client: can't draw the ground: {}\n", terrain.error());
        return EXIT_FAILURE;
    }
    auto ocean = render::Ocean::create(device, presenter.swapchain().format(), depth_format);
    if (!ocean) {
        std::cerr << std::format("hog-client: can't draw the sea: {}\n", ocean.error());
        return EXIT_FAILURE;
    }
    auto pool = render::TerrainPool::create(device, {.patches = settings.capacity,
                                                     .frames = render::frames_in_flight,
                                                     .uploads = settings.uploads});
    if (!pool) {
        std::cerr << std::format("hog-client: can't hold the ground: {}\n", pool.error());
        return EXIT_FAILURE;
    }
    const client::Flight flight = start(options, world);
    // A sun fixed over where the flight starts, high and off to one side.
    const planet::TangentFrame frame = planet::tangent_frame(flight.eye);

    jobs::JobSystem jobs{options.threads};
    Ground ground{.baker = terrain::PatchBaker{world, jobs},
                  .stream = terrain::PatchStream{settings},
                  .terrain = std::move(terrain).value(),
                  .ocean = std::move(ocean).value(),
                  .pool = std::move(pool).value(),
                  .slots = {},
                  .depths = {},
                  .lighting = {.camera = {},
                               .sun = glm::normalize(frame.to_world({0.5, 0.3, 0.8})),
                               .night_light = night_light},
                  .companion = companion(options, device, presenter.swapchain().format())};
    const Result<void> flown = fly(ground, world, flight, options, device, presenter, window);
    // The bakes go before the world, and the GPU may still be drawing with the ground.
    (void)ground.baker.finish();
    vkDeviceWaitIdle(device.handle());
    if (!flown) {
        std::cerr << std::format("hog-client: {}\n", flown.error());
        return EXIT_FAILURE;
    }
    if (options.validate && device.validation_errors() != 0) {
        std::cerr << std::format("hog-client: {} Vulkan validation errors\n",
                                 device.validation_errors());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int run(std::span<const std::string_view> arguments) {
    auto options = client::parse_options(arguments);
    if (!options) {
        std::cerr << std::format("hog-client: {}\n\n{}", options.error().message, client::usage());
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::cout << client::usage();
        return EXIT_SUCCESS;
    }
    // Found from the program too, so it runs from any directory.
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        options->planet = client::locate(options->planet, base);
        if (options->companion) {
            options->companion = client::locate(*options->companion, base);
        }
    }
    const auto planet = planet::load_planet_config(options->planet);
    if (!planet) {
        std::cerr << std::format("hog-client: {}\n", planet.error());
        return EXIT_FAILURE;
    }
    const planet::World world{*planet};

    // Declared in the order they're made, so each goes before what it was made with.
    const window::Sdl sdl;
    if (!sdl.started()) {
        std::cerr << std::format("hog-client: can't start SDL: {}\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    if (!sdl.vulkan()) {
        std::cerr << std::format("hog-client: can't load Vulkan: {}\n", SDL_GetError());
        return window::no_vulkan;
    }
    const window::Window window{
        SDL_CreateWindow("HOG", 1'600, 900,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)};
    if (window == nullptr) {
        std::cerr << std::format("hog-client: can't open a window: {}\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    const auto wanted = window::device_options(
        window.get(),
        {.app_name = "HOG", .validation = options->validate, .prefer_software = options->software});
    if (!wanted) {
        std::cerr << std::format("hog-client: {}\n", wanted.error());
        return window::no_vulkan;
    }
    const auto device = render::Device::create(*wanted);
    if (!device) {
        std::cerr << std::format("hog-client: can't draw: {}\n", device.error());
        return device.error().code == ErrorCode::Unsupported ? window::no_vulkan : EXIT_FAILURE;
    }
    const auto presenter = render::Presenter::create(**device, window::drawable(window.get()));
    if (!presenter) {
        std::cerr << std::format("hog-client: can't show frames: {}\n", presenter.error());
        return EXIT_FAILURE;
    }
    return show(*options, world, **device, **presenter, window.get());
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span<char*> given{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(given.begin() + 1, given.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-client: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

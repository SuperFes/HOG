#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/options.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/scene.hpp>
#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/billboard.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/presenter.hpp>
#include <hog/render/readback.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/window/window.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
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
// Someone 3 m tall flying along 8 m ahead, 2 m below the eye, so they land in front of it.
constexpr double companion_height = 3.0;
constexpr double companion_ahead = 8.0;
constexpr double companion_below = 2.0;

// What the person flying or walking asks for between frames.
struct Controls {
    DVec2 look{};
    double speed = client::FlySettings{}.speed;
    bool walking = false;
    bool quit = false;
    // Doublings of the planet's time asked for, halvings if negative.
    int faster_time = 0;
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
        } else if (event.key.key == SDLK_RIGHTBRACKET) {
            ++controls.faster_time;
        } else if (event.key.key == SDLK_LEFTBRACKET) {
            --controls.faster_time;
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

// Draws a frame of the shot, saving it to `capture` if given; false when there's nothing to draw
// into.
Result<bool> draw(client::Scene& scene, const client::Shot& shot, const render::Device& device,
                  render::Presenter& presenter, SDL_Window* window,
                  const std::optional<std::filesystem::path>& capture) {
    auto begun = presenter.begin(window::drawable(window));
    if (!begun) {
        return std::unexpected(std::move(begun).error());
    }
    if (!begun->has_value()) {
        return false;
    }
    render::Frame& frame = begun->value();
    if (auto recorded = scene.record(frame, shot); !recorded) {
        return std::unexpected(std::move(recorded).error());
    }
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

// Frames a second, height, the local time of day and how fast it runs, in the window's title once
// a second.
class TitleCounter {
public:
    void count(SDL_Window* window, double height, double time_of_day, double rate) {
        ++frames_;
        const MonotonicClock::time_point now = MonotonicClock::now();
        const std::chrono::duration<double> passed = now - since_;
        if (passed.count() >= 1.0) {
            const auto minutes = static_cast<int>(time_of_day * 24.0 * 60.0);
            const std::string title =
                std::format("HOG - {:.0f} fps, {:.0f} m up, {:02}:{:02}{}",
                            static_cast<double>(frames_) / passed.count(), height, minutes / 60,
                            minutes % 60, rate > 1.0 ? std::format(", time {:.0f}x", rate) : "");
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
Result<void> fly(client::Scene& scene, client::PlanetClock& clock, const planet::World& world,
                 client::Flight flight, const client::Options& options,
                 const render::Device& device, render::Presenter& presenter, SDL_Window* window) {
    const double radius = world.geology().planet_radius();
    Controls controls;
    TitleCounter title;
    const MonotonicClock::time_point started = MonotonicClock::now();
    MonotonicClock::time_point last = started;
    while (!controls.quit) {
        controls.look = {};
        controls.faster_time = 0;
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
        flight =
            controls.walking
                ? client::walk(flight, input, faster, radius,
                               [&](WorldPosition at) { return client::ground_under(world, at); })
                : client::fly(flight, input, radius, client::ground_under(world, flight.eye),
                              {.speed = controls.speed * (faster ? fast : 1.0)});
        const double ground_height = client::ground_under(world, flight.eye);
        for (; controls.faster_time > 0; --controls.faster_time) {
            clock.faster();
        }
        for (; controls.faster_time < 0; ++controls.faster_time) {
            clock.slower();
        }
        clock.advance(seconds);
        title.count(
            window, glm::length(flight.eye.metres()) - radius - ground_height,
            scene.calendar().local_time_of_day(clock.time(), glm::normalize(flight.eye.metres())),
            clock.rate());

        if (options.quit_after && now - started >= *options.quit_after) {
            controls.quit = true;
        }
        const DVec3 companion_up = client::companion_direction(flight, companion_ahead);
        const double ground_ahead =
            client::ground_under(world, WorldPosition{companion_up * radius});
        const render::Standing companion{
            .feet = controls.walking
                        ? WorldPosition{companion_up * (radius + ground_ahead)}
                        : client::companion_feet(flight, companion_ahead, companion_below, radius,
                                                 ground_ahead),
            .up = companion_up,
            .height = companion_height};
        const terrain::Bend bend = terrain::bend_from(flight.eye, radius, ground_height);
        const client::Shot shot{.camera = client::flight_camera(flight, vertical_fov),
                                .bend = bend,
                                .time = clock.time(),
                                .seconds = seconds,
                                .companion = companion};
        const auto drawn = draw(scene, shot, device, presenter, window,
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
    auto billboard = render::Billboard::create(device, colour_format, client::depth_format,
                                               {.width = picture->width, .height = picture->height},
                                               picture->texels);
    if (!billboard) {
        std::cerr << std::format("hog-client: flying alone: {}\n", billboard.error());
        return std::nullopt;
    }
    return std::move(billboard).value();
}

int show(const client::Options& options, const planet::PlanetConfig& planet,
         const planet::World& world, render::Device& device, render::Presenter& presenter,
         SDL_Window* window) {
    const client::Flight flight = start(options, world);
    const sim::Calendar calendar{planet.day_length, planet.axial_tilt};
    client::PlanetClock clock{client::start_time(calendar, to_game_time(WallClock::now()),
                                                 glm::normalize(flight.eye.metres()),
                                                 options.time_of_day)};
    // Before the scene, whose bakes run on it.
    jobs::JobSystem jobs{options.threads};
    auto scene = client::Scene::create(device, world, jobs, calendar,
                                       presenter.swapchain().format(), render::frames_in_flight,
                                       companion(options, device, presenter.swapchain().format()));
    if (!scene) {
        std::cerr << std::format("hog-client: {}\n", scene.error());
        return EXIT_FAILURE;
    }
    const Result<void> flown =
        fly(*scene, clock, world, flight, options, device, presenter, window);
    // The GPU may still be drawing with the scene.
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
    return show(*options, *planet, world, **device, **presenter, window.get());
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

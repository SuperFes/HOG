#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/frame_times.hpp>
#include <hog/client/options.hpp>
#include <hog/client/picture.hpp>
#include <hog/client/scene.hpp>
#include <hog/client/session.hpp>
#include <hog/core/log.hpp>
#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/login/service.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/net/client.hpp>
#include <hog/net/simulation.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/billboard.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/presenter.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/shader_watch.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/window/ui.hpp>
#include <hog/window/window.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

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
#include <variant>
#include <vector>

namespace {

using namespace hog;

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
// How often to look for edited shaders.
constexpr std::chrono::milliseconds shader_looks{250};
// Someone 3 m tall flying along 8 m ahead, 2 m below the eye, so they land in front of it.
constexpr double companion_height = 3.0;
constexpr double companion_ahead = 8.0;
constexpr double companion_below = 2.0;

// What the person flying or walking asks for between frames.
struct Controls {
    DVec2 look{};
    double speed = client::FlySettings{}.speed;
    bool walking = false;
    bool timings = false; // shown over the picture
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
        } else if (event.key.key == SDLK_F3 && !event.key.repeat) {
            controls.timings = !controls.timings;
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

// How long frames take, in a corner over the picture.
void show_timings(const client::FrameTimes& times) {
    constexpr float margin = 10.0F;
    constexpr ImVec2 graph{240.0F, 48.0F};
    // Milliseconds, as at 30 fps, or higher to hold the worst frame.
    constexpr double least_top = 1'000.0 / 30.0;
    ImGui::SetNextWindowPos({margin, margin});
    ImGui::SetNextWindowBgAlpha(0.6F);
    if (ImGui::Begin("timings", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        const client::Timing cpu = times.cpu();
        ImGui::TextUnformatted(std::format("{:.0f} fps, {:.1f} ms a frame, worst {:.1f}",
                                           cpu.mean > 0.0 ? 1'000.0 / cpu.mean : 0.0, cpu.mean,
                                           cpu.worst)
                                   .c_str());
        const std::vector<float> history = times.history();
        ImGui::PlotLines("##frames", history.data(), static_cast<int>(history.size()), 0, nullptr,
                         0.0F, static_cast<float>(std::max(cpu.worst, least_top)), graph);
        const std::vector<client::Timing> gpu = times.gpu();
        if (gpu.empty()) {
            ImGui::TextUnformatted("the GPU can't time its passes");
        } else if (ImGui::BeginTable("gpu", 3)) {
            ImGui::TableSetupColumn("pass");
            ImGui::TableSetupColumn("ms");
            ImGui::TableSetupColumn("worst");
            ImGui::TableHeadersRow();
            for (const client::Timing& pass : gpu) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string{pass.name}.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::format("{:.2f}", pass.mean).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::format("{:.2f}", pass.worst).c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// How the connection to the server is doing, and who's near.
void show_network(const client::SessionSummary& summary) {
    constexpr float margin = 10.0F;
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - margin, margin}, ImGuiCond_Always,
                            {1.0F, 0.0F});
    ImGui::SetNextWindowBgAlpha(0.6F);
    if (ImGui::Begin("network", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        for (const std::string* line :
             {&summary.connection, &summary.quality, &summary.traffic, &summary.snapshots}) {
            if (!line->empty()) {
                ImGui::TextUnformatted(line->c_str());
            }
        }
        if (summary.nearby.empty()) {
            ImGui::TextUnformatted("no one near");
        } else if (ImGui::BeginTable("nearby", 2)) {
            ImGui::TableSetupColumn("near");
            ImGui::TableSetupColumn("m");
            ImGui::TableHeadersRow();
            for (const client::SessionSummary::Seen& seen : summary.nearby) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(seen.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::format("{:.0f}", seen.distance).c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// What's drawn over the picture.
struct Overlay {
    const client::FrameTimes* times = nullptr;
    // None when not connecting to a server.
    const client::Session* session = nullptr;
    WorldPosition eye;
    MonotonicClock::time_point now;
};

// Draws a frame of the shot, with the overlay over it if given, saving it to `capture` if given;
// false when there's nothing to draw into.
Result<bool> draw(client::Scene& scene, const client::Shot& shot, const render::Device& device,
                  render::Presenter& presenter, SDL_Window* window, const Overlay* overlay,
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
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (overlay != nullptr) {
        show_timings(*overlay->times);
        if (overlay->session != nullptr) {
            show_network(client::summarise(*overlay->session, overlay->eye, overlay->now));
        }
    }
    ImGui::Render();
    if (overlay != nullptr) {
        render::begin_rendering(frame.commands, frame.view, frame.extent, std::nullopt);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commands);
        vkCmdEndRendering(frame.commands);
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

const log::Category connection_log{"connection"};

// The connection to a server, if there's one to connect to, telling what becomes of it: after
// logging in for a connect token, when asked to.
class Connection {
public:
    [[nodiscard]] static Result<Connection> open(const client::Options& options) {
        Connection connection;
        if (!options.connect) {
            return connection;
        }
        connection.address_ = *options.connect;
        if (!options.simulate.off()) {
            net::simulate(options.simulate);
            log::warn(connection_log, "simulating a bad network: {}",
                      net::describe(options.simulate));
        }
        if (options.login) {
            auto login = net::Client::connect(*options.login);
            if (!login) {
                return std::unexpected(login.error());
            }
            connection.login_ = std::move(*login);
            connection.login_address_ = *options.login;
            connection.name_ = options.name.value_or("");
            log::info(connection_log, "logging in at {} as {}", *options.login, connection.name_);
            return connection;
        }
        if (auto started = connection.connect(); !started) {
            return std::unexpected(started.error());
        }
        return connection;
    }

    // Tells the server the player is at `eye`, once in.
    void look(WorldPosition eye, MonotonicClock::time_point now) {
        if (login_) {
            look_at_login(*login_);
        }
        if (session_) {
            session_->look(eye, now);
        }
    }

    // None until connecting to the server.
    [[nodiscard]] const client::Session* session() const noexcept {
        return session_ ? &*session_ : nullptr;
    }

private:
    [[nodiscard]] Result<void> connect() {
        auto session = client::Session::connect(address_, token_);
        if (!session) {
            return std::unexpected(session.error());
        }
        session_ = std::move(*session);
        return {};
    }

    void look_at_login(net::Client& login) {
        login.poll(events_);
        for (const net::Event& event : events_) {
            if (std::holds_alternative<net::Connected>(event)) {
                if (auto sent = login.send(
                        net::Delivery::Reliable,
                        login::encode(login::Ask{.version = login::login_version, .name = name_}));
                    !sent) {
                    log::warn(connection_log, "couldn't log in: {}", sent.error());
                }
            } else if (const auto* message = std::get_if<net::Message>(&event)) {
                logged_in(message->bytes());
                login.disconnect("answered");
                login_.reset();
                return;
            } else if (const auto* lost = std::get_if<net::Disconnected>(&event)) {
                log::warn(connection_log, "the login service at {} closed: {}", login_address_,
                          lost->reason);
                login_.reset();
                return;
            }
        }
    }

    // What the login service answered: on with its token, or no further.
    void logged_in(std::span<const std::byte> bytes) {
        const auto message = login::decode(bytes);
        if (!message) {
            log::warn(connection_log, "the login service sent something unreadable: {}",
                      message.error());
        } else if (const auto* granted = std::get_if<login::Granted>(&*message)) {
            token_ = granted->token;
            log::info(connection_log, "logged in as {}", name_);
            if (auto started = connect(); !started) {
                log::warn(connection_log, "couldn't connect to {}: {}", address_, started.error());
            }
        } else if (const auto* denied = std::get_if<login::Denied>(&*message)) {
            log::warn(connection_log, "the login was denied: {}", denied->reason);
        }
    }

    std::optional<net::Client> login_;
    std::optional<client::Session> session_;
    std::string login_address_;
    std::string address_;
    std::string name_;
    std::vector<std::byte> token_;
    std::vector<net::Event> events_;
};

// Looks for edited shaders now and then, drawing with them once they compile.
class ShaderReloader {
public:
    explicit ShaderReloader(bool reloading) {
        if (reloading) {
            watch_.emplace(render::shader_catalogue());
        }
    }

    void look(const render::Device& device, MonotonicClock::time_point now) {
        if (!watch_ || now - looked_ < shader_looks) {
            return;
        }
        looked_ = now;
        render::reload_changed(*watch_, device.pipelines());
    }

private:
    std::optional<render::ShaderWatch> watch_;
    MonotonicClock::time_point looked_ = MonotonicClock::now();
};

// Flies or walks over the world until the window closes or it's time to quit.
Result<void> fly(client::Scene& scene, client::PlanetClock& clock, const planet::World& world,
                 client::Flight flight, Connection& connection, const client::Options& options,
                 const render::Device& device, render::Presenter& presenter, SDL_Window* window) {
    const double radius = world.geology().planet_radius();
    Controls controls;
    controls.timings = options.timings;
    TitleCounter title;
    client::FrameTimes times;
    const MonotonicClock::time_point started = MonotonicClock::now();
    MonotonicClock::time_point last = started;
    ShaderReloader shaders{options.reload_shaders};
    while (!controls.quit) {
        controls.look = {};
        controls.faster_time = 0;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handle(event, window, controls);
        }
        const MonotonicClock::time_point now = MonotonicClock::now();
        const double passed = std::chrono::duration<double>(now - last).count();
        const double seconds = std::min(passed, longest_frame);
        last = now;
        times.add(passed, scene.timings());
        shaders.look(device, now);
        connection.look(flight.eye, now);
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
        const client::Shot shot{.camera = client::flight_camera(flight, client::vertical_fov),
                                .bend = bend,
                                .time = clock.time(),
                                .seconds = seconds,
                                .companion = companion};
        const Overlay overlay{
            .times = &times, .session = connection.session(), .eye = flight.eye, .now = now};
        const auto drawn =
            draw(scene, shot, device, presenter, window, controls.timings ? &overlay : nullptr,
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
    auto connection = Connection::open(options);
    if (!connection) {
        std::cerr << std::format("hog-client: {}\n", connection.error());
        return EXIT_FAILURE;
    }
    const client::Flight flight = start(options, world);
    const sim::Calendar calendar{planet.day_length, planet.axial_tilt};
    client::PlanetClock clock{client::start_time(calendar, to_game_time(WallClock::now()),
                                                 glm::normalize(flight.eye.metres()),
                                                 options.time_of_day)};
    const window::Ui ui{window, device, presenter.swapchain().format()};
    if (!ui.started()) {
        std::cerr << "hog-client: can't draw the timings over the picture\n";
        return EXIT_FAILURE;
    }
    // Only shown, so it leaves the mouse and keys to flying.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange |
                                  ImGuiConfigFlags_NoKeyboard;
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
        fly(*scene, clock, world, flight, *connection, options, device, presenter, window);
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

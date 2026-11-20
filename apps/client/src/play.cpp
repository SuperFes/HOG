#include <hog/chat/filter.hpp>
#include <hog/client/block_meshes.hpp>
#include <hog/client/chat.hpp>
#include <hog/client/chunk_cache.hpp>
#include <hog/client/daylight.hpp>
#include <hog/client/drawn.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/forecast.hpp>
#include <hog/client/frame_times.hpp>
#include <hog/client/globe_view.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/options.hpp>
#include <hog/client/others.hpp>
#include <hog/client/piece_asking.hpp>
#include <hog/client/play.hpp>
#include <hog/client/player.hpp>
#include <hog/client/report.hpp>
#include <hog/client/scene.hpp>
#include <hog/client/session.hpp>
#include <hog/client/sonar.hpp>
#include <hog/client/tile_meshes.hpp>
#include <hog/client/water_field.hpp>
#include <hog/core/image.hpp>
#include <hog/core/line_sink.hpp>
#include <hog/core/log.hpp>
#include <hog/core/profile.hpp>
#include <hog/core/time.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/world.hpp>
#include <hog/grid/grid.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/login/service.hpp>
#include <hog/net/client.hpp>
#include <hog/net/simulation.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/render/cloud_quality.hpp>
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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace hog;

constexpr double start_pitch = -0.2;   // radians
constexpr double fast = 5.0;           // times as fast with Shift held
constexpr double speed_step = 1.25;    // per notch of the mouse wheel
constexpr double slowest_speed = 0.01; // heights a second
constexpr double fastest_speed = 10.0;
// A frame's movement is held to this many seconds, so a stall doesn't fling the eye away.
constexpr double longest_frame = 0.1;
// How long to wait between looks at a minimised window.
constexpr Uint32 idle_ms = 50;
// How often to look for edited shaders.
constexpr std::chrono::milliseconds shader_looks{250};
// What it says it is, in what it tells the person.
constexpr std::string_view program = "hog-client";
// Metres the camera stays behind the player, and how near and far the wheel takes it.
constexpr double behind = 4.0;
constexpr double nearest_behind = 1.5;
constexpr double furthest_behind = 20.0;

// What the person flying or walking asks for between frames.
struct Controls {
    DVec2 look{};
    double speed = client::FlySettings{}.speed;
    double behind = ::behind; // metres, while walking
    bool walking = false;
    bool ping = false;        // asked to look for air pockets under the ground about the eye
    bool through = false;     // flying through the ground
    double cutaway = 8.0;     // metres of ground round the eye left undrawn there
    bool timings = false;     // shown over the picture
    bool next_clouds = false; // asked to draw the clouds at the next quality
    bool may_chat = false;    // connected to a server to chat on
    bool chatting = false;    // typing into the chat box, which takes the keys
    bool quit = false;
    bool report = false; // asked to save a report of what's shown
    bool dig = false;    // asked to dig a ball out of the ground ahead
    bool build = false;  // asked to build one up
    // Doublings of the planet's time asked for, halvings if negative.
    int faster_time = 0;
    // The same for the server's whole world, where it allows time control.
    int faster_world = 0;
};

// A key pressed afresh, not held down to repeat: the toggles and one-off asks.
void press_afresh(SDL_Keycode key, Controls& controls) {
    switch (key) {
    case SDLK_RETURN:
        if (controls.may_chat) {
            controls.chatting = true;
        }
        break;
    case SDLK_F:
        // Nowhere to stand but the surface, so a ghost leaves that to G.
        controls.walking = !controls.walking && !controls.through;
        break;
    case SDLK_P:
        controls.ping = true;
        break;
    case SDLK_G:
        controls.through = !controls.through;
        controls.walking = controls.walking && !controls.through;
        break;
    case SDLK_F3:
        controls.timings = !controls.timings;
        break;
    case SDLK_F4:
        controls.next_clouds = true;
        break;
    case SDLK_F12:
        controls.report = true;
        break;
    case SDLK_X:
        controls.dig = true;
        break;
    case SDLK_V:
        controls.build = true;
        break;
    default:
        break;
    }
}

// `]` doubles the planet's time and `[` halves it; with shift, the server's whole world's.
void press_time(const SDL_KeyboardEvent& key, Controls& controls) {
    int& doublings = (key.mod & SDL_KMOD_SHIFT) != 0 ? controls.faster_world : controls.faster_time;
    if (key.key == SDLK_RIGHTBRACKET) {
        ++doublings;
    } else {
        --doublings;
    }
}

// A key pressed. While typing chat only Esc does anything here; the rest go to the chat box.
void press(const SDL_KeyboardEvent& key, Controls& controls) {
    if (controls.chatting) {
        if (key.key == SDLK_ESCAPE) {
            controls.chatting = false;
        }
        return;
    }
    if (key.key == SDLK_ESCAPE) {
        controls.quit = true;
    } else if (key.key == SDLK_RIGHTBRACKET || key.key == SDLK_LEFTBRACKET) {
        press_time(key, controls);
    } else if (!key.repeat) {
        press_afresh(key.key, controls);
    }
}

void handle(const SDL_Event& event, SDL_Window* window, Controls& controls) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
        controls.quit = true;
        break;
    case SDL_EVENT_KEY_DOWN:
        press(event.key, controls);
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
            controls.behind = std::clamp(
                controls.behind / std::pow(speed_step, static_cast<double>(event.wheel.y)),
                nearest_behind, furthest_behind);
            break;
        }
        if (controls.through) {
            controls.cutaway = std::clamp(
                controls.cutaway * std::pow(speed_step, static_cast<double>(event.wheel.y)), 1.0,
                render::block_cutaway_most);
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

// The keys held down, as movement; none while they're typing chat.
DVec3 movement(const Controls& controls) {
    if (controls.chatting) {
        return {};
    }
    int count = 0;
    const std::span held{SDL_GetKeyboardState(&count), static_cast<std::size_t>(count)};
    const auto axis = [&](SDL_Scancode more, SDL_Scancode less) {
        return (held[more] ? 1.0 : 0.0) - (held[less] ? 1.0 : 0.0);
    };
    return {axis(SDL_SCANCODE_D, SDL_SCANCODE_A), axis(SDL_SCANCODE_SPACE, SDL_SCANCODE_C),
            axis(SDL_SCANCODE_W, SDL_SCANCODE_S)};
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
    return write_png(path, Image{.width = static_cast<int>(frame.extent.width),
                                 .height = static_cast<int>(frame.extent.height),
                                 .pixels = std::move(rgb).value()});
}

// How long frames take, in a corner over the picture.
void show_timings(const client::FrameTimes& times, render::CloudQuality clouds) {
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
        ImGui::TextUnformatted(std::format("clouds {} (F4)", render::name_of(clouds)).c_str());
    }
    ImGui::End();
}

// How the connection to the server is doing, the weather it says is over the eye, and who's near.
void show_network(const client::SessionSummary& summary, const std::string& weather) {
    constexpr float margin = 10.0F;
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - margin, margin}, ImGuiCond_Always,
                            {1.0F, 0.0F});
    ImGui::SetNextWindowBgAlpha(0.6F);
    if (ImGui::Begin("network", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        for (const std::string* line : {&summary.connection, &summary.quality, &summary.traffic,
                                        &summary.snapshots, &summary.clock, &weather}) {
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

// The chat box: the lines heard, and the one being typed while it's open.
struct ChatBox {
    client::ChatLog log;
    std::array<char, protocol::most_said + 1> typed{};
    std::optional<std::string> said; // finished with Enter this frame
};

// The chat over the picture, bottom left: what was said lately and, while `chatting`, the line
// being typed, sent with Enter. `opened` skips the very Enter that opened the box.
void show_chat(ChatBox& chat, bool& chatting, bool opened, MonotonicClock::time_point now) {
    constexpr float margin = 10.0F;
    constexpr float width = 360.0F;
    const std::span<const client::ChatLog::Line> all = chat.log.lines();
    const std::span<const client::ChatLog::Line> lines =
        chatting ? all.last(std::min(all.size(), client::ChatLog::most_fresh))
                 : chat.log.fresh(now);
    if (lines.empty() && !chatting) {
        return;
    }
    ImGui::SetNextWindowPos({margin, ImGui::GetIO().DisplaySize.y - margin}, ImGuiCond_Always,
                            {0.0F, 1.0F});
    ImGui::SetNextWindowBgAlpha(0.6F);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
    if (!chatting) {
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
    }
    if (ImGui::Begin("chat", nullptr, flags)) {
        for (const client::ChatLog::Line& line : lines) {
            ImGui::PushTextWrapPos(width);
            ImGui::TextUnformatted(std::format("{}: {}", line.from, line.text).c_str());
            ImGui::PopTextWrapPos();
        }
        if (chatting) {
            ImGui::SetNextItemWidth(width);
            if (!ImGui::IsAnyItemActive()) {
                ImGui::SetKeyboardFocusHere();
            }
            if (ImGui::InputText("##say", chat.typed.data(), chat.typed.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue) &&
                !opened) {
                chat.said = std::string{chat.typed.data()};
                chat.typed[0] = '\0';
                chatting = false;
            }
        }
    }
    ImGui::End();
}

// Others' names over them, darkened behind so they read against the sky.
void show_tags(std::span<const client::Tag> tags) {
    constexpr ImU32 shadow = IM_COL32(0, 0, 0, 160);
    constexpr ImU32 white = IM_COL32(255, 255, 255, 255);
    ImDrawList& drawn = *ImGui::GetBackgroundDrawList();
    for (const client::Tag& tag : tags) {
        const char* begin = tag.name.data();
        const char* end = std::to_address(tag.name.end());
        const ImVec2 text = ImGui::CalcTextSize(begin, end);
        const ImVec2 corner{static_cast<float>(tag.at.x) - (text.x / 2.0F),
                            static_cast<float>(tag.at.y) - text.y};
        drawn.AddText({corner.x + 1.0F, corner.y + 1.0F}, shadow, begin, end);
        drawn.AddText(corner, white, begin, end);
    }
}

// The pockets a ping found, marked over the picture in cyan.
void show_blips(std::span<const client::Blip> blips) {
    constexpr ImU32 shadow = IM_COL32(0, 0, 0, 180);
    constexpr ImU32 cyan = IM_COL32(90, 230, 255, 255);
    ImDrawList& drawn = *ImGui::GetBackgroundDrawList();
    for (const client::Blip& blip : blips) {
        const char* begin = blip.text.c_str();
        const char* end = std::to_address(blip.text.end());
        const ImVec2 text = ImGui::CalcTextSize(begin, end);
        const ImVec2 corner{static_cast<float>(blip.at.x) - (text.x / 2.0F),
                            static_cast<float>(blip.at.y) - (text.y / 2.0F)};
        if (blip.on_screen) {
            drawn.AddCircle({static_cast<float>(blip.at.x), static_cast<float>(blip.at.y)}, 10.0F,
                            cyan, 0, 2.0F);
        }
        drawn.AddText({corner.x + 1.0F, corner.y + 1.0F + (blip.on_screen ? 16.0F : 0.0F)}, shadow,
                      begin, end);
        drawn.AddText({corner.x, corner.y + (blip.on_screen ? 16.0F : 0.0F)}, cyan, begin, end);
    }
}

// What's drawn over the picture.
struct Overlay {
    const client::FrameTimes* times = nullptr;
    bool timings = false;
    render::CloudQuality clouds = render::CloudQuality::High;
    // None when not connecting to a server.
    const client::Session* session = nullptr;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const replication::Shown> others = {};
    WorldPosition eye;
    // What the last ping found, if one's been made.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const globe::Pocket> pockets = {};
    MonotonicClock::time_point now;
    std::string weather; // over the eye, once known
    // None when not connecting to a server.
    ChatBox* chat = nullptr;
    bool* chatting = nullptr;
    bool chat_opened = false; // this frame
};

// Draws a frame of the shot with the overlay over it, saving it to `capture` if given; false when
// there's nothing to draw into.
Result<bool> draw(client::Scene& scene, const client::Shot& shot, const render::Device& device,
                  render::Presenter& presenter, SDL_Window* window, const Overlay& overlay,
                  const std::optional<std::filesystem::path>& capture) {
    HOG_ZONE("draw");
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
    {
        HOG_ZONE("overlay");
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        show_tags(client::tags(overlay.others, shot.camera, shot.bend, {size.x, size.y}));
        show_blips(client::blips(overlay.pockets, shot.camera, shot.bend, {size.x, size.y}, 40.0));
        if (overlay.chat != nullptr) {
            show_chat(*overlay.chat, *overlay.chatting, overlay.chat_opened, overlay.now);
        }
        if (overlay.timings) {
            show_timings(*overlay.times, overlay.clouds);
            if (overlay.session != nullptr) {
                show_network(client::summarise(*overlay.session, overlay.eye, overlay.now),
                             overlay.weather);
            }
        }
        ImGui::Render();
        if (ImGui::GetDrawData()->TotalVtxCount > 0) {
            render::begin_rendering(frame.commands, frame.view, frame.extent, std::nullopt);
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commands);
            vkCmdEndRendering(frame.commands);
        }
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
            std::cerr << std::format("{}: can't capture the window: {}\n", program, saved.error());
        }
    }
    return true;
}

// Where the eye is and which way it looks, as --at takes it.
client::Place seen_from(const client::Flight& flight, DVec3 eye, double radius) {
    constexpr double degree = std::numbers::pi / 180.0;
    client::Place place = client::place_of(eye, radius);
    place.heading = client::bearing_of(flight) / degree;
    place.pitch = flight.pitch / degree;
    return place;
}

// Frames a second, height, the local time of day and how fast it runs, and where the eye is as
// --at takes it, in the window's title once a second.
class TitleCounter {
public:
    void count(SDL_Window* window, double height, double time_of_day, double rate,
               std::uint32_t world_rate, std::string_view at) {
        ++frames_;
        const MonotonicClock::time_point now = MonotonicClock::now();
        const std::chrono::duration<double> passed = now - since_;
        if (passed.count() >= 1.0) {
            const auto minutes = static_cast<int>(time_of_day * 24.0 * 60.0);
            const std::string title =
                std::format("HOG - {:.0f} fps, {:.0f} m up, {:02}:{:02}{}{}, at {}",
                            static_cast<double>(frames_) / passed.count(), height, minutes / 60,
                            minutes % 60, rate > 1.0 ? std::format(", time {:.0f}x", rate) : "",
                            world_rate != 1 ? std::format(", world {}x", world_rate) : "", at);
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
const log::Category player_log{"player"};
const log::Category clouds_log{"clouds"};

// The connection to a server, if there's one to connect to, telling what becomes of it: after
// logging in for a connect token, when asked to.
class Connection {
public:
    [[nodiscard]] static Result<Connection> open(const client::Options& options,
                                                 std::optional<std::uint64_t> world) {
        Connection connection;
        connection.world_ = world;
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

    // Tells the server where the player is, once in.
    void look(const client::Whereabouts& player, MonotonicClock::time_point now) {
        if (login_) {
            look_at_login(*login_);
        }
        if (session_) {
            session_->look(player, now);
        }
    }

    // Where the server put the player back to since last asked.
    [[nodiscard]] std::optional<WorldPosition> take_correction() noexcept {
        return session_ ? session_->take_correction() : std::nullopt;
    }
    // Where the server says the player was when they last left, once.
    [[nodiscard]] std::optional<protocol::Placed> take_placed() noexcept {
        return session_ ? session_->take_placed() : std::nullopt;
    }

    // The newest weather the server sent since last asked.
    [[nodiscard]] std::optional<protocol::Weather> take_weather() noexcept {
        return session_ ? session_->take_weather() : std::nullopt;
    }

    // Says `text` to everyone on the server, once in.
    void say(std::string_view text) {
        if (session_) {
            session_->say(text);
        }
    }

    // Asks the server to run the world at `scale` game seconds a real one, once in.
    void hurry(std::uint32_t scale) {
        if (session_) {
            session_->hurry(scale);
        }
    }

    // Asks the server for grid pieces besides those asked for before, and drops those no longer
    // wanted, once in.
    void ask(const protocol::AskPieces& ask) {
        if (session_) {
            session_->ask(ask);
        }
    }
    void report(std::uint64_t stamp) {
        if (session_) {
            session_->report(stamp);
        }
    }
    [[nodiscard]] bool welcomed() const noexcept {
        return session_ && session_->state() == client::Session::State::Welcomed;
    }

    // The chat heard since last asked.
    [[nodiscard]] std::vector<client::ChatLine> take_chat() {
        return session_ ? session_->take_chat() : std::vector<client::ChatLine>{};
    }

    // The tiles the server sent since last asked.
    [[nodiscard]] std::vector<protocol::SentTile> take_tiles() {
        return session_ ? session_->take_tiles() : std::vector<protocol::SentTile>{};
    }
    // The grid pieces and water bodies the server sent since last asked, as coded.
    [[nodiscard]] std::vector<protocol::SentChunk> take_chunks() {
        return session_ ? session_->take_chunks() : std::vector<protocol::SentChunk>{};
    }
    // Has the server check the chunks kept from before, once in.
    void check(const protocol::CheckChunks& check) {
        if (session_) {
            session_->check(check);
        }
    }
    [[nodiscard]] std::vector<protocol::DonePiece> take_done() {
        return session_ ? session_->take_done() : std::vector<protocol::DonePiece>{};
    }
    [[nodiscard]] std::vector<std::vector<std::byte>> take_bodies() {
        return session_ ? session_->take_bodies() : std::vector<std::vector<std::byte>>{};
    }

    // Answers the server and the login service, with nothing yet to say of where the player is.
    void pump(MonotonicClock::time_point now) {
        if (login_) {
            look_at_login(*login_);
        }
        if (session_) {
            session_->look(std::nullopt, now);
        }
    }

    // Whether edits go to the server: once welcomed by one.
    [[nodiscard]] bool editing_through_server() const noexcept {
        return session_ && session_->state() == client::Session::State::Welcomed;
    }
    void dig(WorldPosition centre, double radius) {
        if (session_) {
            session_->dig(centre, radius);
        }
    }
    void build(WorldPosition centre, double radius, std::uint8_t material) {
        if (session_) {
            session_->build(centre, radius, material);
        }
    }

    // None until connecting to the server.
    [[nodiscard]] const client::Session* session() const noexcept {
        return session_ ? &*session_ : nullptr;
    }

private:
    [[nodiscard]] Result<void> connect() {
        auto session = client::Session::connect(address_, token_, world_);
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
    // The planet this client makes ground of, if it does.
    std::optional<std::uint64_t> world_;
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

// Sends the server where the player's feet are, or their eye flying, puts them back where it
// says, and takes the weather and chat it sends, chat censored unless the player turned that off.
void report(Connection& connection, std::optional<client::Player>& player,
            const client::Flight& flight, client::Forecast& forecast, ChatBox& chat, bool filter,
            MonotonicClock::time_point now) {
    HOG_ZONE("network");
    connection.look({.at = player ? player->feet() : flight.eye,
                     .stance = player ? protocol::Stance::Walking : protocol::Stance::Flying,
                     .facing = client::facing_of(flight)},
                    now);
    if (const auto back = connection.take_correction(); back && player) {
        player->place(*back);
    }
    if (const auto weather = connection.take_weather()) {
        forecast.receive(*weather, now);
    }
    forecast.update(now);
    for (const client::ChatLine& line : connection.take_chat()) {
        chat.log.add(hog::chat::shown_name(line.from, filter),
                     hog::chat::shown_chat(line.text, filter), now);
    }
}

// A ball this far ahead of the eye, this wide, is dug or built on the keys.
constexpr double edit_ahead = 3.0;  // metres
constexpr double edit_radius = 1.5; // metres
constexpr auto edit_material = static_cast<std::uint8_t>(globe::Material::Granite);

// While typing chat the keys go to its box, not the game; back when it closes. Whether the box
// opened just now, so the Enter that opened it isn't taken as sending.
[[nodiscard]] bool route_keys(SDL_Window* window, bool chatting, bool& routed_to_chat) {
    if (chatting == routed_to_chat) {
        return false;
    }
    routed_to_chat = chatting;
    if (chatting) {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoKeyboard;
        SDL_StartTextInput(window);
    } else {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
        SDL_StopTextInput(window);
    }
    return chatting;
}

// Metres the ground drawn stands over the radius under a point.
double drawn_ground(const globe::World& world, WorldPosition at) {
    return globe::ground_height(world, at.metres());
}

// Starts or stops playing the character as asked: stood on the ground under the eye, or flying
// on from where the camera was.

void take_part(Controls& controls, std::optional<client::Player>& player, client::Flight& flight,
               const render::Camera& camera, const globe::World& world) {
    if (controls.walking == player.has_value()) {
        return;
    }
    if (player) {
        player.reset();
        flight.eye = camera.eye;
        return;
    }
    auto made = client::Player::create(world, glm::normalize(flight.eye.metres()));
    if (!made) {
        log::warn(player_log, "can't walk here: {}", made.error());
        controls.walking = false;
        return;
    }
    player = std::move(made).value();
}

// Plays the character or flies a frame on, as the controls ask; what the camera then sees.
render::Camera move(const Controls& controls, std::optional<client::Player>& player,
                    client::Flight& flight, const globe::World& world, double seconds) {
    HOG_ZONE("move");
    const bool faster = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    if (!player) {
        flight = client::fly(
            flight, {.move = movement(controls), .look = controls.look, .seconds = seconds},
            world.planet().body.radius, drawn_ground(world, flight.eye),
            {.speed = controls.speed * (faster ? fast : 1.0),
             .through = controls.through,
             .underground = client::FlySettings{}.underground * (faster ? fast : 1.0)});
        return client::flight_camera(flight, client::vertical_fov);
    }
    flight = client::looked(flight, controls.look);
    if (auto played = player->play(client::stride(flight, movement(controls), faster), seconds);
        !played) {
        log::warn(player_log, "can't keep the ground: {}", played.error());
    }
    flight = client::carried_to(flight, player->shoulder());
    const DVec3 back = -client::flight_camera(flight, client::vertical_fov).forward;
    return client::third_person(flight, player->room(flight.eye, back, controls.behind),
                                client::vertical_fov);
}

// Sends what the chat box finished, if anything; an empty line says nothing.
void say_chat(ChatBox& chat, Connection& connection) {
    connection.say(std::exchange(chat.said, std::nullopt).value_or(std::string{}));
}

// The world's rate as the server last said it, 1 without a session.
std::uint32_t world_rate(const Connection& connection) {
    const client::Session* session = connection.session();
    return session != nullptr ? session->world_rate() : 1;
}

// Doubles or halves the local clock as `]` and `[` asked.
void change_pace(Controls& controls, client::PlanetClock& clock) {
    for (; controls.faster_time > 0; --controls.faster_time) {
        clock.faster();
    }
    for (; controls.faster_time < 0; ++controls.faster_time) {
        clock.slower();
    }
}

// Asks the server to double or halve the world's time as `{` and `}` asked, from the rate it
// last said it runs at; the server obliges only where its config allows time control.
void hurry_world(Controls& controls, Connection& connection) {
    const int doublings = std::exchange(controls.faster_world, 0);
    const client::Session* session = connection.session();
    if (doublings == 0 || session == nullptr) {
        return;
    }
    const double rate =
        static_cast<double>(session->world_rate()) * std::pow(2.0, static_cast<double>(doublings));
    connection.hurry(static_cast<std::uint32_t>(std::clamp(rate, 1.0, client::fastest_time)));
}

// The clouds at the next quality, if asked for; as they were if it can't be made.
void change_clouds(Controls& controls, client::Scene& scene, const render::Device& device) {
    if (!std::exchange(controls.next_clouds, false)) {
        return;
    }
    // What changing quality remakes mustn't be in a frame the GPU's still drawing.
    vkDeviceWaitIdle(device.handle());
    const render::CloudQuality quality = render::next(scene.cloud_quality());
    if (auto set = scene.set_cloud_quality(quality); !set) {
        log::warn(clouds_log, "can't draw the clouds at {} quality: {}", render::name_of(quality),
                  set.error());
    }
}

// Starts a frame's controls afresh and takes in the events since the last.
void take_events(Controls& controls, const Connection& connection, SDL_Window* window) {
    controls.look = {};
    controls.faster_time = 0;
    controls.may_chat = connection.session() != nullptr;
    HOG_ZONE("events");
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        handle(event, window, controls);
    }
}

// Whether the run asked to quit after a while has had it.
bool time_up(const client::Options& options, MonotonicClock::time_point started,
             MonotonicClock::time_point now) {
    return options.quit_after && now - started >= *options.quit_after;
}

// The ground bent down past the horizon as a smaller planet's would be, unless asked not to.
terrain::BendSettings bend_settings(const client::Options& options) {
    return options.bend ? terrain::BendSettings{} : terrain::BendSettings{.faked_radius = 0.0};
}

// The others the server shows about at `now`; none without a session.
std::vector<replication::Shown> others_about(const client::Session* session,
                                             MonotonicClock::time_point now) {
    return session != nullptr ? session->others(now) : std::vector<replication::Shown>{};
}

// What's drawn as capsules: the others about, the grains the server has flying, voxels
// `thickness` metres deep falling under `gravity`, and the player, if playing.
std::vector<render::Capsule> capsules_shown(const client::Session* session,
                                            std::span<const replication::Shown> others,
                                            const std::optional<client::Player>& player,
                                            MonotonicClock::time_point now, double thickness,
                                            double gravity) {
    std::vector<render::Capsule> avatars = client::avatars(others);
    if (session != nullptr) {
        const std::vector<render::Capsule> grains =
            session->grains().capsules(now, thickness, gravity);
        avatars.insert(avatars.end(), grains.begin(), grains.end());
    }
    if (player) {
        // The server doesn't say who the player is, so they take the colour of no one.
        avatars.push_back(client::avatar(player->feet(), client::colour_of(protocol::NetId{})));
    }
    return avatars;
}

// The weather over the eye in words, while timings are shown and the weather's known.
std::string weather_words(bool timings, const client::Forecast& forecast,
                          const client::Shot& shot) {
    return timings && forecast.known()
               ? client::describe(client::weather_at_eye(shot), client::air_at_eye(shot))
               : std::string{};
}

// Draws the frame, saving it to `capture` if given, and sends what the chat box finished; idles a
// moment when there's nothing to draw into.
Result<void> show_frame(client::Scene& scene, const client::Shot& shot,
                        const render::Device& device, render::Presenter& presenter,
                        SDL_Window* window, const Overlay& overlay,
                        const std::optional<std::filesystem::path>& capture, ChatBox& chat,
                        Connection& connection) {
    const auto drawn = draw(scene, shot, device, presenter, window, overlay, capture);
    if (!drawn) {
        return std::unexpected(drawn.error());
    }
    say_chat(chat, connection);
    if (!*drawn) {
        SDL_Delay(idle_ms);
    }
    return {};
}

// Levels of cells in a block the ground's drawn in from the grid: 16 a side.
constexpr std::uint8_t block_cells = 4;

// The ground the server sends, held as it comes: grid pieces, drawn; and any tiles, over the planet
// and its layers, read by the world for footing.
struct StreamedGround {
    StreamedGround(globe::Planet held, const globe::Layers& layers, jobs::JobSystem& jobs)
        : planet{std::move(held)}, world{this->planet, layers, volume},
          grid{layers.ceiling(), layers.thickness}, pieces{grid},
          block_meshes{grid, jobs, block_cells} {
        world.read_tiles(&tiles);
    }
    StreamedGround(const StreamedGround&) = delete;
    StreamedGround& operator=(const StreamedGround&) = delete;
    StreamedGround(StreamedGround&&) = delete;
    StreamedGround& operator=(StreamedGround&&) = delete;
    ~StreamedGround() = default;

    globe::Planet planet;
    globe::Volume volume; // none: the ground is the tiles sent
    globe::HeldTiles tiles;
    globe::World world;
    grid::Grid grid;
    client::HeldPieces pieces;
    client::BlockMeshes block_meshes;
    // The chunks kept from before, in a database of this client's own, and the wishes looked up
    // in it since.
    std::optional<client::ChunkCache> cache;
    client::Looked looked;
    // The pieces asked for and not yet done, from where and when last asked, and whether any
    // came since.
    client::PieceRequests requests;
    std::optional<DVec3> asked_from;
    DVec3 asked_facing{};
    MonotonicClock::time_point asked_at;
    bool asked_came = false;
    bool pieces_came = false;         // since what's drawn of them was last worked out
    std::optional<DVec3> pieces_seen; // where from
    bool blocks_shown = false;        // any drawn from the pieces yet
};

// The lake's or river's water the eye may be in, as the ground held round it draws it.
std::optional<render::StillAtEye> still_at_eye(const StreamedGround& ground, WorldPosition eye) {
    const double radius = ground.planet.body.radius;
    const std::optional<globe::StandingWater> water =
        client::still_water_in(ground.pieces, eye.metres());
    if (!water) {
        return std::nullopt;
    }
    return render::StillAtEye{
        .level = water->top - radius, .rise = water->rise, .peak = water->peak};
}

// Metres the eye moves before what's drawn of the pieces is worked out again.
constexpr double pieces_moved = 4.0;

// Pieces wished for at once, and how often what's asked for is worked out again at most, as the
// eye moves, turns more than this cosine allows, or pieces come.
constexpr std::size_t pieces_asked = protocol::most_asked;
constexpr auto asking_interval = std::chrono::milliseconds{100};
constexpr double asking_turn = 0.94;

// Bytes of chunks kept between runs at most.
constexpr std::uint64_t cache_budget = std::uint64_t{1} << 30U;
// Wishes looked up in the cache at once.
constexpr std::size_t cache_reads = 512;

// Takes in what the cache keeps of the wishes not yet looked up there, nearest first, and has the
// server check it; whether any came.
bool read_cached(Connection& connection, StreamedGround& ground,
                 const std::vector<client::PieceWish>& wishes) {
    if (!ground.cache) {
        return false;
    }
    auto taken =
        client::take_kept(*ground.cache, ground.pieces, wishes, ground.looked, cache_reads);
    if (!taken) {
        log::warn(connection_log, "can't read the chunks kept: {}", taken.error());
        return false;
    }
    for (std::size_t from = 0; from < taken->size(); from += protocol::most_asked) {
        const auto to = std::min(taken->size(), from + protocol::most_asked);
        connection.check(
            protocol::CheckChunks{.chunks = {taken->begin() + static_cast<std::ptrdiff_t>(from),
                                             taken->begin() + static_cast<std::ptrdiff_t>(to)}});
    }
    ground.pieces_came = ground.pieces_came || !taken->empty();
    return !taken->empty();
}

// Asks the server for the pieces to see from `eye` looking along `facing` that aren't held as
// deep as they should be, when the eye has moved, turned or pieces have come since last asked.
void ask_pieces(Connection& connection, StreamedGround& ground, DVec3 eye, DVec3 facing) {
    const MonotonicClock::time_point now = MonotonicClock::now();
    if (!connection.welcomed() || now - ground.asked_at < asking_interval) {
        return;
    }
    if (ground.asked_from && !ground.asked_came &&
        glm::length(eye - *ground.asked_from) <= pieces_moved &&
        glm::dot(facing, ground.asked_facing) >= asking_turn) {
        return;
    }
    std::vector<client::PieceWish> wishes =
        client::pieces_to_ask(ground.pieces, eye, facing, grid::View{}, pieces_asked);
    // What's kept from before is held at once, and what it still lacks asked.
    if (read_cached(connection, ground, wishes)) {
        wishes = client::pieces_to_ask(ground.pieces, eye, facing, grid::View{}, pieces_asked);
    }
    ground.asked_at = now;
    ground.asked_from = eye;
    ground.asked_facing = facing;
    ground.asked_came = false;
    if (const std::optional<protocol::AskPieces> ask = ground.requests.ask(ground.pieces, wishes)) {
        connection.ask(*ask);
    }
}

// Takes in the pieces the server sent, and draws them as seen from `eye` once what was last
// wanted of them is drawn.
void take_pieces(Connection& connection, StreamedGround& ground, client::Scene& scene, DVec3 eye,
                 DVec3 facing) {
    HOG_ZONE("pieces");
    const std::vector<protocol::SentChunk> chunks = connection.take_chunks();
    for (const protocol::SentChunk& chunk : chunks) {
        if (auto taken = ground.pieces.take(chunk); !taken) {
            log::warn(connection_log, "can't hold a chunk sent: {}", taken.error());
        } else {
            ground.pieces_came = true;
            ground.asked_came = true;
        }
    }
    if (ground.cache && !chunks.empty()) {
        if (auto kept = ground.cache->keep(chunks); !kept) {
            log::warn(connection_log, "can't keep the chunks sent: {}", kept.error());
        }
    }
    if (const std::vector<protocol::DonePiece> done = connection.take_done(); !done.empty()) {
        ground.requests.done(done);
        ground.asked_came = true;
    }
    for (const std::vector<std::byte>& bytes : connection.take_bodies()) {
        if (auto taken = ground.pieces.take_bodies(bytes); !taken) {
            log::warn(connection_log, "can't hold water bodies sent: {}", taken.error());
        }
    }
    ask_pieces(connection, ground, eye, facing);
    if (ground.block_meshes.settled() && (ground.pieces_came || !ground.pieces_seen ||
                                          glm::length(eye - *ground.pieces_seen) > pieces_moved)) {
        ground.block_meshes.want(ground.pieces, eye);
        ground.pieces_came = false;
        ground.pieces_seen = eye;
    }
    if (client::GroundChanges changes = ground.block_meshes.collect(); !changes.empty()) {
        scene.show_ground(std::move(changes));
        ground.blocks_shown = true;
    }
}

// Takes in the tiles the server sent, held for the world and the player's footing and meshed to
// draw, and the pieces.
void take_ground(Connection& connection, StreamedGround& ground, client::Scene& scene,
                 std::optional<client::Player>& player, DVec3 eye, DVec3 facing) {
    HOG_ZONE("ground");
    for (const protocol::SentTile& sent : connection.take_tiles()) {
        auto tile = client::sent_tree(sent, ground.world.layers());
        if (!tile) {
            log::warn(connection_log, "can't hold a tile sent: {}", tile.error());
            continue;
        }
        ground.tiles.put(tile->key, tile->tree);
        if (player) {
            player->changed(tile->key);
        }
    }
    take_pieces(connection, ground, scene, eye, facing);
    // Drawn from the pieces, which reach all round the planet, the sea is its own surface
    // everywhere once any is drawn; till then, the disc.
    const double covered = ground.blocks_shown ? std::numbers::pi * ground.planet.body.radius : 0.0;
    scene.set_ground_state(covered, ground.block_meshes.in_flight() > 0);
}

// Starts or stops playing the character as asked, as take_part does; walking waits for the ground
// under the eye to be held as finely as it'll be sent.
void take_part_streamed(Controls& controls, std::optional<client::Player>& player,
                        client::Flight& flight, const render::Camera& camera,
                        const StreamedGround& ground) {
    if (controls.walking && !player &&
        !client::ground_settled(ground.world, ground.tiles, flight.eye.metres())) {
        return;
    }
    take_part(controls, player, flight, camera, ground.world);
}

// Saves a report of what's shown into a fresh directory under var/reports, the picture left
// to the frame drawn next; where it went, or none where it couldn't be saved.
std::optional<std::filesystem::path>
save_report(Connection& connection, const StreamedGround& ground, const render::Camera& camera,
            const std::optional<client::Player>& player, const client::Flight& flight,
            double radius, const client::FrameTimes& times, const client::PlanetClock& clock,
            const client::RecentLog& recent) {
    // As far as the pieces a block could read reach round the eye, and the coarse ones over it.
    constexpr double reach = 2'000.0;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::filesystem::path directory =
        std::filesystem::path{"var/reports"} / std::format("{:%Y%m%d-%H%M%S}", now);
    client::ReportAbout about{.eye = camera.eye.metres(),
                              .forward = camera.forward,
                              .up = camera.up,
                              .feet =
                                  player ? std::optional{player->feet().metres()} : std::nullopt,
                              .lines = {}};
    about.lines.emplace_back("at",
                             client::at_argument(seen_from(flight, camera.eye.metres(), radius)));
    about.lines.emplace_back("metres over the radius",
                             std::format("{:.3f}", glm::length(camera.eye.metres()) - radius));
    about.lines.emplace_back("seed", std::to_string(ground.planet.body.seed));
    about.lines.emplace_back("radius", std::format("{:.17g}", radius));
    about.lines.emplace_back("game time", std::to_string(clock.time().time_since_epoch().count()));
    about.lines.emplace_back("pieces held", std::to_string(ground.pieces.pieces()));
    about.lines.emplace_back("piece bytes", std::to_string(ground.pieces.bytes()));
    about.lines.emplace_back("blocks meshing", std::to_string(ground.block_meshes.in_flight()));
    about.lines.emplace_back("blocks settled", ground.block_meshes.settled() ? "yes" : "no");
    const client::Timing cpu = times.cpu();
    about.lines.emplace_back("frame ms",
                             std::format("mean {:.2f} worst {:.2f}", cpu.mean, cpu.worst));
    for (const client::Timing& pass : times.gpu()) {
        about.lines.emplace_back(std::format("gpu {} ms", pass.name),
                                 std::format("mean {:.2f} worst {:.2f}", pass.mean, pass.worst));
    }
    const std::vector<client::PieceWish> wanted = client::pieces_to_ask(
        ground.pieces, camera.eye.metres(), camera.forward, grid::View{}, protocol::most_asked);
    const std::vector<client::DrawnBlock> drawn = ground.block_meshes.drawn();
    const std::vector<client::PieceWish> asked = ground.requests.open();
    const std::vector<std::string> lines = recent.lines();
    const client::Report report{.about = about,
                                .held = &ground.pieces,
                                .reach = reach,
                                .asked = asked,
                                .wanted = wanted,
                                .drawn = drawn,
                                .log = lines};
    if (const Result<void> written = client::write_report(directory, report); !written) {
        log::warn(connection_log, "can't save a report: {}", written.error());
        return std::nullopt;
    }
    log::info(connection_log, "report saved to {}", directory.string());
    connection.report(static_cast<std::uint64_t>(now.time_since_epoch().count()));
    return directory;
}

// Flies or walks over the ground the server streams until the window closes or it's time to quit,
// drawing it from the tiles held.
Result<void> fly_streamed(client::Scene& scene, client::PlanetClock& clock, StreamedGround& ground,
                          client::Flight flight, Connection& connection,
                          const client::Options& options, const render::Device& device,
                          render::Presenter& presenter, SDL_Window* window,
                          const client::RecentLog& recent) {
    const globe::Planet& planet = ground.planet;
    const double radius = planet.body.radius;
    Controls controls;
    controls.timings = options.timings;
    controls.through = options.through;
    TitleCounter title;
    client::FrameTimes times;
    const MonotonicClock::time_point started = MonotonicClock::now();
    MonotonicClock::time_point last = started;
    ShaderReloader shaders{options.reload_shaders};
    std::optional<client::Player> player;
    client::Forecast forecast{radius};
    ChatBox chat;
    bool routed_to_chat = false;
    render::Camera camera = client::flight_camera(flight, client::vertical_fov);
    while (!controls.quit) {
        take_events(controls, connection, window);
        const bool chat_opened = route_keys(window, controls.chatting, routed_to_chat);
        change_clouds(controls, scene, device);
        const MonotonicClock::time_point now = MonotonicClock::now();
        const double passed = std::chrono::duration<double>(now - last).count();
        const double seconds = std::min(passed, longest_frame);
        last = now;
        times.add(passed, scene.timings());
        shaders.look(device, now);
        take_part_streamed(controls, player, flight, camera, ground);
        report(connection, player, flight, forecast, chat, options.chat_filter, now);
        // Ground is asked for and meshed about the player, wherever the camera has gone.
        const DVec3 focus = player ? player->feet().metres() : flight.eye.metres();
        take_ground(connection, ground, scene, player, focus, camera.forward);
        // Where the server remembers the player, unless they said where to start.
        if (const auto placed = connection.take_placed(); placed && !options.start) {
            const WorldPosition at = protocol::to_world(placed->at);
            flight = client::start_flight(glm::normalize(at.metres()), radius,
                                          glm::length(at.metres()) - radius, planet::north_pole);
        }
        const client::Session* session = connection.session();
        camera = move(controls, player, flight, ground.world, seconds);
        const double ground_height = drawn_ground(ground.world, camera.eye);
        if (std::exchange(controls.dig, false)) {
            connection.dig(WorldPosition{camera.eye.metres() + (camera.forward * edit_ahead)},
                           edit_radius);
        }
        if (std::exchange(controls.build, false)) {
            connection.build(WorldPosition{camera.eye.metres() + (camera.forward * edit_ahead)},
                             edit_radius, edit_material);
        }
        change_pace(controls, clock);
        hurry_world(controls, connection);
        clock.advance(seconds);
        title.count(
            window, glm::length(camera.eye.metres()) - radius - ground_height,
            scene.calendar().local_time_of_day(clock.time(), glm::normalize(flight.eye.metres())),
            clock.rate(), world_rate(connection),
            client::at_argument(seen_from(flight, camera.eye.metres(), radius)));
        if (time_up(options, started, now)) {
            controls.quit = true;
        }
        const terrain::Bend bend =
            terrain::bend_from(camera.eye, radius, ground_height, bend_settings(options));
        const std::vector<replication::Shown> others = others_about(session, now);
        std::vector<render::Capsule> avatars =
            capsules_shown(session, others, player, now, planet.voxel, planet.body.gravity);
        const client::Shot shot{.camera = camera,
                                .bend = bend,
                                .time = clock.time(),
                                .seconds = seconds,
                                .cutaway = 0.0,
                                .companion = std::nullopt,
                                .others = std::move(avatars),
                                .forecast = &forecast,
                                .now = now,
                                .still_water = still_at_eye(ground, camera.eye)};
        const Overlay overlay{.times = &times,
                              .timings = controls.timings,
                              .clouds = scene.cloud_quality(),
                              .session = session,
                              .others = others,
                              .eye = camera.eye,
                              .pockets = {},
                              .now = now,
                              .weather = weather_words(controls.timings, forecast, shot),
                              .chat = &chat,
                              .chatting = &controls.chatting,
                              .chat_opened = chat_opened};
        std::optional<std::filesystem::path> capture =
            controls.quit ? options.capture : std::nullopt;
        if (std::exchange(controls.report, false)) {
            if (const auto saved = save_report(connection, ground, camera, player, flight, radius,
                                               times, clock, recent);
                saved) {
                capture = *saved / client::report_picture;
            }
        }
        if (auto shown = show_frame(scene, shot, device, presenter, window, overlay, capture, chat,
                                    connection);
            !shown) {
            return shown;
        }
        HOG_FRAME();
    }
    return {};
}

// Says what startup is at, and how many seconds in it is, as it starts each long stage.
void startup(std::string_view stage) {
    static const MonotonicClock::time_point started = MonotonicClock::now();
    const std::chrono::duration<double> since = MonotonicClock::now() - started;
    std::cout << std::format("{}: {:7.1f} s  {}\n", program, since.count(), stage) << std::flush;
}

// Connects and waits to be welcomed, which brings the planet the server runs; the window keeps
// answering meanwhile.
Result<globe::Planet> welcomed_to(Connection& connection) {
    constexpr std::chrono::seconds patience{60};
    const MonotonicClock::time_point began = MonotonicClock::now();
    while (MonotonicClock::now() - began < patience) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                return make_error(ErrorCode::Unsupported, "closed before the server welcomed");
            }
        }
        connection.pump(MonotonicClock::now());
        if (const client::Session* session = connection.session()) {
            if (session->state() == client::Session::State::Refused) {
                return make_error(ErrorCode::Unsupported, session->reason());
            }
            if (session->state() == client::Session::State::Welcomed) {
                return globe::decode_planet(session->planet());
            }
        }
        SDL_Delay(5);
    }
    return make_error(ErrorCode::Io, "the server didn't welcome the client in time");
}

// The client: connects, is told the planet, and draws the surfaces the server streams, with none
// of its own and nothing made of the planet here.
int show_streamed_world(const client::Options& options, render::Device& device,
                        render::Presenter& presenter, SDL_Window* window,
                        const client::RecentLog& recent) {
    startup("starting");
    auto connection = Connection::open(options, std::nullopt);
    if (!connection) {
        std::cerr << std::format("{}: {}\n", program, connection.error());
        return EXIT_FAILURE;
    }
    startup("connecting");
    const auto planet = welcomed_to(*connection);
    if (!planet) {
        std::cerr << std::format("{}: {}\n", program, planet.error());
        return EXIT_FAILURE;
    }
    startup(std::format("welcomed to planet {:016x}", globe::world_id(*planet)));
    const sim::Calendar calendar{globe::day_length(*planet), planet->body.tilt};
    constexpr double start_height = 2'000.0;
    client::Flight flight =
        options.start
            ? client::start_flight(
                  planet::direction_at(options.start->latitude * (std::numbers::pi / 180.0),
                                       options.start->longitude * (std::numbers::pi / 180.0)),
                  planet->body.radius, options.start->altitude, planet::north_pole)
            : client::start_flight(planet::start_direction, planet->body.radius, start_height,
                                   planet::north_pole);
    flight.pitch = start_pitch;
    if (options.start && options.start->heading && options.start->pitch) {
        constexpr double degree = std::numbers::pi / 180.0;
        flight = client::turned_to(flight, *options.start->heading * degree,
                                   *options.start->pitch * degree);
    }
    client::PlanetClock clock{client::start_time(calendar, to_game_time(WallClock::now()),
                                                 glm::normalize(flight.eye.metres()),
                                                 options.time_of_day)};
    const window::Ui ui{window, device, presenter.swapchain().format()};
    if (!ui.started()) {
        std::cerr << program << ": can't draw the timings over the picture\n";
        return EXIT_FAILURE;
    }
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange |
                                  ImGuiConfigFlags_NoKeyboard;
    startup("starting the renderer");
    jobs::JobSystem jobs{options.threads};
    auto scene =
        client::Scene::create(device, *planet, jobs, calendar, presenter.swapchain().format(),
                              render::frames_in_flight, std::nullopt);
    if (!scene) {
        std::cerr << std::format("{}: {}\n", program, scene.error());
        return EXIT_FAILURE;
    }
    if (auto set = scene->set_cloud_quality(options.clouds); !set) {
        std::cerr << std::format("{}: {}\n", program, set.error());
        return EXIT_FAILURE;
    }
    scene->set_system(planet->system);
    // The planet only as it's laid out in voxel layers; its ground is the server's to send.
    const client::Session* session = connection->session();
    const auto origin = session != nullptr ? globe::origin_from(session->ground_origin())
                                           : Result<globe::VolumeOrigin>{make_error(
                                                 ErrorCode::Io, "no session to be welcomed by")};
    if (!origin) {
        std::cerr << std::format("{}: the server's voxel layers: {}\n", program, origin.error());
        return EXIT_FAILURE;
    }
    StreamedGround ground{*planet, origin->layers, jobs};
    // One cache a world, so another world's chunks are never taken for this one's.
    const std::filesystem::path cache_file =
        std::filesystem::path{"var/cache"} / std::format("{:016x}.mdb", session->world());
    if (auto cache = client::ChunkCache::open(cache_file, cache_budget); !cache) {
        log::warn(connection_log, "can't keep chunks between runs: {}", cache.error());
    } else {
        log::info(connection_log, "{} chunks kept from before, {} MB", cache->chunks(),
                  cache->bytes() >> 20U);
        ground.cache = std::move(*cache);
    }
    startup("ready");
    const Result<void> flown = fly_streamed(*scene, clock, ground, flight, *connection, options,
                                            device, presenter, window, recent);
    vkDeviceWaitIdle(device.handle());
    if (!flown) {
        std::cerr << std::format("{}: {}\n", program, flown.error());
        return EXIT_FAILURE;
    }
    if (options.validate && device.validation_errors() != 0) {
        std::cerr << std::format("{}: {} Vulkan validation errors\n", program,
                                 device.validation_errors());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

// Logs to the console, to a file a run under var/logs, and keeps the lines lately logged for
// reports; undone as it goes.
struct ClientLogging {
    explicit ClientLogging(const std::string& name) {
        sinks.push_back(
            log::add_sink(std::make_unique<log::LineSink>(std::clog, log::LineFormat::Text)));
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        if (auto file = log::LineSink::open(std::filesystem::path{"var/logs"} /
                                                std::format("{}-{:%Y%m%d-%H%M%S}.log", name, now),
                                            log::LineFormat::Text);
            file) {
            sinks.push_back(log::add_sink(std::move(*file)));
        }
        auto kept = std::make_unique<client::RecentLog>();
        recent = kept.get();
        sinks.push_back(log::add_sink(std::move(kept)));
    }
    ClientLogging(const ClientLogging&) = delete;
    ClientLogging& operator=(const ClientLogging&) = delete;
    ClientLogging(ClientLogging&&) = delete;
    ClientLogging& operator=(ClientLogging&&) = delete;
    ~ClientLogging() {
        for (const log::SinkHandle sink : sinks) {
            log::remove_sink(sink);
        }
    }

    std::vector<log::SinkHandle> sinks;
    const client::RecentLog* recent = nullptr; // owned by the log while it's added
};

int run(std::span<const std::string_view> arguments) {
    auto options = client::parse_options(arguments);
    if (!options) {
        std::cerr << std::format("{}: {}\n\n{}", program, options.error().message, client::usage());
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::cout << client::usage();
        return EXIT_SUCCESS;
    }
    if (!options->connect) {
        std::cerr << "hog-client: --connect ADDRESS is required\n";
        return EXIT_FAILURE;
    }
    const ClientLogging logging{options->name.value_or("client")};
    // Declared in the order they're made, so each goes before what it was made with.
    const window::Sdl sdl;
    if (!sdl.started()) {
        std::cerr << std::format("{}: can't start SDL: {}\n", program, SDL_GetError());
        return EXIT_FAILURE;
    }
    if (!sdl.vulkan()) {
        std::cerr << std::format("{}: can't load Vulkan: {}\n", program, SDL_GetError());
        return window::no_vulkan;
    }
    const window::Window window{
        SDL_CreateWindow("HOG", 1'600, 900,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)};
    if (window == nullptr) {
        std::cerr << std::format("{}: can't open a window: {}\n", program, SDL_GetError());
        return EXIT_FAILURE;
    }
    const auto wanted = window::device_options(
        window.get(),
        {.app_name = "HOG", .validation = options->validate, .prefer_software = options->software});
    if (!wanted) {
        std::cerr << std::format("{}: {}\n", program, wanted.error());
        return window::no_vulkan;
    }
    const auto device = render::Device::create(*wanted);
    if (!device) {
        std::cerr << std::format("{}: can't draw: {}\n", program, device.error());
        return device.error().code == ErrorCode::Unsupported ? window::no_vulkan : EXIT_FAILURE;
    }
    const auto presenter = render::Presenter::create(**device, window::drawable(window.get()));
    if (!presenter) {
        std::cerr << std::format("{}: can't show frames: {}\n", program, presenter.error());
        return EXIT_FAILURE;
    }
    return show_streamed_world(*options, **device, **presenter, window.get(), *logging.recent);
}

} // namespace

namespace hog::client {

int play(std::span<const std::string_view> arguments) {
    try {
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << program << ": " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

} // namespace hog::client

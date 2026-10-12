#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/probe/messages.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/viewer/clock_text.hpp>
#include <hog/viewer/frame.hpp>
#include <hog/viewer/link.hpp>
#include <hog/viewer/map_view.hpp>
#include <hog/viewer/options.hpp>
#include <hog/viewer/overlay.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hog;

constexpr float sidebar_width = 280.0F;
constexpr double zoom_step = 1.2;

struct WindowDeleter {
    void operator()(SDL_Window* window) const noexcept { SDL_DestroyWindow(window); }
};
struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const noexcept { SDL_DestroyRenderer(renderer); }
};
struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const noexcept { SDL_DestroySurface(surface); }
};
using Surface = std::unique_ptr<SDL_Surface, SurfaceDeleter>;
struct TextureDeleter {
    void operator()(SDL_Texture* texture) const noexcept { SDL_DestroyTexture(texture); }
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;

// SDL from starting to quitting.
class Sdl {
public:
    Sdl() : started_(SDL_Init(SDL_INIT_VIDEO)) {}
    Sdl(const Sdl&) = delete;
    Sdl(Sdl&&) = delete;
    Sdl& operator=(const Sdl&) = delete;
    Sdl& operator=(Sdl&&) = delete;
    ~Sdl() {
        if (started_) {
            SDL_Quit();
        }
    }

    [[nodiscard]] bool started() const noexcept { return started_; }

private:
    bool started_;
};

// ImGui, drawing through an SDL renderer.
class Ui {
public:
    Ui(SDL_Window* window, SDL_Renderer* renderer) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
    }
    Ui(const Ui&) = delete;
    Ui(Ui&&) = delete;
    Ui& operator=(const Ui&) = delete;
    Ui& operator=(Ui&&) = delete;
    ~Ui() {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
};

ImTextureRef texture_ref(const SDL_Texture* texture) {
    // The SDL renderer backend takes its textures as ImGui's opaque ids.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture))};
}

std::string format_time(GameTime time) {
    return std::format("{:%F %R} UTC",
                       std::chrono::sys_seconds{
                           std::chrono::floor<std::chrono::seconds>(time.time_since_epoch())});
}

// Saves what's been drawn so far this frame.
Result<void> capture(SDL_Renderer* renderer, const std::filesystem::path& path) {
    const Surface read{SDL_RenderReadPixels(renderer, nullptr)};
    const Surface rgb{read != nullptr ? SDL_ConvertSurface(read.get(), SDL_PIXELFORMAT_RGB24)
                                      : nullptr};
    if (rgb == nullptr) {
        return make_error(ErrorCode::Io, std::format("can't read the window: {}", SDL_GetError()));
    }
    const std::size_t row = 3 * static_cast<std::size_t>(rgb->w);
    const auto rows = static_cast<std::size_t>(rgb->h);
    const auto pitch = static_cast<std::size_t>(rgb->pitch);
    const std::span<const std::uint8_t> pixels{static_cast<const std::uint8_t*>(rgb->pixels),
                                               pitch * rows};
    mapgen::Image image{.width = rgb->w, .height = rgb->h, .pixels = {}};
    image.pixels.reserve(row * rows);
    for (std::size_t y = 0; y < rows; ++y) {
        const auto line = pixels.subspan(y * pitch, row);
        image.pixels.insert(image.pixels.end(), line.begin(), line.end());
    }
    return mapgen::write_png(path, image);
}

// What the person at the viewer has chosen, and what's drawn from it.
struct View {
    viewer::Overlay overlay = viewer::Overlay::Terrain;
    std::string species;
    viewer::MapView map;
    Texture texture;
    std::optional<sim::CellGrid> grid;
    std::uint64_t drawn = 0; // the link's frame count when last drawn
    bool stale = true;       // the choices changed since
};

// Draws the newest frame into the map texture, when there's something new to draw.
void redraw(View& view, const viewer::Link& link, SDL_Renderer* renderer, int width,
            jobs::JobSystem& jobs) {
    const std::shared_ptr<const viewer::Frame> frame = link.frame();
    if (frame == nullptr || (!view.stale && link.frames() == view.drawn)) {
        return;
    }
    const int level = frame->welcome().level;
    if (!view.grid || view.grid->level() != level) {
        view.grid.emplace(level);
    }
    const mapgen::Image image =
        viewer::render_overlay(*view.grid, *frame, view.overlay, view.species, width, jobs);
    if (view.texture == nullptr) {
        view.texture.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                             SDL_TEXTUREACCESS_STATIC, image.width, image.height));
    }
    if (view.texture != nullptr) {
        SDL_UpdateTexture(view.texture.get(), nullptr, image.pixels.data(), 3 * image.width);
    }
    view.drawn = link.frames();
    view.stale = false;
}

void choose(View& view, viewer::Link& link, viewer::Overlay overlay, std::string species) {
    if (overlay == view.overlay && species == view.species) {
        return;
    }
    view.overlay = overlay;
    view.species = std::move(species);
    view.stale = true;
    link.want(viewer::layers_for(view.overlay, view.species));
}

// Where the world's clock is against real time, and, if the server allows it, ways to change it.
void clock_controls(viewer::Link& link, const viewer::Frame& frame) {
    ImGui::TextUnformatted(
        viewer::drift_text(frame.time(), to_game_time(WallClock::now())).c_str());
    if (!frame.welcome().controllable) {
        return;
    }
    const probe::Clock& clock = frame.clock();
    const auto send = [&link](probe::Action action, std::uint32_t amount) {
        link.control(probe::Control{.action = action, .amount = amount});
    };
    if (ImGui::Button(clock.paused ? "resume" : "pause")) {
        send(clock.paused ? probe::Action::Resume : probe::Action::Pause, 0);
    }
    ImGui::BeginDisabled(!clock.paused);
    for (const std::uint32_t ticks : {1U, 10U, 100U}) {
        ImGui::SameLine();
        if (ImGui::Button(std::format("+{}", ticks).c_str())) {
            send(probe::Action::Step, ticks);
        }
    }
    ImGui::EndDisabled();
    for (const viewer::Speed& speed : viewer::speeds) {
        if (ImGui::RadioButton(std::string{speed.label}.c_str(), clock.scale == speed.scale)) {
            send(probe::Action::Speed, speed.scale);
        }
    }
    if (ImGui::Button("rejoin real time")) {
        send(probe::Action::RealTime, 0);
    }
    if (const std::string error = link.control_error(); !error.empty()) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0F, 0.6F, 0.4F, 1.0F});
        ImGui::TextUnformatted(error.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
}

void sidebar(View& view, viewer::Link& link) {
    const viewer::Link::State state = link.state();
    const std::shared_ptr<const viewer::Frame> frame = link.frame();
    ImGui::TextUnformatted(std::format("server: {}", viewer::to_string(state.status)).c_str());
    if (!state.message.empty()) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(state.message.c_str());
        ImGui::PopTextWrapPos();
    }
    if (frame != nullptr) {
        ImGui::TextUnformatted(std::format("world {:016x}", frame->welcome().world_id).c_str());
        ImGui::TextUnformatted(format_time(frame->time()).c_str());
        clock_controls(link, *frame);
    }
    ImGui::Separator();

    viewer::Overlay overlay = view.overlay;
    for (const viewer::Overlay each : viewer::overlays) {
        if (ImGui::RadioButton(std::string{viewer::to_string(each)}.c_str(), overlay == each)) {
            overlay = each;
        }
    }
    std::string species = view.species;
    if (overlay == viewer::Overlay::Wildlife && frame != nullptr) {
        const std::vector<std::string> known = frame->species();
        if (species.empty() && !known.empty()) {
            species = known.front();
        }
        if (ImGui::BeginCombo("species", species.c_str())) {
            for (const std::string& name : known) {
                if (ImGui::Selectable(name.c_str(), name == species)) {
                    species = name;
                }
            }
            ImGui::EndCombo();
        }
    }
    choose(view, link, overlay, std::move(species));

    ImGui::Separator();
    if (frame != nullptr) {
        if (const auto legend = viewer::legend_for(view.overlay, *frame, view.species)) {
            ImGui::TextUnformatted(legend->layer.c_str());
            ImGui::TextUnformatted(
                std::format("{:.4g} to {:.4g} {}", legend->low, legend->high, legend->unit)
                    .c_str());
        }
    }
    ImGui::TextUnformatted(std::format("zoom {:.1f}x", view.map.zoom()).c_str());
    if (ImGui::Button("whole planet")) {
        view.map.reset();
    }
}

// The map, as large as fits at twice as wide as tall; wheel zooms, dragging pans.
void map(View& view) {
    const ImVec2 space = ImGui::GetContentRegionAvail();
    const float width = std::max(std::min(space.x, space.y * 2.0F), 1.0F);
    const ImVec2 size{width, width / 2.0F};
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("map", size);
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered()) {
        const DVec2 at{(io.MousePos.x - corner.x) / size.x, (io.MousePos.y - corner.y) / size.y};
        if (io.MouseWheel != 0.0F) {
            view.map.zoom_by(std::pow(zoom_step, static_cast<double>(io.MouseWheel)), at);
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            view.map.reset();
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        view.map.pan(DVec2{io.MouseDelta.x / size.x, io.MouseDelta.y / size.y});
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 far{corner.x + size.x, corner.y + size.y};
    if (view.texture == nullptr) {
        draw->AddRectFilled(corner, far, ImGui::GetColorU32(ImGuiCol_FrameBg));
        return;
    }
    const viewer::MapRect shown = view.map.shown();
    draw->AddImage(texture_ref(view.texture.get()), corner, far,
                   ImVec2{static_cast<float>(shown.low.x), static_cast<float>(shown.low.y)},
                   ImVec2{static_cast<float>(shown.high.x), static_cast<float>(shown.high.y)});
}

int run(std::span<const std::string_view> arguments) {
    const auto options = viewer::parse_options(arguments);
    if (!options) {
        std::cerr << std::format("hog-viewer: {}\n\n{}", options.error().message, viewer::usage());
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::cout << viewer::usage();
        return EXIT_SUCCESS;
    }
    // Declared in the order they're made, so each goes before what it was made with.
    const Sdl sdl;
    if (!sdl.started()) {
        std::cerr << std::format("hog-viewer: can't start SDL: {}\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    const std::unique_ptr<SDL_Window, WindowDeleter> window{SDL_CreateWindow(
        "HOG viewer", 1'400, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)};
    if (window == nullptr) {
        std::cerr << std::format("hog-viewer: can't open a window: {}\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    const std::unique_ptr<SDL_Renderer, RendererDeleter> renderer{
        SDL_CreateRenderer(window.get(), nullptr)};
    if (renderer == nullptr) {
        std::cerr << std::format("hog-viewer: can't draw: {}\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    SDL_SetRenderVSync(renderer.get(), 1);
    const Ui ui{window.get(), renderer.get()};

    jobs::JobSystem jobs;
    viewer::Link link{options->host, options->port, viewer::Link::Timing{}};
    View view;
    view.overlay = options->overlay;
    view.species = options->species;
    link.want(viewer::layers_for(view.overlay, view.species));

    const MonotonicClock::time_point started = MonotonicClock::now();
    for (bool running = true; running;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
        redraw(view, link, renderer.get(), options->width, jobs);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("viewer", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::BeginChild("sidebar", ImVec2{sidebar_width, 0.0F});
        sidebar(view, link);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("map");
        map(view);
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();

        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(renderer.get(), io.DisplayFramebufferScale.x,
                           io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer.get(), 20, 20, 24, 255);
        SDL_RenderClear(renderer.get());
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer.get());
        if (options->quit_after && MonotonicClock::now() - started >= *options->quit_after) {
            running = false;
        }
        if (!running && options->capture) {
            if (auto saved = capture(renderer.get(), *options->capture); !saved) {
                std::cerr << std::format("hog-viewer: {}\n", saved.error());
            }
        }
        SDL_RenderPresent(renderer.get());
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span<char*> given{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(given.begin() + 1, given.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-viewer: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

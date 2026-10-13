#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/geology.hpp>
#include <hog/probe/messages.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/viewer/clock_text.hpp>
#include <hog/viewer/daylight.hpp>
#include <hog/viewer/frame.hpp>
#include <hog/viewer/ground.hpp>
#include <hog/viewer/inspector.hpp>
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
#include <utility>
#include <vector>

namespace {

using namespace hog;

constexpr float sidebar_width = 280.0F;
constexpr double zoom_step = 1.2;
constexpr float sun_radius = 6.0F;
constexpr float marker_reach = 9.0F; // pixels from the inspected point to a crosshair arm's end
// How long the view must hold still before the ground under it is surveyed.
constexpr std::chrono::milliseconds survey_settle{150};
constexpr float deepest = 100'000.0F; // metres, for the underground overlays

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
    bool daylight = true;
    bool natural = true; // edges of the simulation's cells
    float depth = 10.0F; // metres below the surface the underground overlays show
    viewer::SurveyPacer pacer{survey_settle};
    std::uint64_t grounds = 0; // the link's ground count when last looked at
    Texture texture;
    std::optional<sim::CellGrid> grid;
    std::optional<DVec2> sun;       // where it's overhead on the map, when daylight is shown
    std::uint64_t drawn = 0;        // the link's frame count when last drawn
    std::uint64_t drawn_ground = 0; // and its ground count
    viewer::MapRect drawn_shown;    // the part of the map the texture holds
    bool stale = true;              // the choices changed since
    std::optional<probe::Inspect> inspected;
};

void inspect(View& view, viewer::Link& link, std::optional<probe::Inspect> point) {
    view.inspected = point;
    link.inspect(point);
}

// Draws the newest frame into the map texture, when there's something new to draw.
void redraw(View& view, const viewer::Link& link, SDL_Renderer* renderer, int width,
            jobs::JobSystem& jobs) {
    const std::shared_ptr<const viewer::Frame> frame = link.frame();
    const viewer::MapRect shown = view.map.shown();
    if (frame == nullptr || (!view.stale && link.frames() == view.drawn &&
                             link.grounds() == view.drawn_ground && shown == view.drawn_shown)) {
        return;
    }
    const std::uint64_t grounds = link.grounds();
    const std::shared_ptr<const probe::Ground> ground = link.ground();
    const int level = frame->welcome().level;
    if (!view.grid || view.grid->level() != level) {
        view.grid.emplace(level);
    }
    mapgen::Image image = viewer::render_overlay(*view.grid, *frame, ground.get(), view.overlay,
                                                 view.species, shown, width, view.natural, jobs);
    view.sun.reset();
    if (const auto calendar = viewer::calendar_of(frame->welcome()); calendar && view.daylight) {
        viewer::shade_night(image, shown, *calendar, frame->time(), jobs);
        view.sun = viewer::subsolar_point(*calendar, frame->time());
    }
    if (view.texture == nullptr) {
        view.texture.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                             SDL_TEXTUREACCESS_STATIC, image.width, image.height));
    }
    if (view.texture != nullptr) {
        SDL_UpdateTexture(view.texture.get(), nullptr, image.pixels.data(), 3 * image.width);
    }
    view.drawn = link.frames();
    view.drawn_ground = grounds;
    view.drawn_shown = shown;
    view.stale = false;
}

// Asks for the ground under the view once it holds still, at about the map's own resolution.
void survey(View& view, viewer::Link& link, int width) {
    // A link that connects again starts without ground.
    if (const std::uint64_t grounds = link.grounds(); grounds != view.grounds) {
        view.grounds = grounds;
        if (link.ground() == nullptr) {
            view.pacer.forget();
        }
    }
    const int across = std::min(width, static_cast<int>(probe::most_survey_side));
    const probe::Survey wanted = viewer::survey_of(view.map.shown(), across, across / 2,
                                                   viewer::underground(view.overlay), view.depth);
    if (const auto ready = view.pacer.poll(wanted, MonotonicClock::now())) {
        link.survey(*ready);
    }
}

// What an overlay's colour stands for.
void swatch(mapgen::Rgb colour, std::string_view name) {
    const auto channel = [&colour](std::size_t index) {
        return static_cast<float>(colour.at(index)) / 255.0F;
    };
    const ImVec4 shown{channel(0), channel(1), channel(2), 1.0F};
    ImGui::ColorButton(std::string{name}.c_str(), shown, ImGuiColorEditFlags_NoTooltip,
                       ImVec2{12.0F, 12.0F});
    ImGui::SameLine();
    ImGui::TextUnformatted(std::string{name}.c_str());
}

void biomes_key() {
    for (int biome = 0; biome < planet::biome_count; ++biome) {
        const auto each = static_cast<planet::Biome>(biome);
        swatch(mapgen::biome_colour(each), planet::to_string(each));
    }
    swatch(mapgen::river_colour(0.0), "river");
}

void minerals_key() {
    for (int rock = 0; rock < planet::rock_count; ++rock) {
        const auto each = static_cast<planet::Rock>(rock);
        swatch(mapgen::rock_colour(each), planet::to_string(each));
    }
    for (const planet::Resource resource : planet::rarest_first) {
        swatch(mapgen::resource_colour(resource), planet::to_string(resource));
    }
    for (int cave = 0; cave < planet::cave_count; ++cave) {
        const auto each = static_cast<planet::Cave>(cave);
        swatch(mapgen::cave_colour(each), planet::to_string(each));
    }
    ImGui::TextUnformatted("paler caves: too tight to walk");
}

void heat_key() {
    for (const int celsius : {0, 100, 300, 700, 1'100}) {
        swatch(mapgen::heat_colour(celsius * planet::fixed_one), std::format("{} °C", celsius));
    }
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

// The overlays to choose from, and the species when it's wildlife.
void overlay_picker(View& view, viewer::Link& link, const viewer::Frame* frame) {
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
}

// What the overlay's colours mean.
void legend(const View& view, const viewer::Link& link, const viewer::Frame* frame) {
    if (viewer::underground(view.overlay)) {
        const auto ground = link.ground();
        ImGui::TextUnformatted(ground != nullptr && !ground->rock.empty()
                                   ? std::format("{:.0f} m down", ground->survey.depth).c_str()
                                   : "surveying");
        if (view.overlay == viewer::Overlay::Minerals) {
            minerals_key();
        } else {
            heat_key();
        }
    }
    if (view.overlay == viewer::Overlay::Biome) {
        biomes_key();
    }
    if (frame == nullptr) {
        return;
    }
    if (const auto range = viewer::legend_for(view.overlay, *frame, view.species)) {
        ImGui::TextUnformatted(range->layer.c_str());
        ImGui::TextUnformatted(
            std::format("{:.4g} to {:.4g} {}", range->low, range->high, range->unit).c_str());
    }
}

void readings_table(const char* id, const std::vector<viewer::Reading>& readings) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    for (const viewer::Reading& reading : readings) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(reading.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(reading.value.c_str());
    }
    ImGui::EndTable();
}

void faint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// Everything known about the point picked on the map.
void inspector(View& view, viewer::Link& link) {
    if (!view.inspected) {
        faint("click the map to inspect a point");
        return;
    }
    ImGui::TextUnformatted(
        std::format("{:.4f}°, {:.4f}°", view.inspected->latitude, view.inspected->longitude)
            .c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) {
        inspect(view, link, std::nullopt);
        return;
    }
    const std::shared_ptr<const probe::Inspection> inspection = link.inspection();
    const std::shared_ptr<const viewer::Frame> frame = link.frame();
    if (inspection == nullptr || frame == nullptr) {
        faint("inspecting...");
        return;
    }
    if (view.grid && inspection->cell < view.grid->size()) {
        ImGui::TextUnformatted(
            std::format("cell {} ({})", inspection->cell, view.grid->cell(inspection->cell))
                .c_str());
    }
    for (const viewer::ReadingGroup& group : viewer::readings(frame->welcome(), *inspection)) {
        if (ImGui::CollapsingHeader(group.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            readings_table(group.name.c_str(), group.readings);
        }
    }
    if (ImGui::CollapsingHeader("ground", ImGuiTreeNodeFlags_DefaultOpen)) {
        readings_table("ground", viewer::column_readings(inspection->column));
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

    overlay_picker(view, link, frame.get());
    if (viewer::underground(view.overlay)) {
        ImGui::SliderFloat("depth", &view.depth, 0.0F, deepest, "%.0f m",
                           ImGuiSliderFlags_Logarithmic);
    }
    if (ImGui::Checkbox("daylight", &view.daylight)) {
        view.stale = true;
    }
    if (ImGui::Checkbox("natural edges", &view.natural)) {
        view.stale = true;
    }

    ImGui::Separator();
    legend(view, link, frame.get());
    ImGui::TextUnformatted(std::format("zoom {:.1f}x", view.map.zoom()).c_str());
    if (ImGui::Button("whole planet")) {
        view.map.reset();
    }

    ImGui::Separator();
    inspector(view, link);
}

// A crosshair over the inspected point, if it's on show.
void mark_inspected(const View& view, ImVec2 corner, ImVec2 size) {
    if (!view.inspected) {
        return;
    }
    const DVec2 at =
        viewer::shown_point(view.map.shown(), viewer::inspected_point(*view.inspected));
    const ImVec2 centre{corner.x + (static_cast<float>(at.x) * size.x),
                        corner.y + (static_cast<float>(at.y) * size.y)};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(corner, ImVec2{corner.x + size.x, corner.y + size.y}, true);
    // A dark crosshair under a light one, so it shows on any overlay.
    for (const auto& [colour, thickness] :
         {std::pair{IM_COL32(0, 0, 0, 200), 3.5F}, std::pair{IM_COL32(255, 255, 255, 255), 1.5F}}) {
        draw->AddLine(ImVec2{centre.x - marker_reach, centre.y},
                      ImVec2{centre.x + marker_reach, centre.y}, colour, thickness);
        draw->AddLine(ImVec2{centre.x, centre.y - marker_reach},
                      ImVec2{centre.x, centre.y + marker_reach}, colour, thickness);
    }
    draw->PopClipRect();
}

// The map, as large as fits at twice as wide as tall; wheel zooms, dragging pans, clicking
// inspects and right-clicking or Escape stops.
void map(View& view, viewer::Link& link) {
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
        // Only a press on the map that didn't move far enough to pan.
        const float threshold = io.MouseDragThreshold;
        if (ImGui::IsItemDeactivated() &&
            io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < threshold * threshold) {
            inspect(view, link, viewer::inspect_at(viewer::map_point(view.map.shown(), at)));
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            inspect(view, link, std::nullopt);
        }
    }
    if (view.inspected && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        inspect(view, link, std::nullopt);
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
    // The texture can be a frame behind the view; until it's redrawn, show its part of what's held.
    const viewer::MapRect shown = view.map.shown();
    const DVec2 low = viewer::shown_point(view.drawn_shown, shown.low);
    const DVec2 high = viewer::shown_point(view.drawn_shown, shown.high);
    draw->AddImage(texture_ref(view.texture.get()), corner, far,
                   ImVec2{static_cast<float>(low.x), static_cast<float>(low.y)},
                   ImVec2{static_cast<float>(high.x), static_cast<float>(high.y)});
    if (view.sun) {
        const DVec2 at = viewer::shown_point(shown, *view.sun);
        const ImVec2 centre{corner.x + (static_cast<float>(at.x) * size.x),
                            corner.y + (static_cast<float>(at.y) * size.y)};
        draw->PushClipRect(corner, far, true);
        draw->AddCircleFilled(centre, sun_radius, IM_COL32(255, 210, 60, 255));
        draw->AddCircle(centre, sun_radius, IM_COL32(60, 40, 0, 255), 0, 1.5F);
        draw->PopClipRect();
    }
    mark_inspected(view, corner, size);
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
        survey(view, link, options->width);
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
        map(view, link);
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

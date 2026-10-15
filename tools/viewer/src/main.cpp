#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/geology.hpp>
#include <hog/probe/messages.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/globe.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/presenter.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/viewer/clock_text.hpp>
#include <hog/viewer/daylight.hpp>
#include <hog/viewer/frame.hpp>
#include <hog/viewer/globe_view.hpp>
#include <hog/viewer/ground.hpp>
#include <hog/viewer/inspector.hpp>
#include <hog/viewer/link.hpp>
#include <hog/viewer/map_view.hpp>
#include <hog/viewer/options.hpp>
#include <hog/viewer/overlay.hpp>
#include <hog/viewer/texels.hpp>
#include <hog/window/ui.hpp>
#include <hog/window/window.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

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
// How long to wait between looks at a minimised window.
constexpr Uint32 idle_ms = 50;
constexpr int face_size = 512;      // pixels across each of the globe's faces
constexpr int globe_cells = 128;    // of its mesh, across each face
constexpr int relief_steps = 256;   // between the globe's heights across each face
constexpr float most_relief = 5.0F; // times the land's true height
constexpr int cloud_size = 256;     // texels across each face of the globe's clouds
constexpr VkFormat globe_depth = VK_FORMAT_D32_SFLOAT;
constexpr VkClearColorValue window_colour{
    .float32 = {20.0F / 255.0F, 20.0F / 255.0F, 24.0F / 255.0F, 1.0F}};
constexpr VkClearColorValue space_colour{
    .float32 = {6.0F / 255.0F, 6.0F / 255.0F, 10.0F / 255.0F, 1.0F}};

// The map's image on the GPU.
class MapTexture {
public:
    explicit MapTexture(render::StreamedImage image)
        : image_{std::move(image)}, ui_{image_.image().view()} {}

    [[nodiscard]] render::StreamedImage& image() noexcept { return image_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return image_.image().desc().extent; }
    [[nodiscard]] ImTextureRef ref() const noexcept { return ui_.ref(); }

private:
    render::StreamedImage image_;
    window::UiTexture ui_;
};

Result<void> wait_idle(const render::Device& device, std::string_view why) {
    if (const VkResult waited = vkDeviceWaitIdle(device.handle()); waited != VK_SUCCESS) {
        return render::vulkan_error(waited, why);
    }
    return {};
}

// Records replacing an image's texels, a layer per cube face, making it anew when its size or
// format changes.
Result<void> stream(const render::Device& device, const render::Frame& frame,
                    std::optional<render::StreamedImage>& image, VkFormat format, VkExtent2D extent,
                    std::span<const std::byte> texels) {
    if (!image || image->image().desc().format != format ||
        image->image().desc().extent.width != extent.width ||
        image->image().desc().extent.height != extent.height) {
        if (auto waited = wait_idle(device, "waiting to replace the globe's images"); !waited) {
            return waited;
        }
        image.reset();
        auto made = render::make_streamed_image(device, format, extent, render::frames_in_flight,
                                                planet::face_count);
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        image = std::move(made).value();
    }
    return image->update(frame.commands, frame.slot, texels);
}

// The globe, drawn into an image of its own that ImGui shows in the map's place.
class GlobePanel {
public:
    explicit GlobePanel(render::Globe globe) : globe_{std::move(globe)} {}

    // Whether there's an image to show, once drawn this frame.
    [[nodiscard]] bool showing() const noexcept {
        return target_ != nullptr && faces_ && faces_->ready() && relief_ && relief_->ready();
    }
    // Precondition: showing.
    [[nodiscard]] ImTextureRef ref() const noexcept { return target_->ui.ref(); }

    // What's been drawn for the globe and not yet sent to the GPU, taken as it's sent. None may be
    // null.
    struct Baked {
        std::optional<mapgen::Image>* faces;        // stacked from face 0 down
        std::optional<std::vector<float>>* heights; // as render_heights draws them
        // As render_clouds draws them; empty for none.
        std::optional<std::vector<std::uint8_t>>* clouds;
    };

    // Before the frame is laid out: sends what's newly baked to the GPU, and sizes the image to
    // `wanted`, if it's any size.
    Result<void> prepare(const render::Device& device, const render::Frame& frame, Baked baked,
                         VkExtent2D wanted) {
        if (auto sent = send(device, frame, baked); !sent) {
            return sent;
        }
        if (wanted.width == 0 || wanted.height == 0 ||
            (target_ != nullptr && target_->image.desc().extent.width == wanted.width &&
             target_->image.desc().extent.height == wanted.height)) {
            return {};
        }
        if (auto waited = wait_idle(device, "waiting to resize the globe"); !waited) {
            return waited;
        }
        target_.reset();
        auto image = render::make_image(
            device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                     .extent = wanted,
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
        if (!image) {
            return std::unexpected(std::move(image).error());
        }
        auto depth =
            render::make_image(device, {.format = globe_depth,
                                        .extent = wanted,
                                        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
        if (!depth) {
            return std::unexpected(std::move(depth).error());
        }
        target_ = std::make_unique<Target>(std::move(image).value(), std::move(depth).value());
        return {};
    }

    // Records drawing the globe into its image, ready for ImGui to show, with clouds if asked for
    // and there are any.
    void draw(VkCommandBuffer commands, const render::GlobeView& view, bool clouds) {
        if (!faces_ || !relief_ || !showing()) {
            return;
        }
        const VkExtent2D extent = target_->image.desc().extent;
        target_->tracked.use_as(commands, render::image_use::colour_target);
        target_->tracked_depth.use_as(commands, render::image_use::depth_target);
        render::begin_rendering(commands, target_->image.view(), extent, space_colour,
                                {.view = target_->depth.view()});
        const bool cloudy = clouds && clouded_ && clouds_ && clouds_->ready();
        globe_.draw(commands, extent,
                    {.faces = &faces_->image(),
                     .relief = &relief_->image(),
                     .clouds = cloudy ? &clouds_->image() : nullptr},
                    view);
        vkCmdEndRendering(commands);
        target_->tracked.use_as(commands, render::image_use::sampled);
    }

private:
    struct Target {
        Target(render::Image colour, render::Image depth_image)
            : image{std::move(colour)}, tracked{image.handle(), VK_IMAGE_ASPECT_COLOR_BIT},
              ui{image.view()}, depth{std::move(depth_image)},
              tracked_depth{depth.handle(), VK_IMAGE_ASPECT_DEPTH_BIT} {}

        render::Image image;
        render::TrackedImage tracked;
        window::UiTexture ui;
        render::Image depth;
        render::TrackedImage tracked_depth;
    };

    Result<void> send(const render::Device& device, const render::Frame& frame, Baked baked) {
        std::optional<mapgen::Image>& faces = *baked.faces;
        std::optional<std::vector<float>>& heights = *baked.heights;
        std::optional<std::vector<std::uint8_t>>& clouds = *baked.clouds;
        if (faces) {
            const VkExtent2D extent{.width = static_cast<std::uint32_t>(faces->width),
                                    .height = static_cast<std::uint32_t>(faces->width)};
            if (auto sent = stream(device, frame, faces_, VK_FORMAT_R8G8B8A8_UNORM, extent,
                                   viewer::rgba_texels(*faces));
                !sent) {
                return sent;
            }
            faces.reset();
        }
        if (heights) {
            constexpr auto across = static_cast<std::uint32_t>(relief_steps) + 1;
            if (auto sent =
                    stream(device, frame, relief_, VK_FORMAT_R32_SFLOAT,
                           {.width = across, .height = across}, std::as_bytes(std::span{*heights}));
                !sent) {
                return sent;
            }
            heights.reset();
        }
        if (clouds) {
            clouded_ = !clouds->empty();
            if (clouded_) {
                if (auto sent = stream(device, frame, clouds_, VK_FORMAT_R8G8B8A8_UNORM,
                                       {.width = cloud_size, .height = cloud_size},
                                       std::as_bytes(std::span{*clouds}));
                    !sent) {
                    return sent;
                }
            }
            clouds.reset();
        }
        return {};
    }

    render::Globe globe_;
    std::optional<render::StreamedImage> faces_;
    std::optional<render::StreamedImage> relief_;
    std::optional<render::StreamedImage> clouds_;
    bool clouded_ = false; // whether the newest clouds sent had any sky to draw from
    std::unique_ptr<Target> target_;
};

std::string format_time(GameTime time) {
    return std::format("{:%F %R} UTC",
                       std::chrono::sys_seconds{
                           std::chrono::floor<std::chrono::seconds>(time.time_since_epoch())});
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
    std::unique_ptr<MapTexture> texture;
    std::optional<mapgen::Image> pending; // drawn, but not yet sent to the GPU
    std::optional<sim::CellGrid> grid;
    std::optional<DVec2> sun;       // where it's overhead on the map, when daylight is shown
    std::uint64_t drawn = 0;        // the link's frame count when last drawn
    std::uint64_t drawn_ground = 0; // and its ground count
    viewer::MapRect drawn_shown;    // the part of the map the texture holds
    bool stale = true;              // the choices changed since
    std::optional<probe::Inspect> inspected;
    bool globe = false; // on show instead of the map
    viewer::GlobeCamera orbit;
    VkExtent2D globe_wanted{};          // pixels the globe took when last laid out
    std::optional<mapgen::Image> faces; // drawn for the globe, but not yet sent to the GPU
    std::uint64_t baked = 0;            // the link's frame count when the faces were last drawn
    std::uint64_t baked_ground = 0;     // and its ground count
    bool faces_stale = true;
    bool heights_baked = false; // with the land's elevation, since the edges changed
    bool clouds = true;         // over the globe
    bool clouds_stale = true;
    float relief = 1.0F;                       // times the land's true height, on the globe
    std::optional<std::vector<float>> heights; // drawn for the globe, but not yet sent to the GPU
    std::uint64_t heights_ground = 0;          // the link's ground count when last drawn
    std::optional<std::vector<std::uint8_t>> cloud_texels; // drawn, but not yet sent to the GPU
    std::uint64_t clouded = 0;      // the link's frame count when the clouds were last drawn
    std::vector<std::string> asked; // the layers the link was last asked for
};

// The globe's faces before there's anything to draw on them.
mapgen::Image missing_faces() {
    mapgen::Image image{.width = 1, .height = planet::face_count, .pixels = {}};
    for (int face = 0; face < planet::face_count; ++face) {
        image.pixels.insert(image.pixels.end(), viewer::missing_colour.begin(),
                            viewer::missing_colour.end());
    }
    return image;
}

const sim::CellGrid& grid_for(View& view, const viewer::Frame& frame) {
    const int level = frame.welcome().level;
    if (!view.grid || view.grid->level() != level) {
        view.grid.emplace(level);
    }
    return *view.grid;
}

void inspect(View& view, viewer::Link& link, std::optional<probe::Inspect> point) {
    view.inspected = point;
    link.inspect(point);
}

// Draws the newest frame into the map image, when the map's on show and there's something new to
// draw.
void redraw(View& view, const viewer::Link& link, int width, jobs::JobSystem& jobs) {
    const std::shared_ptr<const viewer::Frame> frame = link.frame();
    const viewer::MapRect shown = view.map.shown();
    if (view.globe || frame == nullptr ||
        (!view.stale && link.frames() == view.drawn && link.grounds() == view.drawn_ground &&
         shown == view.drawn_shown)) {
        return;
    }
    const std::uint64_t grounds = link.grounds();
    const std::shared_ptr<const probe::Ground> ground = link.ground();
    mapgen::Image image =
        viewer::render_overlay(grid_for(view, *frame), *frame, ground.get(), view.overlay,
                               view.species, shown, width, view.natural, jobs);
    view.sun.reset();
    if (const auto calendar = viewer::calendar_of(frame->welcome()); calendar && view.daylight) {
        viewer::shade_night(image, shown, *calendar, frame->time(), jobs);
        view.sun = viewer::subsolar_point(*calendar, frame->time());
    }
    view.pending = std::move(image);
    view.drawn = link.frames();
    view.drawn_ground = grounds;
    view.drawn_shown = shown;
    view.stale = false;
}

// Draws the newest frame onto the globe's faces, when the globe's on show and there's something new
// to draw.
void rebake(View& view, const viewer::Link& link, jobs::JobSystem& jobs) {
    const std::shared_ptr<const viewer::Frame> frame = link.frame();
    if (!view.globe || frame == nullptr) {
        return;
    }
    const std::uint64_t grounds = link.grounds();
    if (view.faces_stale || link.frames() != view.baked || grounds != view.baked_ground) {
        const std::shared_ptr<const probe::Ground> ground = link.ground();
        view.faces =
            viewer::render_faces(grid_for(view, *frame), *frame, ground.get(), view.overlay,
                                 view.species, face_size, view.natural, jobs);
        view.baked = link.frames();
        view.baked_ground = grounds;
        view.faces_stale = false;
    }
    // The land rises and falls only where it's surveyed, so its heights wait for that.
    if (!view.heights_baked || grounds != view.heights_ground) {
        const std::shared_ptr<const probe::Ground> ground = link.ground();
        view.heights = viewer::render_heights(grid_for(view, *frame), *frame, ground.get(),
                                              relief_steps, view.natural, jobs);
        view.heights_baked = !frame->layer("planet/elevation").empty();
        view.heights_ground = grounds;
    }
    if (view.clouds && (view.clouds_stale || link.frames() != view.clouded)) {
        view.cloud_texels =
            viewer::render_clouds(grid_for(view, *frame), *frame, cloud_size, view.natural, jobs);
        view.clouded = link.frames();
        view.clouds_stale = false;
    }
}

// Asks the link for the layers what's on show is drawn from, when they change.
void ask(View& view, viewer::Link& link) {
    std::vector<std::string> wanted =
        viewer::layers_for(view.overlay, view.species, view.globe && view.clouds);
    if (wanted != view.asked) {
        link.want(wanted);
        view.asked = std::move(wanted);
    }
}

// Sends the newest map image to the GPU, before anything this frame samples it.
Result<void> upload(View& view, const render::Device& device, const render::Frame& frame) {
    if (!view.pending) {
        return {};
    }
    const VkExtent2D extent{.width = static_cast<std::uint32_t>(view.pending->width),
                            .height = static_cast<std::uint32_t>(view.pending->height)};
    if (view.texture == nullptr || view.texture->extent().width != extent.width ||
        view.texture->extent().height != extent.height) {
        // Frames still in flight may be drawing the old one.
        if (auto waited = wait_idle(device, "waiting to replace the map"); !waited) {
            return waited;
        }
        view.texture.reset();
        auto image = render::make_streamed_image(device, VK_FORMAT_R8G8B8A8_UNORM, extent,
                                                 render::frames_in_flight);
        if (!image) {
            return std::unexpected(std::move(image).error());
        }
        view.texture = std::make_unique<MapTexture>(std::move(image).value());
    }
    if (auto updated = view.texture->image().update(frame.commands, frame.slot,
                                                    viewer::rgba_texels(*view.pending));
        !updated) {
        return updated;
    }
    view.pending.reset();
    return {};
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
    view.faces_stale = true;
    ask(view, link);
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
        view.faces_stale = true;
        view.heights_baked = false;
        view.clouds_stale = true;
    }

    ImGui::Separator();
    legend(view, link, frame.get());
    if (ImGui::RadioButton("map", !view.globe)) {
        view.globe = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("globe (Tab)", view.globe)) {
        view.globe = true;
    }
    ImGui::TextUnformatted((view.globe
                                ? std::format("altitude {:.2f} radii", view.orbit.orbit().altitude)
                                : std::format("zoom {:.1f}x", view.map.zoom()))
                               .c_str());
    if (view.globe) {
        ImGui::SliderFloat("relief", &view.relief, 0.0F, most_relief, "%.1fx");
        if (ImGui::Checkbox("clouds", &view.clouds)) {
            view.clouds_stale = true;
        }
    }
    if (ImGui::Button("whole planet")) {
        view.map.reset();
        view.orbit.reset();
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
    if (view.texture == nullptr || !view.texture->image().ready()) {
        draw->AddRectFilled(corner, far, ImGui::GetColorU32(ImGuiCol_FrameBg));
        return;
    }
    // The texture can be a frame behind the view; until it's redrawn, show its part of what's held.
    const viewer::MapRect shown = view.map.shown();
    const DVec2 low = viewer::shown_point(view.drawn_shown, shown.low);
    const DVec2 high = viewer::shown_point(view.drawn_shown, shown.high);
    draw->AddImage(view.texture->ref(), corner, far,
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

// The globe, filling its panel; dragging turns it, the wheel zooms and double-clicking resets.
void globe(View& view, const GlobePanel& panel) {
    const ImVec2 space = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(space.x, 1.0F), std::max(space.y, 1.0F)};
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("globe", size);
    const ImGuiIO& io = ImGui::GetIO();
    view.globe_wanted = {
        .width = static_cast<std::uint32_t>(std::lround(size.x * io.DisplayFramebufferScale.x)),
        .height = static_cast<std::uint32_t>(std::lround(size.y * io.DisplayFramebufferScale.y))};
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0.0F) {
            view.orbit.zoom(static_cast<double>(io.MouseWheel));
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            view.orbit.reset();
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        view.orbit.drag(DVec2{io.MouseDelta.x, io.MouseDelta.y}, static_cast<double>(size.y));
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 far{corner.x + size.x, corner.y + size.y};
    if (!panel.showing()) {
        draw->AddRectFilled(corner, far, ImGui::GetColorU32(ImGuiCol_FrameBg));
        return;
    }
    draw->AddImage(panel.ref(), corner, far);
}

// The whole viewer, laid out over the window.
void layout(View& view, viewer::Link& link, const GlobePanel& panel) {
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !ImGui::GetIO().WantTextInput) {
        view.globe = !view.globe;
    }
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
    if (view.globe) {
        globe(view, panel);
    } else {
        map(view, link);
    }
    ImGui::EndChild();
    ImGui::End();
}

// Draws and shows a frame, saving it to `capture` if given. False when there was nothing to draw
// into, such as while the window is minimised.
Result<bool> draw(View& view, viewer::Link& link, const render::Device& device,
                  render::Presenter& presenter, GlobePanel& panel, SDL_Window* window,
                  const std::optional<std::filesystem::path>& capture) {
    auto begun = presenter.begin(window::drawable(window));
    if (!begun) {
        return std::unexpected(std::move(begun).error());
    }
    if (!begun->has_value()) {
        return false;
    }
    render::Frame& frame = begun->value();
    if (auto uploaded = upload(view, device, frame); !uploaded) {
        return std::unexpected(std::move(uploaded).error());
    }
    if (auto prepared = panel.prepare(
            device, frame,
            {.faces = &view.faces, .heights = &view.heights, .clouds = &view.cloud_texels},
            view.globe ? view.globe_wanted : VkExtent2D{});
        !prepared) {
        return std::unexpected(std::move(prepared).error());
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    layout(view, link, panel);
    ImGui::Render();
    if (view.globe) {
        panel.draw(frame.commands,
                   viewer::globe_view(
                       view.orbit, link.frame().get(),
                       {.daylight = view.daylight, .relief = static_cast<double>(view.relief)}),
                   view.clouds);
    }

    frame.target.use_as(frame.commands, render::image_use::colour_target);
    render::begin_rendering(frame.commands, frame.view, frame.extent, window_colour);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commands);
    vkCmdEndRendering(frame.commands);

    std::optional<Result<render::Buffer>> captured;
    if (capture) {
        captured = start_capture(device, presenter, frame);
    }
    if (auto ended = presenter.end(frame); !ended) {
        return std::unexpected(std::move(ended).error());
    }
    if (capture && captured) {
        const Result<void> saved = captured->has_value()
                                       ? save_capture(device, captured->value(), frame, *capture)
                                       : std::unexpected(captured->error());
        if (!saved) {
            std::cerr << std::format("hog-viewer: can't capture the window: {}\n", saved.error());
        }
    }
    return true;
}

// Shows the server's world until the window closes or it's time to quit.
int show(const viewer::Options& options, const render::Device& device, render::Presenter& presenter,
         SDL_Window* window) {
    jobs::JobSystem jobs;
    viewer::Link link{options.host, options.port, viewer::Link::Timing{}};
    View view;
    view.overlay = options.overlay;
    view.species = options.species;
    view.globe = options.globe;
    view.faces = missing_faces();
    // Level ground until the land's known.
    view.heights.emplace(std::size_t{planet::face_count} * (relief_steps + 1) * (relief_steps + 1));
    auto made = render::Globe::create(device, VK_FORMAT_R8G8B8A8_UNORM, globe_depth, globe_cells);
    if (!made) {
        std::cerr << std::format("hog-viewer: can't draw the globe: {}\n", made.error());
        return EXIT_FAILURE;
    }
    GlobePanel panel{std::move(made).value()};

    int status = EXIT_SUCCESS;
    const MonotonicClock::time_point started = MonotonicClock::now();
    for (bool running = true; running;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
        ask(view, link);
        survey(view, link, options.width);
        redraw(view, link, options.width, jobs);
        rebake(view, link, jobs);
        if (options.quit_after && MonotonicClock::now() - started >= *options.quit_after) {
            running = false;
        }
        const auto drawn = draw(view, link, device, presenter, panel, window,
                                running ? std::nullopt : options.capture);
        if (!drawn) {
            std::cerr << std::format("hog-viewer: {}\n", drawn.error());
            status = EXIT_FAILURE;
            break;
        }
        if (!*drawn) {
            SDL_Delay(idle_ms);
        }
    }
    // The globe and the map's texture go first, and the GPU may still be drawing with them.
    vkDeviceWaitIdle(device.handle());
    if (options.validate && device.validation_errors() != 0) {
        std::cerr << std::format("hog-viewer: {} Vulkan validation errors\n",
                                 device.validation_errors());
        status = EXIT_FAILURE;
    }
    return status;
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
    const window::Sdl sdl;
    if (!sdl.started()) {
        std::cerr << std::format("hog-viewer: can't start SDL: {}\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    if (!sdl.vulkan()) {
        std::cerr << std::format("hog-viewer: can't load Vulkan: {}\n", SDL_GetError());
        return window::no_vulkan;
    }
    const window::Window window{
        SDL_CreateWindow("HOG viewer", 1'400, 800,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)};
    if (window == nullptr) {
        std::cerr << std::format("hog-viewer: can't open a window: {}\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    const auto wanted =
        window::device_options(window.get(), {.app_name = "HOG viewer",
                                              .validation = options->validate,
                                              .prefer_software = options->software});
    if (!wanted) {
        std::cerr << std::format("hog-viewer: {}\n", wanted.error());
        return window::no_vulkan;
    }
    const auto device = render::Device::create(*wanted);
    if (!device) {
        std::cerr << std::format("hog-viewer: can't draw: {}\n", device.error());
        return device.error().code == ErrorCode::Unsupported ? window::no_vulkan : EXIT_FAILURE;
    }
    // ImGui's colours are already sRGB, so the window's images take them as they are.
    const auto presenter =
        render::Presenter::create(**device, window::drawable(window.get()), {.srgb = false});
    if (!presenter) {
        std::cerr << std::format("hog-viewer: can't show frames: {}\n", presenter.error());
        return EXIT_FAILURE;
    }
    // The map's texture besides.
    const window::Ui ui{window.get(), **device, (*presenter)->swapchain().format(), 1};
    if (!ui.started()) {
        std::cerr << "hog-viewer: can't start ImGui's Vulkan backend\n";
        return EXIT_FAILURE;
    }

    return show(*options, **device, **presenter, window.get());
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

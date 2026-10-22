#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/render/device.hpp>
#include <hog/render/ocean.hpp>
#include <hog/shaders/ocean.hpp>
#include <hog/terrain/sea.hpp>
#include <hog/terrain/shore.hpp>
#include <hog/waves/foam.hpp>
#include <hog/waves/run_up.hpp>
#include <hog/waves/shoaling.hpp>
#include <hog/waves/tide.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

double square(double x) {
    return x * x;
}

using terrain::SeaVertex;

static_assert(std::is_trivially_copyable_v<SeaVertex> && std::is_standard_layout_v<SeaVertex>);

// Triangles about as long as they're wide, a few pixels across at 1080p.
constexpr int rings = 96;
constexpr int spokes = 128;

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 up_height;
    glm::vec4 east_faked;
    glm::vec4 sun_nearest;
    glm::vec4 sky_furthest;
};

// Within what every device can push.
static_assert(sizeof(Push) <= 128);

constexpr std::array vertex_bindings{VkVertexInputBindingDescription{
    .binding = 0, .stride = sizeof(SeaVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
constexpr std::array vertex_attributes{
    VkVertexInputAttributeDescription{.location = 0,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32_SFLOAT,
                                      .offset = offsetof(SeaVertex, out)},
    VkVertexInputAttributeDescription{.location = 1,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32_SFLOAT,
                                      .offset = offsetof(SeaVertex, towards)}};

static_assert(sizeof(DrawnWaves) == (13 * sizeof(glm::vec4)) +
                                        (drawn_waves * sizeof(DrawnWaves::Wave)) +
                                        (foam_waves * sizeof(DrawnWaves::FoamWave)));

// The foam's pattern: plane waves fixed in the planet's frame, from the size of a whitecap to a
// hand's breadth, each drifting slowly one way or the other so the pattern changes as foam does,
// the finest soonest. The longer half places the foam's patches, the shorter its lace.
struct PatternWave {
    DVec3 heading{1.0, 0.0, 0.0};
    double wavenumber = 0.0;
    double frequency = 0.0;
    double phase = 0.0;
    double amplitude = 0.0;
};

// The lengths of its coarse waves, then its fine, m.
constexpr std::array<double, 2> longest_foam{30.0, 3.0};
constexpr std::array<double, 2> shortest_foam{4.0, 0.4};
constexpr std::size_t coarse_foam = 12; // as waves.slang has it
// How fast its parts drift, m/s, and how much of its variance lies in its longer parts.
constexpr double foam_changing = 0.15;
constexpr double foam_coarseness = 0.5;
// How much of the lace's waves along the wind it streaks leaves out, by how much it streaks.
constexpr double streaking_down = 0.5;
// How much whitecaps churn, against how much the waves that break bunch; as waves.slang has it.
constexpr double foam_ragged = 0.4;
// As waves.slang's breaking_edges and foam_edges.
constexpr double breaking_edges = 0.12;
constexpr double foam_edges = 0.3;

const std::array<PatternWave, foam_waves>& foam_pattern() {
    static const std::array<PatternWave, foam_waves> pattern = [] {
        constexpr std::uint64_t seed = 0x6f616d;
        const auto uniform = [](std::size_t i, int which) {
            return static_cast<double>(hash_values(seed, i, which) >> 11U) * 0x1.0p-53;
        };
        std::array<PatternWave, foam_waves> made{};
        for (std::size_t i = 0; i < made.size(); ++i) {
            const std::size_t part = i < coarse_foam ? 0 : 1;
            const std::size_t first = part == 0 ? 0 : coarse_foam;
            const std::size_t count = part == 0 ? coarse_foam : foam_waves - coarse_foam;
            const double length =
                longest_foam.at(part) *
                std::pow(shortest_foam.at(part) / longest_foam.at(part),
                         static_cast<double>(i - first) / static_cast<double>(count - 1));
            const double z = (2.0 * uniform(i, 0)) - 1.0;
            const double around = 2.0 * std::numbers::pi * uniform(i, 1);
            const double out = std::sqrt(1.0 - (z * z));
            const double wavenumber = 2.0 * std::numbers::pi / length;
            made.at(i) = {.heading = {out * std::cos(around), out * std::sin(around), z},
                          .wavenumber = wavenumber,
                          .frequency = (i % 2 == 0 ? 1.0 : -1.0) * wavenumber * foam_changing,
                          .phase = 2.0 * std::numbers::pi * uniform(i, 2),
                          .amplitude = std::pow(length, foam_coarseness)};
        }
        return made;
    }();
    return pattern;
}

// The foam's pattern as it lies round the eye: waves running steeply into the ground left out,
// and of its lace, those along a gale, so the rest streak along it; each half summing to a
// variance of 1.
std::array<DrawnWaves::FoamWave, foam_waves> foam_pattern_at(const waves::SeaState& sea,
                                                             WorldPosition eye, DVec3 up,
                                                             GameTime time, DVec3 drifted) {
    const double wind = glm::length(sea.wind);
    const double streaks = waves::streaking(wind);
    const DVec3 downwind = wind > 0.0 ? sea.wind / wind : DVec3{0.0};
    const double seconds = std::chrono::duration<double>(time.time_since_epoch()).count();
    const DVec3 carried = eye.metres() - drifted;
    std::array<DrawnWaves::FoamWave, foam_waves> drawn{};
    std::array<double, foam_waves> amplitudes{};
    std::array<double, 2> variances{};
    for (std::size_t i = 0; i < foam_waves; ++i) {
        const PatternWave& wave = foam_pattern().at(i);
        const DVec3 along = wave.heading - (up * glm::dot(up, wave.heading));
        const double flat = glm::length(along);
        const double lying = std::clamp((flat - 0.5) / 0.3, 0.0, 1.0);
        const double with_wind = flat > 0.0 ? glm::dot(along / flat, downwind) : 0.0;
        const double streaked =
            i < coarse_foam ? 1.0
                            : std::exp(-(streaks - 1.0) * streaking_down * with_wind * with_wind);
        amplitudes.at(i) = wave.amplitude * lying * streaked;
        variances.at(i < coarse_foam ? 0 : 1) += amplitudes.at(i) * amplitudes.at(i) / 2.0;
        const double phase = std::remainder((wave.wavenumber * glm::dot(wave.heading, carried)) -
                                                (wave.frequency * seconds) + wave.phase,
                                            2.0 * std::numbers::pi);
        drawn.at(i).wavenumber_phase = {Vec3{wave.heading * wave.wavenumber},
                                        static_cast<float>(phase)};
        drawn.at(i).amplitude_length.y =
            flat > 0.0 ? static_cast<float>(2.0 * std::numbers::pi / (wave.wavenumber * flat))
                       : 0.0F;
        drawn.at(i).amplitude_length.z = static_cast<float>(wave.wavenumber * flat);
    }
    for (std::size_t i = 0; i < foam_waves; ++i) {
        const double variance = variances.at(i < coarse_foam ? 0 : 1);
        drawn.at(i).amplitude_length.x =
            variance > 0.0 ? static_cast<float>(amplitudes.at(i) / std::sqrt(variance)) : 0.0F;
    }
    return drawn;
}

constexpr VkDescriptorSetLayoutBinding
binding(std::uint32_t at, VkDescriptorType type,
        VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

// As ocean.slang has them.
constexpr std::uint32_t waves_binding = 6;
constexpr VkDescriptorSetLayoutBinding waves_descriptor =
    binding(waves_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

constexpr std::uint32_t shore_binding = 7;
constexpr VkDescriptorSetLayoutBinding shore_descriptor =
    binding(shore_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_VERTEX_BIT);

constexpr std::array descriptors{waves_descriptor, shore_descriptor};
constexpr std::array air_descriptors{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                     binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                     binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     waves_descriptor,
                                     shore_descriptor};

// How the swash of the waves, and of their groups' surges, turns along the ground: fixed to the
// planet, each a way and a length, so it runs up one stretch of shore while it draws back from the
// next.
struct Swash {
    DVec3 way;
    double length;
};
constexpr Swash swash_incident{.way = DVec3{0.8, 0.6, 0.0}, .length = 90.0};
constexpr Swash swash_surging{.way = DVec3{-0.6, 0.0, 0.8}, .length = 400.0};

// Of the waves shown, as waves::run_up takes them: the root of their significant height times
// their peak's wavelength, in deep water; the depth at which they start to break; and how fast
// their swash, and their groups', turn.
glm::vec4 run_up_at(std::span<const waves::Train> trains, std::span<const waves::Wave> shown) {
    std::array<double, waves::bands> variances{};
    std::array<double, waves::bands> frequencies{};
    for (const waves::Wave& wave : shown) {
        const waves::Train& train = trains[wave.train];
        const auto band = static_cast<std::size_t>(train.band);
        variances.at(band) += wave.amplitude * wave.amplitude / 2.0;
        frequencies.at(band) = train.frequency;
    }
    double variance = 0.0;
    std::size_t peak = 0;
    for (std::size_t band = 0; band < variances.size(); ++band) {
        variance += variances.at(band);
        if (variances.at(band) > variances.at(peak)) {
            peak = band;
        }
    }
    if (!(variance > 0.0) || !(frequencies.at(peak) > 0.0)) {
        return glm::vec4{0.0F};
    }
    const double height = 4.0 * std::sqrt(variance);
    const double turning = frequencies.at(peak);
    const double period = 2.0 * std::numbers::pi / turning;
    // As run_up has it, with the beach's slope left for the shaders.
    const double scale =
        std::sqrt(height * waves::gravity * period * period / (2.0 * std::numbers::pi));
    return {static_cast<float>(scale),
            static_cast<float>(height / (std::numbers::sqrt2 * waves::breaking_depth)),
            static_cast<float>(turning), static_cast<float>(turning / waves::surging)};
}

// `swash` turning `turning` rad/s, as it lies through the eye at `eye` at `time`.
glm::vec4 swash_at(const Swash& swash, double turning, WorldPosition eye, GameTime time) {
    const double wavenumber = 2.0 * std::numbers::pi / swash.length;
    const double seconds = std::chrono::duration<double>(time.time_since_epoch()).count();
    return {Vec3{swash.way * wavenumber},
            static_cast<float>(std::remainder((turning * seconds) +
                                                  (wavenumber * glm::dot(swash.way, eye.metres())),
                                              2.0 * std::numbers::pi))};
}

} // namespace

DrawnWaves drawn_waves_at(std::span<const waves::Train> trains, std::span<const waves::Wave> waves,
                          const waves::SeaState& sea, WorldPosition eye, const terrain::Bend& bend,
                          GameTime time, DVec3 drifted, GameDuration day) {
    const double wind = glm::length(sea.wind);
    const auto breaking_slope = [&](const waves::Wave& wave) {
        const waves::Train& train = trains[wave.train];
        const double slope =
            train.wavenumber * wave.amplitude * waves::breaking_share(train.frequency, wind);
        return slope * slope / 2.0;
    };
    std::vector<waves::Wave> shown;
    double breaking_all = 0.0;
    for (const waves::Wave& wave : waves) {
        const double showing =
            std::sqrt(waves::showing(glm::dot(bend.up, trains[wave.train].heading)));
        if (showing > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing});
            breaking_all += breaking_slope(shown.back());
        }
    }
    const waves::Strongest strongest = waves::strongest(trains, shown, drawn_waves);
    const planet::TangentFrame frame = planet::tangent_frame(bend.up);
    // The longest wave drawn feels the floor from deepest.
    double longest = std::numeric_limits<double>::infinity();
    for (const waves::Wave& wave : strongest.waves) {
        const waves::Train& train = trains[wave.train];
        longest = std::min(longest, train.wavenumber *
                                        std::sqrt(1.0 - square(glm::dot(bend.up, train.heading))));
    }
    DrawnWaves drawn{.count_left_radius = {static_cast<float>(strongest.waves.size()),
                                           static_cast<float>(strongest.slope_left),
                                           static_cast<float>(bend.radius),
                                           static_cast<float>(waves::deep_enough / longest)},
                     .up_height = {Vec3{bend.up}, static_cast<float>(bend.eye_height)},
                     .first = {Vec3{frame.east}, 0.0F},
                     .second = {Vec3{frame.north}, 0.0F}};
    double shorter = 0.0;
    double heave = 0.0;
    double breaking_shorter = 0.0;
    for (std::size_t i = strongest.waves.size(); i-- > 0;) {
        const waves::Wave& wave = strongest.waves[i];
        const waves::Train& train = trains[wave.train];
        const DVec3 along = train.heading - (bend.up * glm::dot(bend.up, train.heading));
        const double flat = glm::length(along);
        const double wavenumber = train.wavenumber * flat;
        const double slope = wavenumber * wave.amplitude;
        const double share = waves::breaking_share(train.frequency, wind);
        shorter += slope * slope / 2.0;
        heave += wave.amplitude * wave.amplitude / 2.0;
        breaking_shorter += slope * slope * share * share / 2.0;
        breaking_all -= breaking_slope(wave);
        const waves::LagTerms lag = waves::lag_terms(wavenumber);
        drawn.waves.at(i) = {
            .heading_wavenumber = {Vec3{train.heading}, static_cast<float>(train.wavenumber)},
            .amplitude_phase_slope_heave = {static_cast<float>(wave.amplitude),
                                            static_cast<float>(
                                                waves::phase_at(train, eye.metres(), time)),
                                            static_cast<float>(shorter), static_cast<float>(heave)},
            .along = {static_cast<float>(glm::dot(along, frame.east) / flat),
                      static_cast<float>(glm::dot(along, frame.north) / flat),
                      static_cast<float>(wavenumber), static_cast<float>(share)},
            .breaking = {static_cast<float>(breaking_shorter), static_cast<float>(lag.turn),
                         static_cast<float>(lag.scale), static_cast<float>(lag.offset)}};
    }
    // What's left over is what wasn't drawn; the bunching of all that breaks, drawn or not, is
    // near enough normal, so whitecaps cover as much as they should where it's highest.
    const double breaking_left = std::max(breaking_all, 0.0);
    const waves::Whitecaps white = waves::whitecaps(sea);
    const double breaking = breaking_shorter + breaking_left;
    // What's left lies in patches, in each as a lace along where the pattern crosses its mean, so
    // wide as to cover as much as is left in all; each share found as blurred over the foam's
    // edges. The surf leaves foam in the pattern too, wind or none.
    const double blurred = std::sqrt(1.0 + (foam_edges * foam_edges));
    const double patches = std::sqrt(white.left);
    drawn.foam_left = {static_cast<float>(blurred * waves::exceeded(patches)),
                       static_cast<float>(blurred * waves::exceeded((1.0 - patches) / 2.0)), 0.0F,
                       0.0F};
    drawn.foam_pattern = foam_pattern_at(sea, eye, bend.up, time, drifted);
    // Over the few kilometres seen, the tide rises evenly enough along the ground.
    constexpr double tide_step = 1'000.0;
    const auto tide_towards = [&](const DVec3& way) {
        return (waves::tide_at(bend.up + (way * (tide_step / bend.radius)), time, day) -
                waves::tide_at(bend.up - (way * (tide_step / bend.radius)), time, day)) /
               (2.0 * tide_step);
    };
    drawn.tide = {
        Vec3{(frame.east * tide_towards(frame.east)) + (frame.north * tide_towards(frame.north))},
        static_cast<float>(waves::tide_at(bend.up, time, day))};
    drawn.run_up = run_up_at(trains, shown);
    drawn.swash_incident = swash_at(swash_incident, drawn.run_up.z, eye, time);
    drawn.swash_surging = swash_at(swash_surging, drawn.run_up.w, eye, time);
    if (white.breaking > 0.0 && breaking > 0.0) {
        // Whitecaps are ragged: their own churning adds to how much the waves bunch.
        const double ragged = foam_ragged * std::sqrt(breaking);
        drawn.foam = {static_cast<float>(std::sqrt(breaking * (1.0 + (foam_ragged * foam_ragged) +
                                                               (breaking_edges * breaking_edges))) *
                                         waves::exceeded(white.breaking)),
                      static_cast<float>(breaking_left), static_cast<float>(white.breaking),
                      static_cast<float>(white.left)};
        drawn.foam_left.z = static_cast<float>(std::sqrt(breaking));
        drawn.foam_left.w = static_cast<float>(ragged);
    }
    return drawn;
}

Result<Ocean> Ocean::create(Device& device, VkFormat colour_format, VkFormat depth_format, bool air,
                            std::uint32_t frames) {
    Ocean ocean;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto waves = make_buffer(device, sizeof(DrawnWaves), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 Memory::Upload);
        if (!waves) {
            return std::unexpected(std::move(waves).error());
        }
        // A calm sea at its mean, for ground drawn before the sea's first drawn.
        const DrawnWaves calm{};
        std::memcpy(waves->mapped().data(), &calm, sizeof(calm));
        if (auto flushed = waves->flush(); !flushed) {
            return std::unexpected(std::move(flushed).error());
        }
        ocean.waves_.push_back(std::move(waves).value());
    }
    const terrain::SeaDisc disc = terrain::sea_disc(rings, spokes);
    auto vertices = make_filled_buffer(device, std::as_bytes(std::span{disc.vertices}),
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vertices) {
        return std::unexpected(std::move(vertices).error());
    }
    ocean.vertices_ = std::move(vertices).value();
    auto indices = make_filled_buffer(device, std::as_bytes(std::span{disc.indices}),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!indices) {
        return std::unexpected(std::move(indices).error());
    }
    ocean.indices_ = std::move(indices).value();
    ocean.index_count_ = static_cast<std::uint32_t>(disc.indices.size());
    auto shore = make_streamed_image(
        device, VK_FORMAT_R16_SFLOAT, {.width = terrain::shore_size, .height = terrain::shore_size},
        frames, static_cast<std::uint32_t>(terrain::shore_spacing.size()));
    if (!shore) {
        return std::unexpected(std::move(shore).error());
    }
    ocean.shore_depths_ = std::move(shore).value();
    auto sampler = make_sampler(device, VK_FILTER_LINEAR);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    ocean.shore_sampler_ = std::move(sampler).value();
    // Deep water, until there are depths.
    const std::vector<std::uint16_t> deep_water =
        shore_texels(std::vector<float>(terrain::shore_spacing.size(), 1'000.0F));
    auto none =
        make_filled_image(device,
                          {.format = VK_FORMAT_R16_SFLOAT,
                           .layers = static_cast<std::uint32_t>(terrain::shore_spacing.size())},
                          std::as_bytes(std::span{deep_water}));
    if (!none) {
        return std::unexpected(std::move(none).error());
    }
    ocean.no_shore_ = std::move(none).value();

    // Seen from below too, from under the sea.
    ocean.air_ = air;
    auto pipeline = make_pipeline(
        device, {.spirv = shaders::ocean,
                 .fragment_entry = air ? "fragment_air" : "fragment_main",
                 .bindings = vertex_bindings,
                 .attributes = vertex_attributes,
                 .colour_format = colour_format,
                 .depth_format = depth_format,
                 .blend = Blend::Premultiplied,
                 .push_constant_bytes = sizeof(Push),
                 .descriptors = air ? std::span<const VkDescriptorSetLayoutBinding>{air_descriptors}
                                    : std::span<const VkDescriptorSetLayoutBinding>{descriptors}});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    ocean.pipeline_ = std::move(pipeline).value();
    if (air) {
        auto deep = make_pipeline(device, {.spirv = shaders::ocean,
                                           .fragment_entry = "fragment_deep",
                                           .bindings = vertex_bindings,
                                           .attributes = vertex_attributes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .depth_write = false,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = air_descriptors});
        if (!deep) {
            return std::unexpected(std::move(deep).error());
        }
        ocean.deep_ = std::move(deep).value();
    }
    return ocean;
}

std::vector<std::uint16_t> shore_texels(std::span<const float> depths) {
    std::vector<std::uint16_t> halves;
    halves.reserve(depths.size());
    for (const float depth : depths) {
        halves.push_back(static_cast<std::uint16_t>(glm::packHalf1x16(depth)));
    }
    return halves;
}

Result<> Ocean::update_shore(VkCommandBuffer commands, std::uint32_t frame,
                             const terrain::ShoreFrame& shore, std::span<const float> depths) {
    const std::vector<std::uint16_t> halves = shore_texels(depths);
    if (auto updated = shore_depths_.update(commands, frame, std::as_bytes(std::span{halves}));
        !updated) {
        return updated;
    }
    shore_ = shore;
    return {};
}

SeaBindings Ocean::bindings(std::uint32_t frame) const {
    return {.waves = waves_.at(frame).handle(),
            .shore = shore_ ? shore_depths_.image().view() : no_shore_.view(),
            .sampler = shore_sampler_.handle()};
}

Result<> Ocean::draw(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                     std::uint32_t frame) const {
    return record(commands, extent, view, frame, pipeline_);
}

Result<> Ocean::draw_deep(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                          std::uint32_t frame) const {
    HOG_ASSERT(air_, "the deep water drawn without the air");
    // From under the sea, the sky shows through its surface overhead.
    if (view.bend.eye_height > 0.0) {
        return record(commands, extent, view, frame, deep_);
    }
    return {};
}

Result<> Ocean::record(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                       std::uint32_t frame, const Pipeline& pipeline) const {
    if (extent.width == 0 || extent.height == 0) {
        return {};
    }
    // A calm sea too lies on the planet round the eye.
    DrawnWaves drawn = view.waves;
    const planet::TangentFrame ground = planet::tangent_frame(view.bend.up);
    drawn.count_left_radius.z = static_cast<float>(view.bend.radius);
    drawn.up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)};
    drawn.first = {Vec3{ground.east}, 0.0F};
    drawn.second = {Vec3{ground.north}, 0.0F};
    if (shore_) {
        const DVec3 eye = view.bend.up * (view.bend.radius + view.bend.eye_height);
        drawn.shore_anchor = {Vec3{(shore_->anchor * view.bend.radius) - eye},
                              static_cast<float>(1.0 / terrain::shore_spacing[0])};
        drawn.shore_east = {Vec3{shore_->east},
                            static_cast<float>(1.0 / terrain::shore_spacing[1])};
        drawn.shore_north = {Vec3{shore_->north}, 0.0F};
    }
    const Buffer& waves = waves_.at(frame);
    std::memcpy(waves.mapped().data(), &drawn, sizeof(drawn));
    if (auto flushed = waves.flush(); !flushed) {
        return flushed;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const terrain::SeaReach reach = terrain::sea_reach(view.bend);
    const Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
                    .up_height =
                        glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
                    .east_faked = glm::vec4{Vec3{planet::tangent_frame(view.bend.up).east},
                                            static_cast<float>(view.bend.faked)},
                    .sun_nearest = glm::vec4{Vec3{view.sun}, static_cast<float>(reach.nearest)},
                    .sky_furthest = glm::vec4{view.sky, static_cast<float>(reach.furthest)}};

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    if (air_) {
        HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE,
                   "drawing the sea lit by the air without it");
        push_uniform_buffer(commands, pipeline, 0, view.air.air);
        push_uniform_buffer(commands, pipeline, 1, view.air.lighting);
        push_sampled_image(commands, pipeline, 2, view.air.transmittance, view.air.sampler);
        push_sampled_image(commands, pipeline, 3, view.air.sky_view, view.air.sampler);
        push_sampled_image(commands, pipeline, 4, view.air.aerial, view.air.sampler);
        push_sampled_image(commands, pipeline, 5, view.air.cloud_shadow, view.air.sampler);
    }
    push_uniform_buffer(commands, pipeline, waves_binding, waves.handle());
    push_sampled_image(commands, pipeline, shore_binding,
                       shore_ ? shore_depths_.image().view() : no_shore_.view(),
                       shore_sampler_.handle());
    const std::array vertices{vertices_.handle()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, vertices.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices_.handle(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(commands, pipeline.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                       &push);
    vkCmdDrawIndexed(commands, index_count_, 1, 0, 0, 0);
    return {};
}

} // namespace hog::render

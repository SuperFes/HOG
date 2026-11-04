#include <hog/client/globe_view.hpp>
#include <hog/core/assert.hpp>
#include <hog/core/log.hpp>
#include <hog/core/text_file.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hog::client {

Result<globe::Planet> globe_planet(std::optional<std::string_view> named, std::uint64_t seed,
                                   double radius) {
    globe::Planet planet;
    if (named) {
        if (const auto preset = globe::preset(*named)) {
            planet = *preset;
        } else {
            // A planet file sets its own size.
            auto loaded = globe::load_planet(std::string{*named});
            if (loaded) {
                loaded->seed = seed;
            }
            return loaded;
        }
    }
    planet.seed = seed;
    planet.radius = radius;
    if (auto valid = globe::validate(planet); !valid) {
        return std::unexpected(std::move(valid).error());
    }
    return planet;
}

Result<globe::Planet> globe_planet_in(const std::filesystem::path& file, std::uint64_t seed,
                                      double radius) {
    const auto text = read_text_file(file);
    if (!text || !text->contains("[globe]")) {
        return globe_planet(std::nullopt, seed, radius);
    }
    auto described = globe::parse_planet(*text);
    if (described) {
        described->seed = seed;
    }
    return described;
}

double globe_ground(const globe::Generator& generator, DVec3 at) {
    const globe::Planet& planet = generator.planet();
    return globe::ground_below(generator, glm::normalize(at), globe::voxel_lod(generator))
               .value_or(generator.layers().floor) -
           planet.radius;
}

Vec3 surface_colour(globe::Material material) {
    using globe::Material;
    switch (material) {
    case Material::Air:
        return {0.8F, 0.9F, 1.0F};
    case Material::Water:
        return {0.05F, 0.2F, 0.45F};
    case Material::Snow:
        return {0.9F, 0.92F, 0.95F};
    case Material::Soil:
        return {0.3F, 0.22F, 0.12F};
    case Material::Sand:
        return {0.76F, 0.68F, 0.5F};
    case Material::Sediment:
        return {0.6F, 0.52F, 0.36F};
    case Material::Sandstone:
        return {0.7F, 0.5F, 0.3F};
    case Material::Shale:
        return {0.4F, 0.38F, 0.35F};
    case Material::Limestone:
        return {0.75F, 0.73F, 0.65F};
    case Material::Granite:
        return {0.55F, 0.5F, 0.48F};
    case Material::Basalt:
        return {0.18F, 0.18F, 0.2F};
    case Material::Mantle:
        return {0.3F, 0.36F, 0.22F};
    case Material::Iron:
        return {0.42F, 0.4F, 0.38F};
    case Material::Anorthosite:
        return {0.7F, 0.7F, 0.68F};
    case Material::Regolith:
        return {0.52F, 0.5F, 0.48F};
    }
    return {1.0F, 0.0F, 1.0F};
}

std::array<terrain::Look, globe::material_count> globe_looks() {
    using globe::Material;
    struct Variation {
        Material material;
        DVec3 accent; // relative to the colour
        double share;
        double grain;
        double mottle;
    };
    constexpr auto vary = [](Material material, DVec3 accent, double share, double grain,
                             double mottle) {
        return Variation{.material = material,
                         .accent = accent,
                         .share = share,
                         .grain = grain,
                         .mottle = mottle};
    };
    constexpr std::array variations{vary(Material::Air, {1.0, 1.0, 1.0}, 0.0, 0.0, 0.0),
                                    vary(Material::Water, {1.0, 1.0, 1.0}, 0.0, 0.05, 0.1),
                                    // Blue in the hollows, grey where it's old.
                                    vary(Material::Snow, {0.9, 0.95, 1.05}, 0.3, 0.08, 0.08),
                                    // Drier, paler patches.
                                    vary(Material::Soil, {1.3, 1.15, 0.9}, 0.3, 0.35, 0.25),
                                    vary(Material::Sand, {1.1, 1.05, 0.95}, 0.3, 0.2, 0.12),
                                    // Greyer where it's wet.
                                    vary(Material::Sediment, {0.8, 0.8, 0.85}, 0.3, 0.3, 0.2),
                                    // Buff, with rusty patches.
                                    vary(Material::Sandstone, {0.85, 0.7, 0.6}, 0.3, 0.3, 0.2),
                                    // Dark grey, browner where it weathers.
                                    vary(Material::Shale, {1.15, 1.0, 0.85}, 0.3, 0.35, 0.15),
                                    // Pale, with darker lichen.
                                    vary(Material::Limestone, {0.75, 0.75, 0.72}, 0.3, 0.25, 0.3),
                                    // Speckled, bluer where the feldspar shows.
                                    vary(Material::Granite, {1.0, 1.2, 1.4}, 0.3, 0.5, 0.15),
                                    // Near black, rusting.
                                    vary(Material::Basalt, {1.4, 1.1, 0.9}, 0.2, 0.35, 0.25),
                                    vary(Material::Mantle, {0.8, 0.9, 0.7}, 0.3, 0.3, 0.2),
                                    vary(Material::Iron, {1.3, 1.1, 0.9}, 0.3, 0.3, 0.2),
                                    vary(Material::Anorthosite, {0.9, 0.9, 0.92}, 0.3, 0.3, 0.2),
                                    vary(Material::Regolith, {0.85, 0.85, 0.85}, 0.35, 0.3, 0.25)};
    static_assert(variations.size() == globe::material_count);

    std::array<terrain::Look, globe::material_count> looks{};
    for (const Variation& variation : variations) {
        const auto material = static_cast<std::size_t>(variation.material);
        const DVec3 colour{surface_colour(variation.material)};
        looks.at(material) = {.colour = colour,
                              .accent = glm::min(colour * variation.accent, DVec3{1.0}),
                              .accent_share = variation.share,
                              .grain = variation.grain,
                              .mottle = variation.mottle,
                              .seed = 0x1000 + material};
    }
    return looks;
}

std::vector<terrain::MaterialImage> block_tiles() {
    std::vector<terrain::MaterialImage> tiles;
    tiles.reserve(globe::material_count);
    for (const terrain::Look& look : globe_looks()) {
        tiles.push_back(terrain::material_image(look, material_size));
    }
    return tiles;
}

terrain::BlockMesh globe_blocks(const globe::SurfaceMesh& mesh) {
    terrain::BlockMesh blocks{.origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = {}};
    blocks.vertices.reserve(mesh.vertices.size());
    for (const globe::SurfaceVertex& vertex : mesh.vertices) {
        const auto tile = static_cast<std::uint8_t>(vertex.material);
        blocks.vertices.push_back({.position = vertex.position,
                                   .normal = vertex.normal,
                                   .tiles = {tile, tile, tile},
                                   .corner = terrain::packed_corner(0, double{vertex.wet}, double{vertex.glow})});
    }
    blocks.indices.reserve(mesh.indices.size());
    for (std::size_t corner = 0; corner + 2 < mesh.indices.size(); corner += 3) {
        const bool liquid = std::ranges::all_of(
            std::span{mesh.indices}.subspan(corner, 3), [&](std::uint32_t index) {
                return mesh.vertices[index].material == globe::Material::Water;
            });
        if (!liquid) {
            blocks.indices.insert(blocks.indices.end(),
                                  mesh.indices.begin() + static_cast<std::ptrdiff_t>(corner),
                                  mesh.indices.begin() + static_cast<std::ptrdiff_t>(corner + 3));
        }
    }
    terrain::mix_corners(blocks);
    return blocks;
}

terrain::BlockMesh globe_still_water(const globe::SurfaceMesh& mesh,
                                     std::optional<double> sea_level, double thickness) {
    constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
    terrain::BlockMesh water{.origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = {}};
    std::vector<std::uint32_t> made(mesh.vertices.size(), none);
    const auto tile = static_cast<std::uint8_t>(globe::Material::Water);
    for (std::size_t corner = 0; corner + 2 < mesh.indices.size(); corner += 3) {
        const auto corners = std::span{mesh.indices}.subspan(corner, 3);
        bool liquid = true;
        double stands = 0.0;
        for (const std::uint32_t index : corners) {
            const globe::SurfaceVertex& vertex = mesh.vertices[index];
            liquid = liquid && vertex.material == globe::Material::Water;
            stands += glm::length(mesh.origin + DVec3{vertex.position}) / 3.0;
        }
        if (!liquid || (sea_level && std::abs(stands - *sea_level) <= thickness)) {
            continue;
        }
        for (const std::uint32_t index : corners) {
            if (made[index] == none) {
                made[index] = static_cast<std::uint32_t>(water.vertices.size());
                water.vertices.push_back({.position = mesh.vertices[index].position,
                                          .normal = mesh.vertices[index].normal,
                                          .tiles = {tile, tile, tile},
                                          .corner = 0});
            }
            water.indices.push_back(made[index]);
        }
    }
    return water;
}

std::uint64_t globe_key(globe::Cell chunk) {
    return (std::uint64_t{chunk.level} << 56U) | globe::index_of(chunk);
}

namespace {

const log::Category globe_log{"globe"};

// How the chunks beside one are drawn, by each: at its detail, coarser, finer, or not at all.
std::uint32_t drawn_beside(const globe::Drawn& drawn, globe::Cell chunk) {
    std::uint32_t found = 0;
    const globe::CellNeighbors near = globe::around(chunk);
    for (const globe::Cell beside : near.all()) {
        std::uint32_t how = 3;
        if (const std::optional<globe::Cell> over = drawn.over(beside)) {
            how = *over == beside ? 0 : 1;
        } else if (std::ranges::any_of(globe::children(beside),
                                       [&](globe::Cell finer) { return drawn.contains(finer); })) {
            how = 2;
        }
        found = (found << 2U) | how;
    }
    return found;
}

// What a chunk is meshed against: the band its sight was made for, 0 for none, over how the
// chunks beside it are drawn.
std::uint64_t against_key(std::uint64_t band, std::uint32_t beside) {
    return (band << 32U) | beside;
}

std::uint64_t band_of(std::uint64_t key) {
    return key >> 32U;
}

// Metres to the horizon from `height` over a sphere of `radius`.
double horizon(double radius, double height) {
    return std::sqrt((2.0 * radius * height) + (height * height));
}

} // namespace

GlobeStream::GlobeStream(const globe::World& world, jobs::JobSystem& jobs, GlobeDetail detail)
    : world_{&world}, jobs_{&jobs}, detail_{detail} {}

void GlobeStream::follow(DVec3 eye) {
    const globe::Generator& generator = world_->generator();
    const double radius = generator.planet().radius;
    const DVec3 up = glm::normalize(eye);
    const double height = std::max(0.0, glm::length(eye) - radius - globe_ground(generator, eye));
    if (around_ && glm::length(eye - *around_) <= std::max(globe_relay, height / 20.0)) {
        return;
    }
    around_ = eye;
    // Land stands over the horizon: the highest of it is seen from as far again.
    const double reach =
        std::max(detail_.least, horizon(radius, height) +
                                    horizon(radius, std::max(0.0, generator.shape().highest)));
    order_ = globe::chunks_around(generator, {.up = up, .height = height}, reach, detail_.ratio,
                                  drawn_ ? *drawn_ : globe::Drawn{});
    log::info(globe_log,
              "seeking {} chunks round ({:.9f}, {:.9f}, {:.9f}) at {:.1f} m, {:.0f} m out",
              order_.size(), up.x, up.y, up.z, height, reach);
    drawn_ = std::make_shared<const globe::Drawn>(order_);
    // A chunk near the eye sees under its ground as far as the eye does, kept as it was made
    // while the eye stays within its band.
    const double eye_radius = glm::length(eye);
    const auto sight_for = [&](globe::Cell chunk) -> std::uint64_t {
        const std::array<DVec3, 4> corners = globe::corners(chunk);
        const double across = glm::length(corners[1] - corners[0]) * radius;
        if (glm::length((globe::center(chunk) * radius) - (up * radius)) > globe_sight + across) {
            return 0;
        }
        const auto shown = shown_.find(chunk);
        return shown != shown_.end() && globe::sight_holds(band_of(shown->second), eye_radius)
                   ? band_of(shown->second)
                   : globe::sight_band(eye_radius);
    };
    wanted_.clear();
    for (const globe::Cell chunk : order_) {
        wanted_.emplace(chunk, against_key(sight_for(chunk), drawn_beside(*drawn_, chunk)));
    }
    queue_.clear();
    for (const globe::Cell chunk : order_ | std::views::reverse) {
        const std::uint64_t against = wanted_.at(chunk);
        const auto shown = shown_.find(chunk);
        const bool meshing = std::ranges::any_of(working_, [&](const Working& work) {
            return work.chunk == chunk && work.against == against;
        });
        if ((shown == shown_.end() || shown->second != against) && !meshing) {
            queue_.push_back(chunk);
        }
    }
    start_more();
    let_go();
}

void GlobeStream::changed(globe::BrickSite brick) {
    HOG_ASSERT(working_.empty(), "the world changed under chunks being meshed");
    const globe::Layers& layers = world_->generator().layers();
    // A chunk's grid reaches a margin of coarse boxes into the chunks beside it.
    const auto touches = [&](globe::Cell chunk) {
        if (chunk.face != brick.face) {
            return false;
        }
        const int detail = globe::detail_of(chunk, layers);
        const std::int64_t across = std::int64_t{globe::chunk_side} << detail;
        const std::int64_t margin = std::int64_t{10} << detail;
        const std::int64_t i0 = (std::int64_t{chunk.i} * across) - margin;
        const std::int64_t j0 = (std::int64_t{chunk.j} * across) - margin;
        const std::int64_t bi = std::int64_t{brick.i} * globe::brick_side;
        const std::int64_t bj = std::int64_t{brick.j} * globe::brick_side;
        return bi + globe::brick_side > i0 && bi < i0 + across + (2 * margin) &&
               bj + globe::brick_side > j0 && bj < j0 + across + (2 * margin);
    };
    std::set<globe::Cell> again;
    for (const auto& [chunk, beside] : wanted_) {
        if (!touches(chunk)) {
            continue;
        }
        again.insert(chunk);
        // Over a cube's edge, the chunks on the next face reach back into this one.
        const globe::CellNeighbors near = globe::around(chunk);
        for (const globe::Cell next : near.all()) {
            if (next.face != chunk.face && wanted_.contains(next)) {
                again.insert(next);
            }
        }
    }
    // Left for the next collect() to start, so the bricks of one change can all be told first.
    for (const globe::Cell chunk : again) {
        if (std::ranges::find(queue_, chunk) == queue_.end()) {
            queue_.push_back(chunk);
        }
    }
}

bool GlobeStream::in_flight() const noexcept {
    return !queue_.empty() || !working_.empty() || !changes_.empty() ||
           std::ranges::any_of(shown_,
                               [&](const auto& shown) { return !wanted_.contains(shown.first); });
}

void GlobeStream::start_more() {
    // Leaves a thread for drawing.
    const std::size_t most = std::max<std::size_t>(1, jobs_->thread_count() - 1);
    const double radius = world_->generator().planet().radius;
    while (working_.size() < most && !queue_.empty()) {
        const globe::Cell chunk = queue_.back();
        queue_.pop_back();
        const std::uint64_t against = wanted_.at(chunk);
        working_.push_back({.chunk = chunk,
                            .against = against,
                            .mesh = std::make_unique<jobs::Async<globe::SurfaceMesh>>(
                                *jobs_, [world = world_, drawn = drawn_, chunk, radius, against] {
                                    globe::SurfaceMesh mesh;
                                    mesh.origin = globe::center(chunk) * radius;
                                    globe::add_chunk(mesh, *world, chunk, *drawn,
                                                     globe::sight_of(band_of(against)));
                                    return mesh;
                                })});
    }
}

void GlobeStream::take(std::size_t working) {
    Working work = std::move(working_[working]);
    working_.erase(working_.begin() + static_cast<std::ptrdiff_t>(working));
    const globe::SurfaceMesh mesh = work.mesh->take();
    // Meshed against neighbours since drawn otherwise, it's been sought again.
    if (const auto wanted = wanted_.find(work.chunk);
        wanted == wanted_.end() || wanted->second != work.against) {
        return;
    }
    shown_.insert_or_assign(work.chunk, work.against);
    const globe::Generator& generator = world_->generator();
    if (globe::detail_of(work.chunk, generator.layers()) == 0) {
        finest_.insert(work.chunk);
    }
    changes_.shown.push_back(
        {.key = globe_key(work.chunk),
         .ground = std::make_shared<const terrain::BlockMesh>(globe_blocks(mesh)),
         .water = std::make_shared<const terrain::BlockMesh>(
             globe_still_water(mesh, generator.sea_level(), generator.layers().thickness))});
}

void GlobeStream::let_go() {
    // Chunks wanted and not yet shown, and every one over them.
    std::set<globe::Cell> pending;
    std::set<globe::Cell> under_pending;
    for (const auto& [chunk, beside] : wanted_) {
        if (shown_.contains(chunk)) {
            continue;
        }
        pending.insert(chunk);
        for (globe::Cell over = chunk; over.level > 0;) {
            over = globe::parent(over);
            under_pending.insert(over);
        }
    }
    std::erase_if(shown_, [&](const auto& shown) {
        const globe::Cell chunk = shown.first;
        if (wanted_.contains(chunk) || under_pending.contains(chunk)) {
            return false;
        }
        for (globe::Cell over = chunk;; over = globe::parent(over)) {
            if (pending.contains(over)) {
                return false;
            }
            if (over.level == 0) {
                break;
            }
        }
        changes_.gone.push_back(globe_key(chunk));
        return true;
    });
}

GlobeStream::Changes GlobeStream::collect() {
    for (std::size_t working = working_.size(); working-- > 0;) {
        if (working_[working].mesh->done()) {
            take(working);
        }
    }
    start_more();
    let_go();
    return std::exchange(changes_, {});
}

GlobeStream::Changes GlobeStream::finish() {
    start_more();
    while (!working_.empty()) {
        // The oldest, as it's furthest along.
        take(0);
        start_more();
    }
    let_go();
    return std::exchange(changes_, {});
}

} // namespace hog::client

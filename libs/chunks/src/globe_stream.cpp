#include <hog/chunks/globe_stream.hpp>
#include <hog/core/assert.hpp>
#include <hog/core/log.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace hog::chunks {

double globe_ground(const globe::World& world, DVec3 at) {
    const globe::Generator& generator = world.generator();
    return globe::top_of(world, glm::normalize(at)).value_or(generator.layers().floor) -
           generator.planet().radius;
}

terrain::BlockMesh globe_blocks(const globe::SurfaceMesh& mesh) {
    terrain::BlockMesh blocks{.origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = {}};
    blocks.vertices.reserve(mesh.vertices.size());
    for (const globe::SurfaceVertex& vertex : mesh.vertices) {
        const auto tile = static_cast<std::uint8_t>(vertex.material);
        blocks.vertices.push_back(
            {.position = vertex.position,
             .normal = vertex.normal,
             .tiles = {tile, tile, tile},
             .corner = terrain::packed_corner(0, double{vertex.wet}, double{vertex.glow},
                                              vertex.standing)});
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

globe::Cell globe_cell(std::uint64_t key) {
    return globe::cell_at_index(static_cast<int>(key >> 56U),
                                key & ((std::uint64_t{1} << 56U) - 1));
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

void GlobeStream::follow(DVec3 eye, DVec3 forward) {
    const globe::Generator& generator = world_->generator();
    const double radius = generator.planet().radius;
    const DVec3 up = glm::normalize(eye);
    const double height = std::max(0.0, glm::length(eye) - radius - globe_ground(*world_, eye));
    const bool turned = forward != looking_ && glm::dot(forward, looking_) < globe_turn_cos;
    if (around_ && !turned && glm::length(eye - *around_) <= std::max(globe_relay, height / 20.0)) {
        return;
    }
    around_ = eye;
    looking_ = forward;
    // Land stands over the horizon: the highest of it is seen from as far again.
    const double reach =
        std::max(detail_.least, horizon(radius, height) +
                                    horizon(radius, std::max(0.0, generator.shape().highest)));
    order_ = globe::chunks_around(generator, {.up = up, .height = height}, reach, detail_.ratio,
                                  drawn_ ? *drawn_ : globe::Drawn{}, detail_.slope, detail_.least);
    if (glm::dot(forward, forward) > 0.0) {
        // What's in front of the eye first, each half still nearest first.
        std::ranges::stable_partition(order_, [&](globe::Cell chunk) {
            return glm::dot(glm::normalize((globe::center(chunk) * radius) - eye), forward) >
                   globe_view_cos;
        });
    }
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
    while (!held_ && working_.size() < most && !queue_.empty()) {
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

void GlobeStream::forget() {
    HOG_ASSERT(working_.empty(), "chunks forgotten while being meshed");
    shown_.clear();
    wanted_.clear();
    order_.clear();
    queue_.clear();
    drawn_.reset();
    around_.reset();
    changes_ = {};
}

void GlobeStream::settle() {
    while (!working_.empty()) {
        take(0);
    }
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

} // namespace hog::chunks

#include <hog/client/globe_view.hpp>
#include <hog/core/assert.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>

namespace hog::client {

Result<globe::Planet> globe_planet(std::optional<std::string_view> named, std::uint64_t seed,
                                   double radius) {
    globe::Planet planet;
    if (named) {
        if (const auto preset = globe::preset(*named)) {
            planet = *preset;
        } else {
            return globe::load_planet(std::string{*named});
        }
    }
    planet.seed = seed;
    planet.radius = radius;
    if (auto valid = globe::validate(planet); !valid) {
        return std::unexpected(std::move(valid).error());
    }
    return planet;
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

terrain::BlockMesh globe_blocks(const globe::SurfaceMesh& mesh) {
    terrain::BlockMesh blocks{.origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = {}};
    blocks.vertices.reserve(mesh.vertices.size());
    for (const globe::SurfaceVertex& vertex : mesh.vertices) {
        blocks.vertices.push_back({.position = vertex.position,
                                   .normal = vertex.normal,
                                   .colour = surface_colour(vertex.material)});
    }
    blocks.indices = mesh.indices;
    return blocks;
}

std::uint64_t globe_key(globe::Cell chunk) {
    return (std::uint64_t{chunk.level} << 56U) | globe::index_of(chunk);
}

namespace {

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
    const double reach = std::max(detail_.least, horizon(radius, height));
    order_ = globe::chunks_around(generator, {.up = up, .height = height}, reach, detail_.ratio);
    drawn_ = std::make_shared<const globe::Drawn>(order_);
    wanted_.clear();
    for (const globe::Cell chunk : order_) {
        wanted_.emplace(chunk, drawn_beside(*drawn_, chunk));
    }
    queue_.clear();
    for (const globe::Cell chunk : order_ | std::views::reverse) {
        const std::uint32_t beside = wanted_.at(chunk);
        const auto shown = shown_.find(chunk);
        const bool meshing = std::ranges::any_of(working_, [&](const Working& work) {
            return work.chunk == chunk && work.beside == beside;
        });
        if ((shown == shown_.end() || shown->second != beside) && !meshing) {
            queue_.push_back(chunk);
        }
    }
    start_more();
    let_go();
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
        working_.push_back({.chunk = chunk,
                            .beside = wanted_.at(chunk),
                            .mesh = std::make_unique<jobs::Async<globe::SurfaceMesh>>(
                                *jobs_, [world = world_, drawn = drawn_, chunk, radius] {
                                    globe::SurfaceMesh mesh;
                                    mesh.origin = globe::center(chunk) * radius;
                                    globe::add_chunk(mesh, *world, chunk, *drawn);
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
        wanted == wanted_.end() || wanted->second != work.beside) {
        return;
    }
    shown_.insert_or_assign(work.chunk, work.beside);
    changes_.shown.emplace_back(globe_key(work.chunk),
                                std::make_shared<const terrain::BlockMesh>(globe_blocks(mesh)));
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

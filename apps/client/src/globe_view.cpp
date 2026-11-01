#include <hog/client/globe_view.hpp>
#include <hog/core/assert.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
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

terrain::BlockMesh globe_blocks(std::span<const globe::SurfaceMesh* const> meshes, DVec3 origin) {
    terrain::BlockMesh blocks{.origin = WorldPosition{origin}, .vertices = {}, .indices = {}};
    std::size_t vertices = 0;
    std::size_t indices = 0;
    for (const globe::SurfaceMesh* mesh : meshes) {
        vertices += mesh->vertices.size();
        indices += mesh->indices.size();
    }
    blocks.vertices.reserve(vertices);
    blocks.indices.reserve(indices);
    for (const globe::SurfaceMesh* mesh : meshes) {
        const auto first = static_cast<std::uint32_t>(blocks.vertices.size());
        const Vec3 offset{mesh->origin - origin};
        for (const globe::SurfaceVertex& vertex : mesh->vertices) {
            blocks.vertices.push_back({.position = offset + vertex.position,
                                       .normal = vertex.normal,
                                       .colour = surface_colour(vertex.material)});
        }
        std::ranges::transform(mesh->indices, std::back_inserter(blocks.indices),
                               [&](std::uint32_t index) { return first + index; });
    }
    return blocks;
}

GlobeStream::GlobeStream(const globe::World& world, jobs::JobSystem& jobs, double reach)
    : world_{&world}, jobs_{&jobs}, reach_{reach} {}

void GlobeStream::follow(DVec3 eye) {
    const double radius = world_->generator().planet().radius;
    if (around_ && glm::length(eye - *around_) * radius <= globe_relay) {
        return;
    }
    around_ = eye;
    // Let go only past the relay, so chunks at the edge don't come and go as the eye wanders.
    fresh_ += std::erase_if(meshed_, [&](const auto& meshed) {
        return glm::length(globe::center(meshed.first) - eye) * radius > reach_ + globe_relay;
    });
    std::set<globe::Cell> started;
    for (const auto& [chunk, async] : working_) {
        started.insert(chunk);
    }
    wanted_.clear();
    for (const globe::Cell chunk : globe::chunks_near(*world_, eye, reach_)) {
        if (!meshed_.contains(chunk) && !started.contains(chunk)) {
            wanted_.push_back(chunk);
        }
    }
    std::ranges::reverse(wanted_);
    start_more();
}

bool GlobeStream::in_flight() const noexcept {
    return !wanted_.empty() || !working_.empty() || fresh_ > 0;
}

void GlobeStream::start_more() {
    // Leaves a thread for drawing.
    const std::size_t most = std::max<std::size_t>(1, jobs_->thread_count() - 1);
    const double radius = world_->generator().planet().radius;
    while (working_.size() < most && !wanted_.empty()) {
        const globe::Cell chunk = wanted_.back();
        wanted_.pop_back();
        working_.emplace_back(chunk, std::make_unique<jobs::Async<globe::SurfaceMesh>>(
                                         *jobs_, [world = world_, chunk, radius] {
                                             globe::SurfaceMesh mesh;
                                             mesh.origin = globe::center(chunk) * radius;
                                             globe::add_chunk(mesh, *world, chunk);
                                             return mesh;
                                         }));
    }
}

void GlobeStream::take(std::size_t working) {
    auto& [chunk, async] = working_[working];
    meshed_.insert_or_assign(chunk, async->take());
    working_.erase(working_.begin() + static_cast<std::ptrdiff_t>(working));
    ++fresh_;
}

std::optional<terrain::BlockMesh> GlobeStream::collect() {
    for (std::size_t working = working_.size(); working-- > 0;) {
        if (working_[working].second->done()) {
            take(working);
        }
    }
    start_more();
    // In batches of a quarter of what's shown, so it's remade a few times over as it fills.
    const bool all = wanted_.empty() && working_.empty();
    if (fresh_ == 0 || (!all && fresh_ * 4 < std::max<std::size_t>(meshed_.size(), 4))) {
        return std::nullopt;
    }
    fresh_ = 0;
    return mesh();
}

std::optional<terrain::BlockMesh> GlobeStream::finish() {
    while (!working_.empty()) {
        take(working_.size() - 1);
        start_more();
    }
    if (fresh_ == 0) {
        return std::nullopt;
    }
    fresh_ = 0;
    return mesh();
}

terrain::BlockMesh GlobeStream::mesh() const {
    HOG_ASSERT(around_.has_value(), "surfaces are meshed round where the eye was");
    std::vector<const globe::SurfaceMesh*> meshes;
    meshes.reserve(meshed_.size());
    for (const auto& [chunk, meshed] : meshed_) {
        meshes.push_back(&meshed);
    }
    return globe_blocks(meshes, *around_ * world_->generator().planet().radius);
}

} // namespace hog::client

#include <hog/client/page_meshes.hpp>
#include <hog/core/assert.hpp>
#include <hog/globe/material.hpp>

#include <algorithm>
#include <utility>

namespace hog::client {
namespace {

// Past the tiles' keys, whose level takes the top bits but never this high, and the cutaway's.
constexpr std::uint64_t page_keys = std::uint64_t{0b110} << 61U;

const std::shared_ptr<const terrain::BlockMesh>& no_water() {
    static const auto none = std::make_shared<const terrain::BlockMesh>();
    return none;
}

// Of a page drawn, which holds one.
const grid::Held<grid::Shape>& shape_of(const grid::Page& page) {
    return page.shape.value();
}

// A page and its mesh as a seam reads them, kept alive for the job.
struct Part {
    std::shared_ptr<const grid::Page> page;
    std::uint8_t levels = 0;
    std::vector<grid::Key> under;
    std::shared_ptr<const ShapeMesh> mesh;
};

} // namespace

std::uint64_t page_key(grid::Key root, bool seam) noexcept {
    HOG_ASSERT(root.depth < 20, "a page key in the bits kept for it");
    return page_keys | (grid::code(root) << 1U) | (seam ? 1U : 0U);
}

terrain::BlockMesh block_of(const ShapeMesh& mesh) {
    terrain::BlockMesh block{
        .origin = WorldPosition{mesh.origin}, .vertices = {}, .indices = mesh.indices};
    block.vertices.reserve(mesh.vertices.size());
    for (const ShapeVertex& vertex : mesh.vertices) {
        const std::uint16_t shown =
            vertex.share >= 0.5F ? vertex.materials[0] : vertex.materials[1];
        // The generator's materials come first in the palette; what's unknown shows as soil.
        const auto tile = shown == 0 || shown > 0xFF
                              ? static_cast<std::uint8_t>(globe::Material::Soil)
                              : static_cast<std::uint8_t>(shown);
        const Vec3 normal = glm::dot(vertex.normal, vertex.normal) > 0.0F
                                ? vertex.normal
                                : Vec3{glm::normalize(mesh.origin + DVec3{vertex.position})};
        block.vertices.push_back({.position = vertex.position,
                                  .normal = normal,
                                  .tiles = {tile, tile, tile},
                                  .corner = terrain::packed_corner(0, 0.0)});
    }
    terrain::mix_corners(block);
    return block;
}

bool PageMeshes::Look::same(const Look& other) const {
    const auto& shape = shape_of(*page);
    const auto& other_shape = shape_of(*other.page);
    return levels == other.levels && under == other.under && shape.version == other_shape.version &&
           shape.tree.levels() == other_shape.tree.levels() &&
           page->generator == other.page->generator;
}

PageMeshes::PageMeshes(const grid::Grid& grid, jobs::JobSystem& jobs)
    : grid_{grid}, jobs_{&jobs}, most_working_{std::max<std::size_t>(2, jobs.thread_count())} {}

PageMeshes::~PageMeshes() {
    gather(true);
}

void PageMeshes::want(const HeldPages& held, const std::vector<DrawnPage>& drawn, DVec3 eye) {
    HOG_ASSERT(settled(), "a drawing wanted while the last is still meshing");
    wanted_.clear();
    touching_.clear();
    std::vector<std::pair<double, grid::Key>> to_mesh;
    for (const DrawnPage& page : drawn) {
        Look look{.page = held.shared(page.page), .levels = page.levels, .under = page.under};
        HOG_ASSERT(look.page != nullptr && look.page->shape, "a page drawn held with its shape");
        Drawing& drawing = wanted_[page.page];
        if (const auto found = drawn_.find(page.page);
            found != drawn_.end() && found->second.look.same(look)) {
            drawing.own = found->second.own;
        } else {
            to_mesh.emplace_back(grid_.distance(page.page, eye), page.page);
        }
        drawing.look = std::move(look);
    }
    std::ranges::sort(to_mesh);
    for (const auto& [metres, root] : to_mesh) {
        queued_.push_back({.root = root, .seam = false});
    }
    stage_ = Stage::Pages;
    advance();
}

std::unique_ptr<jobs::Async<PageMeshes::Meshed>> PageMeshes::mesh_page(grid::Key root) const {
    const Look& look = wanted_.at(root).look;
    return std::make_unique<jobs::Async<Meshed>>(
        *jobs_, [grid = grid_, root, page = look.page, levels = look.levels, under = look.under] {
            ShapeMesh mesh = mesh_shape(grid, ShapeDrawn{.page = root,
                                                         .tree = &shape_of(*page).tree,
                                                         .levels = levels,
                                                         .under = under,
                                                         .mesh = nullptr});
            auto block = std::make_shared<const terrain::BlockMesh>(block_of(mesh));
            mesh.vertices = {};
            mesh.indices = {};
            return Meshed{.shape = std::make_shared<const ShapeMesh>(std::move(mesh)),
                          .block = std::move(block)};
        });
}

std::unique_ptr<jobs::Async<PageMeshes::Meshed>> PageMeshes::mesh_seam(grid::Key root) const {
    std::vector<Part> parts;
    for (const auto& [read, mesh] : wanted_.at(root).reads) {
        const Drawing& drawing = wanted_.at(read);
        parts.push_back({.page = drawing.look.page,
                         .levels = drawing.look.levels,
                         .under = drawing.look.under,
                         .mesh = drawing.own.shape});
    }
    return std::make_unique<jobs::Async<Meshed>>(
        *jobs_, [grid = grid_, root, parts = std::move(parts)] {
            ShapesDrawn drawn{grid};
            for (const Part& part : parts) {
                drawn.add({.page = part.page->root,
                           .tree = &shape_of(*part.page).tree,
                           .levels = part.levels,
                           .under = part.under,
                           .mesh = part.mesh.get()});
            }
            return Meshed{.shape = nullptr,
                          .block = std::make_shared<const terrain::BlockMesh>(
                              block_of(client::mesh_seam(drawn, root)))};
        });
}

void PageMeshes::start() {
    while (!queued_.empty() && working_.size() < most_working_) {
        const Queued next = queued_.front();
        queued_.pop_front();
        working_.push_back({.root = next.root,
                            .seam = next.seam,
                            .job = next.seam ? mesh_seam(next.root) : mesh_page(next.root)});
    }
}

void PageMeshes::gather(bool wait) {
    std::vector<Work> still;
    for (Work& work : working_) {
        if (!wait && !work.job->done()) {
            still.push_back(std::move(work));
            continue;
        }
        Meshed meshed = work.job->take();
        const auto found = wanted_.find(work.root);
        if (found == wanted_.end()) {
            continue;
        }
        if (work.seam) {
            found->second.seam = std::move(meshed.block);
        } else {
            found->second.own = std::move(meshed);
        }
    }
    working_ = std::move(still);
}

void PageMeshes::advance() {
    if (stage_ == Stage::Pages && in_flight() == 0) {
        // Seams read every page meshed, so they're met once all are.
        ShapesDrawn drawn{grid_};
        for (const auto& [root, drawing] : wanted_) {
            drawn.add({.page = root,
                       .tree = &shape_of(*drawing.look.page).tree,
                       .levels = drawing.look.levels,
                       .under = drawing.look.under,
                       .mesh = drawing.own.shape.get()});
        }
        for (auto& [root, drawing] : wanted_) {
            drawing.reads = {{root, drawing.own.shape.get()}};
            for (const grid::Key next : drawn.touching(root)) {
                drawing.reads.emplace_back(next, wanted_.at(next).own.shape.get());
            }
            std::ranges::sort(drawing.reads);
            if (const auto found = drawn_.find(root);
                found != drawn_.end() && found->second.reads == drawing.reads) {
                drawing.seam = found->second.seam;
            } else {
                queued_.push_back({.root = root, .seam = true});
            }
        }
        stage_ = Stage::Seams;
    }
    if (stage_ == Stage::Seams && in_flight() == 0) {
        show();
        stage_ = Stage::Settled;
    }
    start();
}

void PageMeshes::show() {
    const auto place = [&](grid::Key root, bool seam,
                           const std::shared_ptr<const terrain::BlockMesh>& now,
                           const std::shared_ptr<const terrain::BlockMesh>& was) {
        if (now == was) {
            return;
        }
        if (now && !now->indices.empty()) {
            changes_.shown.push_back(
                {.key = page_key(root, seam), .ground = now, .water = no_water()});
        } else if (was && !was->indices.empty()) {
            changes_.gone.push_back(page_key(root, seam));
        }
    };
    for (const auto& [root, drawing] : wanted_) {
        const auto found = drawn_.find(root);
        const Drawing* was = found == drawn_.end() ? nullptr : &found->second;
        place(root, false, drawing.own.block, was != nullptr ? was->own.block : nullptr);
        place(root, true, drawing.seam, was != nullptr ? was->seam : nullptr);
    }
    for (const auto& [root, drawing] : drawn_) {
        if (!wanted_.contains(root)) {
            place(root, false, nullptr, drawing.own.block);
            place(root, true, nullptr, drawing.seam);
        }
    }
    drawn_ = std::move(wanted_);
    wanted_.clear();
}

GroundChanges PageMeshes::collect() {
    gather(false);
    advance();
    return std::exchange(changes_, {});
}

void PageMeshes::finish() {
    while (!settled()) {
        gather(true);
        advance();
    }
}

} // namespace hog::client

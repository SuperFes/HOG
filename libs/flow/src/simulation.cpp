#include <hog/flow/simulation.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <utility>

namespace hog::flow {

using globe::BrickSite;

Simulation::Simulation(globe::World& world) : world_{&world}, region_{world} {}

void Simulation::take(BrickSite site) {
    const globe::VoxelStore& store = world_->store();
    if (!store.holds_site(site)) {
        return;
    }
    region_.load(site);
    still_[globe::brick_key(site)] = 0;
}

void Simulation::touched(BrickSite site) {
    take(site);
    constexpr std::array<std::array<int, 3>, 6> about{
        {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
    for (const auto& [di, dj, dl] : about) {
        const std::int64_t i = std::int64_t{site.i} + di;
        const std::int64_t j = std::int64_t{site.j} + dj;
        const std::int64_t l = std::int64_t{site.layer} + dl;
        if (i >= 0 && j >= 0 && l >= 0) {
            take({.face = site.face,
                  .i = static_cast<std::uint32_t>(i),
                  .j = static_cast<std::uint32_t>(j),
                  .layer = static_cast<std::uint32_t>(l)});
        }
    }
}

namespace {

std::uint64_t column_key(std::uint8_t face, std::uint32_t i, std::uint32_t j) noexcept {
    return (std::uint64_t{face} << 42U) | (std::uint64_t{i} << 21U) | j;
}

} // namespace

Simulation::Moisture Simulation::moisture(std::uint8_t face, std::uint32_t i,
                                          std::uint32_t j) const {
    const auto found = moisture_.find(column_key(face, i, j));
    return found == moisture_.end() ? Moisture{} : found->second;
}

std::vector<BrickSite> Simulation::wet(std::span<const Wetted> columns, double seconds) {
    const double thickness = world_->store().layers().thickness;
    // Bricks the flow doesn't hold, as the world has them (generated, where unedited), and which
    // of them were changed.
    struct Copy {
        globe::BrickVoxels voxels;
        bool changed = false;
    };
    std::unordered_map<std::uint64_t, Copy> copies;
    const auto copy = [&](BrickSite brick) -> Copy& {
        const auto [found, fresh] = copies.try_emplace(globe::brick_key(brick));
        if (fresh) {
            found->second.voxels = world_->brick(brick);
        }
        return found->second;
    };
    std::vector<BrickSite> woken;
    ++wettings_;
    for (const Wetted& wetted : columns) {
        const globe::Site column = wetted.column;
        const auto read = [&](std::uint32_t layer) -> std::optional<globe::Voxel> {
            if (layer >= static_cast<std::uint32_t>(world_->store().layers().count)) {
                return std::nullopt;
            }
            const globe::Site site{.face = column.face, .i = column.i, .j = column.j, .layer = layer};
            const BrickSite brick = globe::brick_of(site);
            if (region_.holds(brick)) {
                return region_.at(site);
            }
            return copy(brick).voxels[static_cast<std::size_t>(globe::index_in_brick(site))];
        };
        for (const Soaking& change : Wetting::column(read, column, column.layer, thickness,
                                                      wetted.sky, seconds, wettings_)) {
            const BrickSite brick = globe::brick_of(change.site);
            if (region_.holds(brick)) {
                region_.pour(change.site, change.amount);
                continue;
            }
            globe::Voxel& voxel =
                copy(brick).voxels[static_cast<std::size_t>(globe::index_in_brick(change.site))];
            voxel.liquid = change.liquid;
            voxel.liquid_amount = change.amount;
            copy(brick).changed = true;
            // A film over the ground has somewhere to go.
            if (change.amount > 0 && voxel.solid == globe::Material::Air) {
                woken.push_back(brick);
            }
        }
    }
    std::vector<BrickSite> written;
    for (auto& [key, held] : copies) {
        if (held.changed) {
            written.push_back(globe::brick_at_key(key));
            world_->write(written.back(), held.voxels);
        }
    }
    for (const BrickSite brick : woken) {
        touched(brick);
    }
    return written;
}

Simulation::Progress Simulation::step(double seconds) {
    Progress progress;
    if (region_.size() == 0) {
        return progress;
    }
    const Stepped stepped = region_.step(seconds);
    progress.changed = stepped.changed;
    progress.eroded = stepped.eroded;
    progress.slumped = stepped.slumped;
    progress.flying = stepped.flying;
    progress.scattered = stepped.scattered;
    const double thickness = world_->store().layers().thickness;
    for (const Stepped::Wet& wet : stepped.wet) {
        Moisture& moisture = moisture_[column_key(wet.face, wet.i, wet.j)];
        moisture.stood +=
            static_cast<double>(wet.standing) / globe::Voxel::full * thickness * seconds;
        const Region::Flux flux = region_.flux(wet.face, wet.i, wet.j);
        moisture.ran += static_cast<double>(std::abs(flux.along_i) + std::abs(flux.along_j)) /
                        globe::Voxel::full;
    }
    for (std::size_t k = 0; k < std::min(stepped.wanted.size(), most_taken); ++k) {
        take(stepped.wanted[k]);
    }
    progress.written = region_.flush();
    for (const BrickSite brick : progress.written) {
        still_[globe::brick_key(brick)] = 0;
    }
    std::vector<BrickSite> let_go;
    for (auto& [key, steps] : still_) {
        // Grain in flight may land anywhere held, so nothing is let go while any is.
        if (stepped.flying > 0) {
            steps = 0;
        }
        if (++steps > rest) {
            let_go.push_back(globe::brick_at_key(key));
        }
    }
    for (const BrickSite brick : let_go) {
        region_.unload(brick);
        still_.erase(globe::brick_key(brick));
    }
    progress.held = region_.size();
    return progress;
}

} // namespace hog::flow

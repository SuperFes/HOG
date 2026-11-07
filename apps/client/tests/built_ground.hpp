#pragma once

#include <hog/client/column_meshes.hpp>
#include <hog/client/scene.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/column_cache.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::client::testing {

// The ground round a place as a client is sent it: each tile column in reach made as the world's
// built ahead, held at the level for its distance, and meshed for a scene to draw.
class BuiltGround {
public:
    // Metres round the place the ground's sent, as a server sends it...
    static constexpr double sent = 8'000.0;
    // ...but no more tile columns than this out, so fine voxels don't make millions of them.
    static constexpr double most_out = 40.0;

    // The generator and jobs must outlive it.
    BuiltGround(const globe::Generator& generator, jobs::JobSystem& jobs)
        : generator_{&generator}, jobs_{&jobs},
          world_{generator.planet(), generator.layers(), volume_}, meshes_{world_, jobs} {
        world_.read_coarse(&held_);
    }
    BuiltGround(const BuiltGround&) = delete;
    BuiltGround& operator=(const BuiltGround&) = delete;
    BuiltGround(BuiltGround&&) = delete;
    BuiltGround& operator=(BuiltGround&&) = delete;
    ~BuiltGround() = default;

    [[nodiscard]] const globe::World& world() const noexcept { return world_; }
    [[nodiscard]] double reach() const noexcept {
        return std::min(sent, most_out * globe::Volume::tile_side * world_.layers().thickness);
    }

    // Holds the ground round the point on the planet over `up`, lets go of what's out of reach,
    // and has the scene draw it; fails on a tile column made that can't be held.
    Result<> show_round(Scene& scene, DVec3 up) {
        const globe::Layers& layers = world_.layers();
        const DVec3 at = glm::normalize(up) * world_.planet().radius;
        constexpr auto side = static_cast<std::uint32_t>(globe::Volume::tile_side);
        constexpr int coarser = std::countr_zero(side);
        struct Wanted {
            globe::Site corner;
            std::uint8_t level = 0;
        };
        std::vector<Wanted> wanted;
        for (const globe::Site corner : globe::tile_columns_near(layers, at, reach())) {
            const globe::Cell cell{.level = static_cast<std::uint8_t>(layers.level - coarser),
                                   .face = corner.face,
                                   .i = corner.i / side,
                                   .j = corner.j / side};
            const std::uint8_t level =
                globe::level_at(glm::length((globe::center(cell) * glm::length(at)) - at));
            if (held_at(corner) != level) {
                wanted.push_back({.corner = corner, .level = level});
            }
        }
        constexpr std::size_t batch = 16;
        std::vector<std::unique_ptr<jobs::Async<std::vector<globe::SavedColumn>>>> making;
        for (std::size_t first = 0; first < wanted.size(); first += batch) {
            const std::vector<Wanted> some{wanted.begin() + static_cast<std::ptrdiff_t>(first),
                                           wanted.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                                first + batch, wanted.size()))};
            making.push_back(std::make_unique<jobs::Async<std::vector<globe::SavedColumn>>>(
                *jobs_, [generator = generator_, some] {
                    std::vector<globe::SavedColumn> made;
                    for (const Wanted& each : some) {
                        const std::uint32_t i = each.corner.i / side;
                        const std::uint32_t j = each.corner.j / side;
                        const globe::Volume tiles =
                            globe::generate_tiles(*generator, each.corner.face, i, j);
                        const std::optional<globe::TileColumn> column = tiles.column(each.corner);
                        made.push_back(column ? globe::saved_levels(*column, each.corner.face, i, j)
                                                    .at(each.level)
                                              : globe::SavedColumn{.face = each.corner.face,
                                                                   .i = i,
                                                                   .j = j,
                                                                   .level = each.level,
                                                                   .bytes = {}});
                    }
                    return made;
                }));
        }
        for (auto& job : making) {
            for (const globe::SavedColumn& column : job->take()) {
                if (auto taken =
                        held_.take(column.face, column.i, column.j, column.level, column.bytes);
                    !taken) {
                    return taken;
                }
            }
        }
        held_.keep_near(layers, at, reach());
        meshes_.changed(held_.take_changed());
        meshes_.finish();
        scene.show_ground(meshes_.collect());
        scene.set_ground_state(meshes_.covered(at, reach()), false);
        return {};
    }

private:
    // The level the tile column at `corner` is held at; none where it isn't.
    [[nodiscard]] std::optional<std::uint8_t> held_at(globe::Site corner) const {
        if (volume_.floor(corner)) {
            return 0;
        }
        if (const globe::ColumnCache::Coarse* coarse = held_.coarse(corner)) {
            return coarse->level;
        }
        return std::nullopt;
    }

    const globe::Generator* generator_;
    jobs::JobSystem* jobs_;
    globe::Volume volume_;
    globe::World world_;
    globe::ColumnCache held_{volume_};
    ColumnMeshes meshes_;
};

} // namespace hog::client::testing

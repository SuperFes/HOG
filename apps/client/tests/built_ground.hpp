#pragma once

#include <hog/client/scene.hpp>
#include <hog/client/tile_meshes.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/held_tiles.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/server/tile_sending.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <utility>

namespace hog::client::testing {

// The ground round a place as a client is sent it: the land cut as the server cuts it for a player
// there, held as tiles and meshed for a scene to draw.
class BuiltGround {
public:
    // Metres round the place the ground's drawn whole, as a client is sent it.
    static constexpr double sent = 8'000.0;

    // The generator and jobs must outlive it.
    BuiltGround(const globe::Generator& generator, jobs::JobSystem& jobs)
        : jobs_{&jobs}, land_{generator}, world_{generator.planet(), generator.layers(), volume_},
          meshes_{generator.planet(), generator.layers(), jobs} {
        world_.read_tiles(&tiles_);
    }
    BuiltGround(const BuiltGround&) = delete;
    BuiltGround& operator=(const BuiltGround&) = delete;
    BuiltGround(BuiltGround&&) = delete;
    BuiltGround& operator=(BuiltGround&&) = delete;
    ~BuiltGround() = default;

    [[nodiscard]] const globe::World& world() const noexcept { return world_; }

    // Holds the land cut round the point on the planet over `up`, and has the scene draw it;
    // fails on a tile made that can't be held.
    Result<> show_round(Scene& scene, DVec3 up) {
        const DVec3 at = glm::normalize(up) * world_.planet().radius;
        const globe::EachOf each = [this](std::size_t count,
                                          const std::function<void(std::size_t)>& body) {
            jobs_->parallel_for(static_cast<std::uint32_t>(count), 1,
                                [&](jobs::Range range, std::uint32_t /*thread*/) {
                                    for (std::uint32_t n = range.begin; n < range.end; ++n) {
                                        body(n);
                                    }
                                });
        };
        const server::TileSending sending{
            server::TileSending::reach_for(world_.layers().thickness)};
        server::TileSending::Cut cut = sending.cut(at, held_, land_, each);
        for (const protocol::SentTile& coded : cut.sent) {
            auto tile = sent_tree(coded, world_.layers());
            if (!tile) {
                return std::unexpected(std::move(tile).error());
            }
            tiles_.put(tile->key, tile->tree);
            meshes_.take(*tile);
        }
        held_ = {cut.keys.begin(), cut.keys.end()};
        GroundChanges changes = meshes_.collect();
        meshes_.finish();
        GroundChanges more = meshes_.collect();
        changes.shown.insert(changes.shown.end(), more.shown.begin(), more.shown.end());
        changes.gone.insert(changes.gone.end(), more.gone.begin(), more.gone.end());
        scene.show_ground(std::move(changes));
        scene.set_ground_state(meshes_.covered(at, sent), false);
        return {};
    }

private:
    jobs::JobSystem* jobs_;
    globe::Land land_;
    globe::Volume volume_; // none: the ground is the tiles held
    globe::HeldTiles tiles_;
    globe::World world_;
    TileMeshes meshes_;
    std::set<globe::TileKey> held_;
};

} // namespace hog::client::testing

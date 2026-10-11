#include <hog/server/spatial_tracking.hpp>

namespace hog::server {

// flecs::world is a handle whose methods are all const, though they change the world.
// NOLINTNEXTLINE(misc-const-correctness)
void track_positions(flecs::world& world, SpatialIndex& index) {
    world.observer<const Position>()
        .event(flecs::OnSet)
        .each([&index](flecs::entity entity, const Position& position) {
            index.update(entity.id(), position.at);
        });
    world.observer<const Position>()
        .event(flecs::OnRemove)
        .each([&index](flecs::entity entity, const Position&) { index.remove(entity.id()); });

    // Change detection works per table, so a table with one moved entity is reindexed whole;
    // update() is cheap for entities that stayed in their cell.
    const auto moved = world.query_builder<const Position>().detect_changes().build();
    world.system().kind(flecs::PostUpdate).run([moved, &index](flecs::iter&) {
        moved.run([&index](flecs::iter& it) {
            while (it.next()) {
                if (!it.changed()) {
                    continue;
                }
                const auto positions = it.field<const Position>(0);
                for (const auto i : it) {
                    index.update(it.entity(i).id(), positions[i].at);
                }
            }
        });
    });
}

} // namespace hog::server

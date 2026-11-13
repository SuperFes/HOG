// What the network carries for some everyday happenings, in encoded bytes, from the real
// messages and the real world. Run: hog_server_tests "[.network]"
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <cstdio>
#include <print>
#include <vector>

using namespace hog;

namespace {

std::size_t size_of(const protocol::Message& message) {
    return protocol::encode(message).size();
}

} // namespace

TEST_CASE("what the network carries for everyday happenings", "[.network]") {
    // Messages as sent. Positions are planet-scale millimetres, as in the real thing.
    const protocol::Millimetres far{115'000'000, 3'000'000, 2'000'000};
    const protocol::Moved moved{.sequence = 1'000'000, .corrections = 3, .at = far};
    std::println("Moved (client to server, one move):            {} bytes", size_of(moved));
    protocol::Snapshot one{
        .tick = 100'000, .baseline = 99'999, .entered = {}, .changed = {}, .left = {}};
    one.changed.push_back({.id = protocol::NetId{7}, .moved_by = protocol::Millimetres{400, 0, 0}});
    std::println("Snapshot, one other player moving:            {} bytes", size_of(one));
    protocol::Snapshot ten = one;
    for (std::uint32_t id = 8; id < 17; ++id) {
        ten.changed.push_back(
            {.id = protocol::NetId{id}, .moved_by = protocol::Millimetres{400, 100, 0}});
    }
    std::println("Snapshot, ten others moving:                   {} bytes", size_of(ten));
    std::println("Dug (client to server):                        {} bytes",
                 size_of(protocol::Dug{.centre = far, .radius = 750}));
    for (const int level : {3, 4, 5}) {
        const protocol::Weather weather{
            .level = static_cast<std::uint8_t>(level),
            .fade = std::chrono::milliseconds{2000},
            .cells = std::vector<protocol::CellWeather>(
                protocol::weather_cells(static_cast<std::uint8_t>(level)))};
        std::println("Weather at level {} ({} cells):                  {} bytes", level,
                     weather.cells.size(), size_of(weather));
    }
    protocol::Grains grains{.tick = 100'000, .flying = {}};
    grains.flying.assign(1, {});
    std::println("Grains, one lump in flight:                    {} bytes", size_of(grains));
    grains.flying.assign(50, {});
    std::println("Grains, fifty lumps in flight:                 {} bytes", size_of(grains));

    // A hole dug in real generated ground, a ball 1.5 m across at a time, going down: the tile
    // columns changed, as the volume codes them.
    const globe::Generator& generator = hog::testing::small_world();
    globe::MadeWorld made{generator};
    globe::World& world = made.world();
    const double sea = generator.sea_level().value_or(generator.planet().body.radius);
    DVec3 up{};
    for (std::uint64_t k = 0;; ++k) {
        up = globe::center(globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304));
        const double ground = globe::ground_below(generator, up, 0.5).value_or(0.0) - sea;
        if (ground > 20.0 && ground < 300.0) {
            break;
        }
    }
    made.cover(up, 1);
    const double ground = globe::ground_below(generator, up, 0.5).value_or(0.0);
    std::size_t total = 0;
    std::println("\nDigging a hole, a ball 1.5 m across each time, going down 0.5 m a dig:");
    for (int dig = 0; dig < 8; ++dig) {
        const std::uint64_t before = world.revision();
        globe::dig(world, up * (ground - (0.5 * dig)), 0.75);
        const std::size_t bricks = world.changed_since(before).size();
        std::size_t sent = 0;
        for (const globe::Site corner : made.volume().unsaved()) {
            sent += made.volume().encode_column(corner).size();
            made.volume().saved(corner);
        }
        total += sent;
        std::println("  dig {}: {} bricks changed, {} bytes of tile columns", dig + 1, bricks,
                     sent);
    }
    std::println("  eight digs: {} bytes sent in all", total);
    CHECK(total > 0);
}

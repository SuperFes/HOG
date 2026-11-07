// What the network carries for some everyday happenings, in encoded bytes, from the real
// messages and the real world. Run: hog_server_tests "[.network]"
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>
#include <hog/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <cstdio>
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
    std::printf("Moved (client to server, one move):            %zu bytes\n", size_of(moved));
    protocol::Snapshot one{.tick = 100'000, .baseline = 99'999, .entered = {}, .changed = {}, .left = {}};
    one.changed.push_back({.id = protocol::NetId{7}, .moved_by = protocol::Millimetres{400, 0, 0}});
    std::printf("Snapshot, one other player moving:            %zu bytes\n", size_of(one));
    protocol::Snapshot ten = one;
    for (std::uint32_t id = 8; id < 17; ++id) {
        ten.changed.push_back({.id = protocol::NetId{id}, .moved_by = protocol::Millimetres{400, 100, 0}});
    }
    std::printf("Snapshot, ten others moving:                   %zu bytes\n", size_of(ten));
    std::printf("Dug (client to server):                        %zu bytes\n",
                size_of(protocol::Dug{.centre = far, .radius = 750}));
    for (const int level : {3, 4, 5}) {
        const protocol::Weather weather{.level = static_cast<std::uint8_t>(level),
                                        .fade = std::chrono::milliseconds{2000},
                                        .cells = std::vector<protocol::CellWeather>(protocol::weather_cells(static_cast<std::uint8_t>(level)))};
        std::printf("Weather at level %d (%zu cells):                  %zu bytes\n", level,
                    weather.cells.size(), size_of(weather));
    }
    protocol::Grains grains{.tick = 100'000, .flying = {}};
    grains.flying.assign(1, {});
    std::printf("Grains, one lump in flight:                    %zu bytes\n", size_of(grains));
    grains.flying.assign(50, {});
    std::printf("Grains, fifty lumps in flight:                 %zu bytes\n", size_of(grains));

    // A hole dug in real generated ground, a ball 1.5 m across at a time, going down: the tile
    // columns changed, as the volume codes them.
    const globe::Generator generator{globe::Planet{}};
    globe::MadeWorld made{generator};
    globe::World& world = made.world();
    const double sea = generator.sea_level().value_or(generator.planet().radius);
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
    std::printf("\nDigging a hole, a ball 1.5 m across each time, going down 0.5 m a dig:\n");
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
        std::printf("  dig %d: %zu bricks changed, %zu bytes of tile columns\n", dig + 1, bricks,
                    sent);
    }
    std::printf("  eight digs: %zu bytes sent in all\n", total);
    CHECK(total > 0);
}

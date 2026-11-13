#include <hog/server/grain_sharing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

using hog::DVec3;
using hog::WorldPosition;
using hog::flow::Parcel;
using hog::globe::Material;
using hog::server::GrainSharing;

Parcel parcel(std::uint32_t id, double x) {
    return Parcel{.id = id,
                  .at = DVec3{x, 6.0e6, 0.0},
                  .velocity = DVec3{0.0, -12.3456, 1000.0},
                  .material = Material::Sand,
                  .amount = 40'000};
}

} // namespace

TEST_CASE("grain is told to those within reach, in order of id", "[server][grains]") {
    const std::vector<Parcel> parcels{parcel(5, 10.0), parcel(2, 20.0),
                                      parcel(3, GrainSharing::reach + 50.0)};
    const auto told = GrainSharing::near(parcels, WorldPosition{0.0, 6.0e6, 0.0}, 9);
    REQUIRE(told.flying.size() == 2);
    CHECK(told.tick == 9);
    CHECK(told.flying[0].id == hog::protocol::NetId{2});
    CHECK(told.flying[1].id == hog::protocol::NetId{5});
    CHECK(told.flying[1].at[0] == 10'000);
    CHECK(told.flying[1].velocity[1] == -1235);
    // Faster than a message can tell is held at what it can.
    CHECK(told.flying[1].velocity[2] == 32'767);
    CHECK(told.flying[1].amount == 40'000);
}

TEST_CASE("when there is more grain than a message holds, the nearest is told",
          "[server][grains]") {
    std::vector<Parcel> parcels;
    for (std::uint32_t id = 1; id <= hog::protocol::most_grains + 10; ++id) {
        parcels.push_back(parcel(id, 100.0 - (static_cast<double>(id) * 0.1)));
    }
    const auto told = GrainSharing::near(parcels, WorldPosition{0.0, 6.0e6, 0.0}, 1);
    REQUIRE(told.flying.size() == hog::protocol::most_grains);
    // Ids at the far end of the line are the nearest to the player here.
    CHECK(told.flying.back().id == hog::protocol::NetId{hog::protocol::most_grains + 10});
    CHECK(told.flying.front().id == hog::protocol::NetId{11});
}

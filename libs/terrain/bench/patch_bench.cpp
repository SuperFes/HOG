#include <hog/planet/projection.hpp>
#include <hog/terrain/patch.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("patch baking", "[benchmark]") {
    const hog::planet::World world{hog::planet::PlanetConfig{.seed = 1, .radius = 115'000.0}};
    const auto& terrain = world.geology().terrain();
    const hog::planet::CellId land = hog::planet::cell_at(terrain.archipelago().center(), 10);
    // Evolves the landform outside the timing.
    (void)hog::terrain::bake_patch(world, land);

    BENCHMARK("a patch over the islands") {
        return hog::terrain::bake_patch(world, land);
    };
}

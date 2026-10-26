#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/terrain/patch.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("patch baking", "[benchmark]") {
    const hog::planet::World world{hog::planet::PlanetConfig{.seed = 1, .radius = 115'000.0}};
    const auto& terrain = world.geology().terrain();
    const hog::planet::CellId land = hog::planet::cell_at(
        terrain.nearest_land(hog::planet::start_direction, 50 * hog::planet::fixed_one), 10);
    // Evolves the landform outside the timing.
    (void)hog::terrain::bake_patch(world, land);

    BENCHMARK("a patch over land") {
        return hog::terrain::bake_patch(world, land);
    };
}

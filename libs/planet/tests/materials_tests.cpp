#include <hog/planet/materials.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using hog::planet::Material;
using hog::planet::properties;

TEST_CASE("every material has a density and holds heat", "[planet][materials]") {
    for (std::size_t index = 0; index < hog::planet::material_count; ++index) {
        const auto held = properties(static_cast<Material>(index));
        CHECK(held.density > 0.0F);
        CHECK(held.specific_heat > 0.0F);
    }
}

TEST_CASE("materials float and sink as they should", "[planet][materials]") {
    CHECK(properties(Material::Ice).density < properties(Material::Water).density);
    CHECK(properties(Material::Snow).density < properties(Material::Ice).density);
    // Continents ride high on denser ocean floor.
    CHECK(properties(Material::Granite).density < properties(Material::Basalt).density);
    // Melt rises through the rock it came from.
    CHECK(properties(Material::Magma).density < properties(Material::Basalt).density);
}

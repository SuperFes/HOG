#include <hog/client/globe_view.hpp>
#include <hog/core/assert.hpp>
#include <hog/core/log.hpp>
#include <hog/core/text_file.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hog::client {

Result<globe::Planet> globe_planet(std::optional<std::string_view> named, std::uint64_t seed,
                                   double radius) {
    globe::Planet planet;
    if (named) {
        if (const auto preset = globe::preset(*named)) {
            planet = *preset;
        } else {
            // A planet file sets its own size.
            auto loaded = globe::load_planet(std::string{*named});
            if (loaded) {
                loaded->seed = seed;
            }
            return loaded;
        }
    }
    planet.seed = seed;
    planet.radius = radius;
    if (auto valid = globe::validate(planet); !valid) {
        return std::unexpected(std::move(valid).error());
    }
    return planet;
}

Result<globe::Planet> globe_planet_in(const std::filesystem::path& file, std::uint64_t seed,
                                      double radius) {
    const auto text = read_text_file(file);
    if (!text || !text->contains("[globe]")) {
        return globe_planet(std::nullopt, seed, radius);
    }
    auto described = globe::parse_planet(*text);
    if (described) {
        described->seed = seed;
    }
    return described;
}

Vec3 surface_colour(globe::Material material) {
    using globe::Material;
    switch (material) {
    case Material::Air:
        return {0.8F, 0.9F, 1.0F};
    case Material::Water:
        return {0.05F, 0.2F, 0.45F};
    case Material::Snow:
        return {0.9F, 0.92F, 0.95F};
    case Material::Soil:
        return {0.3F, 0.22F, 0.12F};
    case Material::Sand:
        return {0.76F, 0.68F, 0.5F};
    case Material::Sediment:
        return {0.6F, 0.52F, 0.36F};
    case Material::Sandstone:
        return {0.7F, 0.5F, 0.3F};
    case Material::Shale:
        return {0.4F, 0.38F, 0.35F};
    case Material::Limestone:
        return {0.75F, 0.73F, 0.65F};
    case Material::Granite:
        return {0.55F, 0.5F, 0.48F};
    case Material::Basalt:
        return {0.18F, 0.18F, 0.2F};
    case Material::Mantle:
        return {0.3F, 0.36F, 0.22F};
    case Material::Iron:
        return {0.42F, 0.4F, 0.38F};
    case Material::Anorthosite:
        return {0.7F, 0.7F, 0.68F};
    case Material::Regolith:
        return {0.52F, 0.5F, 0.48F};
    case Material::Firn:
        return {0.82F, 0.86F, 0.9F};
    case Material::Ice:
        return {0.55F, 0.75F, 0.9F};
    case Material::Clay:
        return {0.52F, 0.36F, 0.24F};
    case Material::Gravel:
        return {0.44F, 0.4F, 0.35F};
    }
    return {1.0F, 0.0F, 1.0F};
}

std::array<terrain::Look, globe::material_count> globe_looks() {
    using globe::Material;
    struct Variation {
        Material material;
        DVec3 accent; // relative to the colour
        double share;
        double grain;
        double mottle;
    };
    constexpr auto vary = [](Material material, DVec3 accent, double share, double grain,
                             double mottle) {
        return Variation{.material = material,
                         .accent = accent,
                         .share = share,
                         .grain = grain,
                         .mottle = mottle};
    };
    constexpr std::array variations{vary(Material::Air, {1.0, 1.0, 1.0}, 0.0, 0.0, 0.0),
                                    vary(Material::Water, {1.0, 1.0, 1.0}, 0.0, 0.05, 0.1),
                                    // Blue in the hollows, grey where it's old.
                                    vary(Material::Snow, {0.9, 0.95, 1.05}, 0.3, 0.08, 0.08),
                                    // Drier, paler patches.
                                    vary(Material::Soil, {1.3, 1.15, 0.9}, 0.3, 0.35, 0.25),
                                    vary(Material::Sand, {1.1, 1.05, 0.95}, 0.3, 0.2, 0.12),
                                    // Greyer where it's wet.
                                    vary(Material::Sediment, {0.8, 0.8, 0.85}, 0.3, 0.3, 0.2),
                                    // Buff, with rusty patches.
                                    vary(Material::Sandstone, {0.85, 0.7, 0.6}, 0.3, 0.3, 0.2),
                                    // Dark grey, browner where it weathers.
                                    vary(Material::Shale, {1.15, 1.0, 0.85}, 0.3, 0.35, 0.15),
                                    // Pale, with darker lichen.
                                    vary(Material::Limestone, {0.75, 0.75, 0.72}, 0.3, 0.25, 0.3),
                                    // Speckled, bluer where the feldspar shows.
                                    vary(Material::Granite, {1.0, 1.2, 1.4}, 0.3, 0.5, 0.15),
                                    // Near black, rusting.
                                    vary(Material::Basalt, {1.4, 1.1, 0.9}, 0.2, 0.35, 0.25),
                                    vary(Material::Mantle, {0.8, 0.9, 0.7}, 0.3, 0.3, 0.2),
                                    vary(Material::Iron, {1.3, 1.1, 0.9}, 0.3, 0.3, 0.2),
                                    vary(Material::Anorthosite, {0.9, 0.9, 0.92}, 0.3, 0.3, 0.2),
                                    vary(Material::Regolith, {0.85, 0.85, 0.85}, 0.35, 0.3, 0.25),
                                    // Grey where it's old and dirty.
                                    vary(Material::Firn, {0.85, 0.88, 0.92}, 0.3, 0.1, 0.15),
                                    // Clear blue deep down, whiter where it's cracked.
                                    vary(Material::Ice, {1.5, 1.25, 1.1}, 0.3, 0.05, 0.2),
                                    // Ochre streaks, greyer where it's gleyed.
                                    vary(Material::Clay, {1.2, 1.05, 0.8}, 0.3, 0.15, 0.3),
                                    // Pale stones in darker earth.
                                    vary(Material::Gravel, {1.5, 1.45, 1.4}, 0.4, 0.6, 0.2)};
    static_assert(variations.size() == globe::material_count);

    std::array<terrain::Look, globe::material_count> looks{};
    for (const Variation& variation : variations) {
        const auto material = static_cast<std::size_t>(variation.material);
        const DVec3 colour{surface_colour(variation.material)};
        looks.at(material) = {.colour = colour,
                              .accent = glm::min(colour * variation.accent, DVec3{1.0}),
                              .accent_share = variation.share,
                              .grain = variation.grain,
                              .mottle = variation.mottle,
                              .seed = 0x1000 + material};
    }
    return looks;
}

std::vector<terrain::MaterialImage> block_tiles() {
    std::vector<terrain::MaterialImage> tiles;
    tiles.reserve(globe::material_count);
    for (const terrain::Look& look : globe_looks()) {
        tiles.push_back(terrain::material_image(look, material_size));
    }
    return tiles;
}

} // namespace hog::client

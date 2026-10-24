#include <hog/flora/stand.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/grass_lattice.hpp>

#include <cstdint>
#include <numbers>
#include <optional>

namespace hog::render {

namespace {

// The child of a cell its dice pick: i in the low bit, j in the high.
std::uint32_t picked_child(std::uint32_t hash) noexcept {
    return hash >> 30U;
}

// Salts of the clump corners' dice, set apart from the blades'.
constexpr std::uint32_t clump_salt = 0x517cU;
constexpr std::uint32_t wobble_salt = 0xc0bbU;
// How far either way the finer corners shift where a clump's edge falls.
constexpr float wobble = 0.2F;

float unit(std::uint32_t hash) noexcept {
    return static_cast<float>(hash >> 8U) / 16'777'216.0F;
}

float mixed(float from, float to, float by) noexcept {
    return from + ((to - from) * by);
}

// The dice of the corners of the cell `levels` up from `blade`, blended to where in it the blade
// stands; each corner's dice as `dice` casts it.
template <typename Dice>
float blended(planet::FaceIJ blade, Vec2 spot, int levels, Dice dice) noexcept {
    const std::uint32_t inside = (1U << static_cast<unsigned>(levels)) - 1U;
    const auto cells = static_cast<float>(inside + 1U);
    const float x = (static_cast<float>(blade.i & inside) + spot.x) / cells;
    const float y = (static_cast<float>(blade.j & inside) + spot.y) / cells;
    const auto corner = [&](std::uint32_t across, std::uint32_t up) {
        return dice(planet::FaceIJ{.face = blade.face,
                                   .i = (blade.i >> static_cast<unsigned>(levels)) + across,
                                   .j = (blade.j >> static_cast<unsigned>(levels)) + up,
                                   .level = blade.level - levels});
    };
    return mixed(mixed(corner(0, 0), corner(1, 0), x), mixed(corner(0, 1), corner(1, 1), x), y);
}

} // namespace

int grass_lattice_level(double planet_radius) {
    return planet::level_for(finest_grass_cell, planet_radius);
}

GrassLattice grass_lattice(double planet_radius, std::uint64_t world_seed) {
    return {.level = grass_lattice_level(planet_radius),
            .seed = static_cast<std::uint32_t>(world_seed ^ (world_seed >> 32U))};
}

std::uint32_t grass_hash(planet::FaceIJ cell, std::uint32_t seed) noexcept {
    const auto where =
        static_cast<std::uint32_t>(cell.level) | (static_cast<std::uint32_t>(cell.face) << 8U);
    return flora::hashed((cell.i * 0x8da6'b343U) ^
                         flora::hashed((cell.j * 0xd816'3841U) ^ flora::hashed(seed ^ where)));
}

planet::FaceIJ standing_for(planet::FaceIJ cell, int lattice_level, std::uint32_t seed) noexcept {
    while (cell.level < lattice_level) {
        const std::uint32_t child = picked_child(grass_hash(cell, seed));
        cell = {.face = cell.face,
                .i = (cell.i << 1U) | (child & 1U),
                .j = (cell.j << 1U) | (child >> 1U),
                .level = cell.level + 1};
    }
    return cell;
}

int stands_up(planet::FaceIJ cell, std::uint32_t seed, int most) noexcept {
    int up = 0;
    while (up < most && cell.level > 0) {
        const std::uint32_t child = (cell.i & 1U) | ((cell.j & 1U) << 1U);
        cell = {.face = cell.face, .i = cell.i >> 1U, .j = cell.j >> 1U, .level = cell.level - 1};
        if (picked_child(grass_hash(cell, seed)) != child) {
            break;
        }
        ++up;
    }
    return up;
}

planet::FaceIJ unpicked_child(planet::FaceIJ cell, int which, std::uint32_t seed) noexcept {
    const auto picked = picked_child(grass_hash(cell, seed));
    const auto nth = static_cast<std::uint32_t>(which);
    const std::uint32_t child = nth + (nth >= picked ? 1U : 0U);
    return {.face = cell.face,
            .i = (cell.i << 1U) | (child & 1U),
            .j = (cell.j << 1U) | (child >> 1U),
            .level = cell.level + 1};
}

std::uint32_t thinning_dice(planet::FaceIJ cell, std::uint32_t seed, int most) noexcept {
    int up = 0;
    std::uint32_t child = 4;
    std::optional<std::uint32_t> hash;
    while (up < most && cell.level > 0) {
        child = (cell.i & 1U) | ((cell.j & 1U) << 1U);
        cell = {.face = cell.face, .i = cell.i >> 1U, .j = cell.j >> 1U, .level = cell.level - 1};
        hash = grass_hash(cell, seed);
        if (picked_child(*hash) != child) {
            break;
        }
        child = 4;
        ++up;
    }
    return flora::hashed(hash.value_or(grass_hash(cell, seed)) + child + 1U);
}

float clumped(planet::FaceIJ blade, Vec2 spot, std::uint32_t seed, float on) noexcept {
    const float corners = blended(blade, spot, clump_levels, [&](planet::FaceIJ corner) {
        return unit(grass_hash(corner, seed ^ clump_salt)) < on ? 1.0F : 0.0F;
    });
    const float rough = blended(blade, spot, clump_wobble_levels, [&](planet::FaceIJ corner) {
        return unit(grass_hash(corner, seed ^ wobble_salt));
    });
    return corners + (2.0F * wobble * (rough - 0.5F)) - 0.5F;
}

float clump_corners_on(float share) noexcept {
    if (share <= 0.0F || share >= 1.0F) {
        return share <= 0.0F ? 0.0F : 1.0F;
    }
    // A cell's ground over half its corners' blend, with one corner on: from a corner to where
    // x y = 1/2. Two on cover half of it, three all but one corner's share.
    const double one = (1.0 - std::numbers::ln2) / 2.0;
    const auto covered = [&](double on) {
        const double off = 1.0 - on;
        return (4.0 * on * off * off * off * one) + (6.0 * on * on * off * off * 0.5) +
               (4.0 * on * on * on * off * (1.0 - one)) + (on * on * on * on);
    };
    double low = 0.0;
    double high = 1.0;
    for (int step = 0; step < 40; ++step) {
        const double middle = (low + high) / 2.0;
        (covered(middle) < static_cast<double>(share) ? low : high) = middle;
    }
    return static_cast<float>((low + high) / 2.0);
}

Vec2 grass_spot(std::uint32_t hash) noexcept {
    const std::uint32_t spot = flora::hashed(hash);
    return {static_cast<float>(spot & 0xffffU) / 65536.0F,
            static_cast<float>(spot >> 16U) / 65536.0F};
}

} // namespace hog::render

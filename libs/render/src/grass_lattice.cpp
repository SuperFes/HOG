#include <hog/flora/stand.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/grass_lattice.hpp>

#include <algorithm>
#include <cstdint>
#include <numbers>

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

constexpr std::uint32_t dither_salt = 0xd17eU;

// How a dither block's order is scrambled: its cells' i and j flipped by these bits.
struct Dither {
    std::uint32_t flip_i;
    std::uint32_t flip_j;
};

// The scrambling of the dither block whose cells all lie within `block`.
Dither dither_of(planet::FaceIJ block, std::uint32_t seed) noexcept {
    const std::uint32_t hash = grass_hash(block, seed ^ dither_salt);
    return {.flip_i = hash, .flip_j = hash >> 8U};
}

// The share kept by the time thinning keeps the `order`th of a block's 3 << (2 bits).
float dice_of(std::uint32_t order, unsigned bits) noexcept {
    return (static_cast<float>(order) + 0.5F) / static_cast<float>(3U << (2U * bits));
}

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

float dithered(planet::FaceIJ parent, int which, std::uint32_t seed) noexcept {
    const auto bits = static_cast<unsigned>(std::min(dither_levels, parent.level));
    const Dither block = dither_of({.face = parent.face,
                                    .i = parent.i >> bits,
                                    .j = parent.j >> bits,
                                    .level = parent.level - static_cast<int>(bits)},
                                   seed);
    const std::uint32_t mask = (1U << bits) - 1U;
    const std::uint32_t x = (parent.i ^ block.flip_i) & mask;
    const std::uint32_t y = (parent.j ^ block.flip_j) & mask;
    std::uint32_t rank = 0;
    for (unsigned level = 0; level < bits; ++level) {
        const std::uint32_t across = (x >> level) & 1U;
        const std::uint32_t up = (y >> level) & 1U;
        rank = (rank << 2U) | ((across ^ up) << 1U) | up;
    }
    return dice_of((static_cast<std::uint32_t>(which) << (2U * bits)) + rank, bits);
}

float thinning_dice(planet::FaceIJ cell, std::uint32_t seed, int most) noexcept {
    for (int up = 0; up < most && cell.level > 0; ++up) {
        const std::uint32_t child = (cell.i & 1U) | ((cell.j & 1U) << 1U);
        cell = {.face = cell.face, .i = cell.i >> 1U, .j = cell.j >> 1U, .level = cell.level - 1};
        const std::uint32_t picked = picked_child(grass_hash(cell, seed));
        if (picked != child) {
            return dithered(cell, static_cast<int>(child - (child > picked ? 1U : 0U)), seed);
        }
    }
    return 0.0F;
}

Sowing sown_in_layer(planet::FaceIJ patch, int above, std::uint32_t into,
                     std::uint32_t seed) noexcept {
    const int level = patch.level + above;
    const auto bits = static_cast<unsigned>(std::min(dither_levels, level));
    const auto deep = static_cast<unsigned>(above);
    if (deep < bits) {
        // Less than a block: as they stand.
        const std::uint32_t cell = into / 3U;
        const planet::FaceIJ parent{.face = patch.face,
                                    .i = (patch.i << deep) + (cell & ((1U << deep) - 1U)),
                                    .j = (patch.j << deep) + (cell >> deep),
                                    .level = level};
        const auto which = static_cast<int>(into % 3U);
        return {.parent = parent, .which = which, .dice = dithered(parent, which, seed)};
    }
    // Each block's first, then each block's next.
    const unsigned blocks = deep - bits;
    const std::uint32_t order = into >> (2U * blocks);
    const std::uint32_t block = into & ((1U << (2U * blocks)) - 1U);
    const planet::FaceIJ corner{.face = patch.face,
                                .i = (patch.i << blocks) + (block & ((1U << blocks) - 1U)),
                                .j = (patch.j << blocks) + (block >> blocks),
                                .level = level - static_cast<int>(bits)};
    const Dither dither = dither_of(corner, seed);
    const std::uint32_t rank = order & ((1U << (2U * bits)) - 1U);
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    for (unsigned level_in = 0; level_in < bits; ++level_in) {
        const std::uint32_t digit = (rank >> (2U * (bits - 1U - level_in))) & 3U;
        const std::uint32_t up = digit & 1U;
        x |= ((digit >> 1U) ^ up) << level_in;
        y |= up << level_in;
    }
    const std::uint32_t mask = (1U << bits) - 1U;
    return {.parent = {.face = patch.face,
                       .i = (corner.i << bits) + ((x ^ dither.flip_i) & mask),
                       .j = (corner.j << bits) + ((y ^ dither.flip_j) & mask),
                       .level = level},
            .which = static_cast<int>(order >> (2U * bits)),
            .dice = dice_of(order, bits)};
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

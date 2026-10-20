#include <hog/flora/stand.hpp>

#include <cstdint>
#include <optional>

namespace hog::flora {

namespace {

// a + t(b - a), spelled out: the ordinary rounding of each step is part of the contract.
float mixed(float a, float b, float t) noexcept {
    return a + (t * (b - a));
}

float smooth(float t) noexcept {
    return t * t * (3.0F - (2.0F * t));
}

float stand_corner(std::uint32_t base_x, std::uint32_t base_y, std::uint32_t face,
                   std::uint32_t salt) noexcept {
    return unit(hashed(base_x ^ (face * 0x9e3779b9U) ^ salt) + base_y);
}

// Value noise over the trees' lattice, cells gathered `period_log` levels up. The coordinates are
// never negative, so truncation is the floor.
float stand_octave(Vec2 cell, std::uint32_t face, std::uint32_t period_log,
                   std::uint32_t salt) noexcept {
    const auto period = static_cast<float>(std::uint32_t{1} << period_log);
    const float at_x = cell.x / period;
    const float at_y = cell.y / period;
    const auto base_x = static_cast<std::uint32_t>(at_x);
    const auto base_y = static_cast<std::uint32_t>(at_y);
    const float fx = smooth(at_x - static_cast<float>(base_x));
    const float fy = smooth(at_y - static_cast<float>(base_y));
    const float c00 = stand_corner(base_x, base_y, face, salt);
    const float c10 = stand_corner(base_x + 1, base_y, face, salt);
    const float c01 = stand_corner(base_x, base_y + 1, face, salt);
    const float c11 = stand_corner(base_x + 1, base_y + 1, face, salt);
    return mixed(mixed(c00, c01, fy), mixed(c10, c11, fy), fx);
}

float saturated(float value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

float smoothstepped(float from, float to, float value) noexcept {
    return smooth(saturated((value - from) / (to - from)));
}

// A broad field stretched to span its range.
float drawing_field(Vec2 cell, std::uint32_t face, std::uint32_t period_log,
                    std::uint32_t salt) noexcept {
    return saturated(0.5F + (2.2F * (stand_octave(cell, face, period_log, salt) - 0.5F)));
}

// The field roughened by the cell's own dice, against a share.
bool drawn_in(Vec2 cell, std::uint32_t face, std::uint32_t period_log, std::uint32_t salt,
              float dice, float share) noexcept {
    return mixed(drawing_field(cell, face, period_log, salt), dice, 0.3F) < share;
}

// Of the cells about `cell`, the share drawn_in draws, their dice evenly spread.
float drawn_share(Vec2 cell, std::uint32_t face, std::uint32_t period_log, std::uint32_t salt,
                  float share) noexcept {
    return saturated((share - (0.7F * drawing_field(cell, face, period_log, salt))) / 0.3F);
}

constexpr std::uint32_t broadleaf_period_log = 5;
constexpr std::uint32_t broadleaf_salt = 0x0a4c1U;
constexpr std::uint32_t maple_period_log = 4;
constexpr std::uint32_t maple_salt = 0x3a91eU;
constexpr std::uint32_t palm_period_log = 3;
constexpr std::uint32_t palm_salt = 0x9a1f3U;

// How deep into a stand a tree grows makes it older, and its age taller.
float stand_age(float wooded) noexcept {
    return mixed(0.55F, 1.0F, wooded);
}
float reached(float age) noexcept {
    return mixed(0.2F, 1.2F, age * (2.0F - age));
}
// Tallest deep in a stand.
float stand_scale(float wooded) noexcept {
    return mixed(0.8F, 1.05F, wooded);
}

float within(const Range& range, float value) noexcept {
    return saturated((value - (range.low - range.margin)) / range.margin) *
           saturated(((range.high + range.margin) - value) / range.margin);
}

// Metres down a tree's roots reach for water.
constexpr float rooting = 4.0F;

// Grown back since the last loss, young trees hold more of the ground than of the wood: many small
// stems where the old stood. Of their share y of the wood, they take sqrt(y) of the spots, as far
// as the old and the standing dead leave room, and reach up to 2 sqrt(y) of the old's age.
struct Regrowth {
    float old = 0.0F; // the spots of the old growth, as its share of the wood
    float spots = 0.0F;
    float maturity = 1.0F;
};

Regrowth regrowth(const Habitat& habitat) noexcept {
    const float old = std::min(habitat.established, habitat.living);
    const float young = habitat.living - old;
    if (young <= 0.0F) {
        return {.old = old};
    }
    const float root = std::sqrt(young);
    const float room = std::max(1.0F - old - habitat.dead, young);
    return {.old = old, .spots = std::min(root, room), .maturity = std::min(2.0F * root, 1.0F)};
}

// How a tree fares by its own place among its woods' shares, if it stands at all; a tree grown
// back since the last loss is made younger, the youngest those that came back last.
std::optional<Condition> fared(std::uint32_t hash, const Habitat& habitat, float& age) noexcept {
    const Regrowth young = regrowth(habitat);
    const float living = young.old + young.spots;
    const float fate = unit(hash + 20U);
    if (fate >= living) {
        if (fate >= living + habitat.dead) {
            return std::nullopt;
        }
        return unit(hash + 22U) < habitat.charred ? Condition::Charred : Condition::Dead;
    }
    if (fate >= living - habitat.failing) {
        return Condition::Failing;
    }
    if (fate >= young.old) {
        age *= young.maturity * (living - fate) / young.spots;
        return Condition::Sound;
    }
    // The old trees that lived through a fire bear its marks while its dead still stand thick
    // about them.
    if (unit(hash + 22U) < habitat.charred * saturated(4.0F * habitat.dead)) {
        return Condition::Scorched;
    }
    return Condition::Sound;
}

} // namespace

std::uint32_t hashed(std::uint32_t at) noexcept {
    at ^= at >> 16U;
    at *= 0x7feb352dU;
    at ^= at >> 15U;
    at *= 0x846ca68bU;
    at ^= at >> 16U;
    return at;
}

float unit(std::uint32_t at) noexcept {
    return static_cast<float>(hashed(at) & 0xffffU) / 65536.0F;
}

std::uint32_t cell_hash(std::uint32_t lattice_i, std::uint32_t lattice_j,
                        std::uint32_t face) noexcept {
    return hashed(hashed(lattice_i ^ (face * 0x9e3779b9U)) + lattice_j);
}

Habitat habitat_of(const planet::CellState& state) noexcept {
    const auto real = [](std::int64_t fixed) {
        return static_cast<float>(static_cast<double>(fixed) /
                                  static_cast<double>(planet::fixed_one));
    };
    return {.temperature = real(state.temperature),
            .moisture = real(state.moisture),
            .water_table = real(state.water_table)};
}

float wetness(const Habitat& habitat) noexcept {
    const float table = saturated(1.0F - (habitat.water_table / rooting));
    return habitat.moisture > table ? habitat.moisture : table;
}

float suited(Genus genus, const Habitat& habitat) noexcept {
    const Niche niche = niche_of(genus);
    return within(niche.warmth, habitat.temperature) * within(niche.wetness, wetness(habitat));
}

float stand_field(Vec2 cell, std::uint32_t face) noexcept {
    return (0.65F * stand_octave(cell, face, 6U, 0x51a9dU)) +
           (0.35F * stand_octave(cell, face, 4U, 0xb07a7U));
}

Vec2 tree_spot(std::uint32_t lattice_i, std::uint32_t lattice_j, std::uint32_t face) noexcept {
    const std::uint32_t hash = cell_hash(lattice_i, lattice_j, face);
    return {unit(hash), unit(hash + 1U)};
}

std::optional<Stood> tree_grown(std::uint32_t lattice_i, std::uint32_t lattice_j,
                                std::uint32_t face, planet::Biome biome,
                                const Habitat& habitat) noexcept {
    const BiomeTrees trees = trees_of(biome);
    if (trees.height <= 0.0F) {
        return std::nullopt;
    }
    const std::uint32_t hash = cell_hash(lattice_i, lattice_j, face);
    const Vec2 spot{unit(hash), unit(hash + 1U)};
    const Vec2 cell{static_cast<float>(lattice_i) + spot.x, static_cast<float>(lattice_j) + spot.y};
    const float wooded = smoothstepped(trees.edge, trees.edge + 0.1F, stand_field(cell, face));
    const float occupancy = mixed(trees.lone, trees.inside, wooded);
    if (unit(hash + 2U) >= occupancy) {
        return std::nullopt;
    }
    // Most trees in a stand are grown; its edges and the open are where the young stand, the
    // woods spreading outward.
    const float drawn = 1.0F - unit(hash + 4U);
    float age = (1.0F - (drawn * drawn)) * stand_age(wooded);
    // Palms gather in groves of their own; of the rest, broadleaves in theirs, maples in smaller
    // stands within them.
    Genus genus = Genus::Pine;
    if (trees.palm > 0.0F &&
        drawn_in(cell, face, palm_period_log, palm_salt, unit(hash + 16U), trees.palm)) {
        genus = Genus::Palm;
    } else if (trees.broadleaf > 0.0F && drawn_in(cell, face, broadleaf_period_log, broadleaf_salt,
                                                  unit(hash + 8U), trees.broadleaf)) {
        genus = drawn_in(cell, face, maple_period_log, maple_salt, unit(hash + 14U), trees.maple)
                    ? Genus::Maple
                    : Genus::Oak;
    }
    // Each kind thins out toward the edges of its climate, leaving the ground to the others.
    if (unit(hash + 18U) >= suited(genus, habitat)) {
        return std::nullopt;
    }
    const std::optional<Condition> condition = fared(hash, habitat, age);
    if (!condition) {
        return std::nullopt;
    }
    // Quick to rise while young, then barely taller.
    float height =
        trees.height * reached(age) * (0.85F + (0.3F * unit(hash + 5U))) * stand_scale(wooded);
    height *= stature(genus);
    return Stood{.spot = spot,
                 .height = height,
                 .wooded = wooded,
                 .age = age,
                 .hash = hash,
                 .genus = genus,
                 .condition = *condition};
}

Woodland woodland_at(Vec2 cell, std::uint32_t face, planet::Biome biome,
                     const Habitat& habitat) noexcept {
    const BiomeTrees trees = trees_of(biome);
    if (trees.height <= 0.0F) {
        return {};
    }
    // Of the cells, the share each kind is drawn to, kept as far as the climate suits it.
    const float palm =
        trees.palm > 0.0F ? drawn_share(cell, face, palm_period_log, palm_salt, trees.palm) : 0.0F;
    const float broadleaf = trees.broadleaf > 0.0F ? drawn_share(cell, face, broadleaf_period_log,
                                                                 broadleaf_salt, trees.broadleaf)
                                                   : 0.0F;
    const float maple = drawn_share(cell, face, maple_period_log, maple_salt, trees.maple);
    const float palms = palm * suited(Genus::Palm, habitat);
    const float oaks = broadleaf * (1.0F - maple) * suited(Genus::Oak, habitat);
    const float maples = broadleaf * maple * suited(Genus::Maple, habitat);
    const float pines = (1.0F - broadleaf) * suited(Genus::Pine, habitat);
    const float kept = palms + ((1.0F - palm) * (oaks + maples + pines));
    if (kept <= 0.0F) {
        return {};
    }
    const float wooded = smoothstepped(trees.edge, trees.edge + 0.1F, stand_field(cell, face));
    // A tree's age is the stand's times 1 - d^2 for its dice d, so on average 2/3 of it, and its
    // square 8/15 of the stand's squared.
    const float oldest = stand_age(wooded);
    float mean_reach = 0.2F + ((4.0F / 3.0F) * oldest) - ((8.0F / 15.0F) * oldest * oldest);
    // Those grown back since the last loss are younger by an even share of their age up to their
    // maturity m, so m/3 as much of it on average and 8/45 m^2 of its square.
    const Regrowth young = regrowth(habitat);
    const float living = young.old + young.spots;
    if (young.spots > 0.0F) {
        const float m = young.maturity * oldest;
        const float young_reach = 0.2F + ((2.0F / 3.0F) * m) - ((8.0F / 45.0F) * m * m);
        mean_reach = mixed(young_reach, mean_reach, young.old / living);
    }
    const float others = oaks + maples + pines;
    return Woodland{.grown = mixed(trees.lone, trees.inside, wooded) * kept * living,
                    .height = trees.height * mean_reach * stand_scale(wooded),
                    .broadleaf = others > 0.0F ? (oaks + maples) / others : 0.0F,
                    .maple = oaks + maples > 0.0F ? maples / (oaks + maples) : 0.0F,
                    .palm = palms / kept};
}

} // namespace hog::flora

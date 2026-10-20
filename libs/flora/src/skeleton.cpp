#include "dice.hpp"
#include "trig.hpp"

#include <hog/flora/skeleton.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace hog::flora {

namespace {

constexpr float tau = 6.2831853F;
constexpr Vec3 up{0.0F, 0.0F, 1.0F};

Quat about_x(float radians) noexcept {
    return {turning_cos(radians * 0.5F), turning_sin(radians * 0.5F), 0.0F, 0.0F};
}

Quat about_z(float radians) noexcept {
    return {turning_cos(radians * 0.5F), 0.0F, 0.0F, turning_sin(radians * 0.5F)};
}

struct Shoot {
    Vec3 origin{0.0F};
    Quat frame{1.0F, 0.0F, 0.0F, 0.0F};
    float length = 1.0F;
    float radius = 0.1F;
    int level = 0;
};

// The ring partway along a grown bough, its frame leaned between the rings it lies between.
Ring between(const Bough& bough, float along) {
    const float at = along * static_cast<float>(bough.rings.size() - 1);
    const auto first = std::min(static_cast<std::size_t>(at), bough.rings.size() - 2);
    const float rest = at - static_cast<float>(first);
    const Ring& near = bough.rings[first];
    const Ring& far = bough.rings[first + 1];
    return {.centre = near.centre + ((far.centre - near.centre) * rest),
            .frame = glm::normalize(near.frame + ((far.frame - near.frame) * rest)),
            .radius = near.radius + ((far.radius - near.radius) * rest)};
}

// Foliage: the twig's needles as one solid frond along its spine, sized to its own twig, so
// short crown twigs carry short blades. The dice are dealt in named order, as everywhere.
void lay_frond(const Species& species, const Bough& bough, const Shoot& shoot, Dice& dice,
               Skeleton& tree) {
    const float sway = dice.around(species.frond_sway);
    const std::uint32_t seed = dice.roll();
    const float width = species.frond_width * shoot.length * (1.0F + sway);
    tree.fronds.push_back({.spine = bough.rings,
                           .width = width,
                           .tilt = species.frond_tilt,
                           .fullness = species.frond_fullness,
                           .seed = seed});
}

// One fruit, an ellipsoid of wood running from `from` along its frame's heading.
Bough one_fruit(const Fruit& fruit, Vec3 from, Quat frame) {
    constexpr std::array<float, 5> along{0.0F, 0.15F, 0.5F, 0.85F, 1.0F};
    constexpr std::array<float, 5> across{0.0F, 0.75F, 1.0F, 0.75F, 0.0F};
    const float length = 2.0F * fruit.radius * fruit.length;
    const Vec3 heading = frame * up;
    Bough bough{.level = 0, .sides = 6, .rings = {}};
    for (std::size_t ring = 0; ring < along.size(); ++ring) {
        bough.rings.push_back({.centre = from + (heading * (length * along.at(ring))),
                               .frame = frame,
                               .radius = fruit.radius * across.at(ring)});
    }
    return bough;
}

// Fruit hung round the trunk at `under`, just below its crown, each on a short stalk, hanging
// down and out; then the fallen, lying on their sides on the ground beneath, rolled out from the
// trunk and sunk a third into the ground, which also hides a gentle slope under them.
void hang_fruit(const Fruit& fruit, const Ring& under, Dice& dice, Skeleton& tree) {
    for (int nut = 0; nut < fruit.count; ++nut) {
        const float radial =
            (tau * (static_cast<float>(nut) / static_cast<float>(fruit.count))) + dice.around(0.4F);
        const float hang = 2.5F + dice.around(0.3F);
        const float drop = dice.unit() * fruit.radius;
        const Quat frame = glm::normalize(under.frame * about_z(radial) * about_x(hang));
        // The way a child leaning off the trunk at this turn leans, as grow() sprouts them.
        const Vec3 out = under.frame * (about_z(radial) * Vec3{0.0F, -1.0F, 0.0F});
        const Vec3 stalk = under.centre + (out * (under.radius + (0.5F * fruit.radius))) -
                           ((under.frame * up) * drop);
        tree.boughs.push_back(one_fruit(fruit, stalk, frame));
    }
    const float length = 2.0F * fruit.radius * fruit.length;
    for (int lying = 0; lying < fruit.fallen; ++lying) {
        const float way = dice.unit() * tau;
        const float roll = under.radius + fruit.radius + (dice.unit() * fruit.scatter);
        const float turn = dice.unit() * tau;
        const float tip = dice.around(0.25F);
        const Vec3 landed = Vec3{under.centre.x + (turning_cos(way) * roll),
                                 under.centre.y + (turning_sin(way) * roll), fruit.radius / 3.0F};
        const Quat frame = glm::normalize(about_z(turn) * about_x((0.25F * tau) + tip));
        tree.boughs.push_back(one_fruit(fruit, landed - ((frame * up) * (0.5F * length)), frame));
    }
}

} // namespace

Skeleton grow(const Species& species, std::uint32_t seed) {
    Dice dice{seed};
    Skeleton tree;
    const Level& trunk = species.levels[0];
    const float length = trunk.length * (1.0F + dice.around(trunk.sway));
    // Only a leaning species rolls for it, so the others deal the trees they always have.
    Quat foot{1.0F, 0.0F, 0.0F, 0.0F};
    if (species.lean > 0.0F) {
        const float lean = dice.unit() * species.lean;
        const float way = dice.unit() * tau;
        foot = glm::normalize(about_z(way) * about_x(lean));
    }
    std::vector<Shoot> shoots{{.origin = Vec3{0.0F},
                               .frame = foot,
                               .length = length,
                               .radius = trunk.radius,
                               .level = 0}};

    // Breadth first, one stream of dice: every machine deals the same tree.
    for (std::size_t at = 0; at < shoots.size(); ++at) {
        const Shoot shoot = shoots[at];
        const Level& level = species.levels[static_cast<std::size_t>(shoot.level)];
        const bool deepest = shoot.level + 1 >= species.grown;
        const float step = shoot.length / static_cast<float>(level.sections);

        Bough bough{.level = shoot.level, .sides = level.sides, .rings = {}};
        bough.rings.reserve(static_cast<std::size_t>(level.sections) + 1);
        Vec3 head = shoot.origin;
        Quat frame = shoot.frame;
        bough.rings.push_back({.centre = head, .frame = frame, .radius = shoot.radius});
        for (int section = 1; section <= level.sections; ++section) {
            // Wander a little, and lean toward (or away from) upright, thinner wood more
            // easily. The dice are dealt in named order: an expression with two rolls would
            // leave the deal to the compiler, and every machine must grow the same tree.
            const float pitch = dice.around(level.gnarl);
            const float yaw = dice.around(level.gnarl);
            frame = frame * about_x(pitch) * about_z(yaw);
            const Vec3 heading = frame * up;
            const Vec3 axis = glm::cross(heading, up);
            const float swing = glm::length(axis);
            if (level.rise != 0.0F && swing > 1e-4F) {
                const float lean =
                    std::clamp(level.rise / std::max(shoot.radius, 0.05F), -0.5F, 0.5F);
                const Vec3 turn = (axis / swing) * turning_sin(lean * 0.5F);
                frame =
                    glm::normalize(Quat{turning_cos(lean * 0.5F), turn.x, turn.y, turn.z} * frame);
            }
            head += (frame * up) * step;
            const float grown = static_cast<float>(section) / static_cast<float>(level.sections);
            float radius = shoot.radius * (1.0F - (level.taper * grown));
            if (deepest && section == level.sections) {
                radius = std::min(radius, 0.004F);
            }
            bough.rings.push_back({.centre = head, .frame = frame, .radius = radius});
        }

        if (!deepest) {
            // The next level sprouts in whorls, each ringing the parent a golden turn on.
            const Level& next = species.levels[static_cast<std::size_t>(shoot.level) + 1];
            const int whorl = std::max(level.whorl, 1);
            const int whorls = std::max((level.children + whorl - 1) / whorl, 1);
            for (int child = 0; child < level.children; ++child) {
                const int round = child / whorl;
                const int spoke = child % whorl;
                const float along = std::clamp(
                    level.start + ((1.0F - level.start) *
                                   ((static_cast<float>(round) + 0.5F + dice.around(0.2F)) /
                                    static_cast<float>(whorls))),
                    level.start, 0.97F);
                const Ring ring = between(bough, along);
                const float radial =
                    (tau * (static_cast<float>(spoke) / static_cast<float>(whorl))) +
                    (static_cast<float>(round) * 2.39996F) + dice.around(0.3F);
                const float lean = level.angle * (1.0F + dice.around(0.15F));
                shoots.push_back(
                    {.origin = ring.centre,
                     .frame = glm::normalize(ring.frame * about_z(radial) * about_x(lean)),
                     .length = next.length * (1.0F + dice.around(next.sway)) *
                               (1.0F - (level.fall * along)),
                     .radius = std::min(ring.radius * next.radius, ring.radius * 0.9F),
                     .level = shoot.level + 1});
            }
        } else {
            lay_frond(species, bough, shoot, dice, tree);
        }
        const bool bearing =
            shoot.level == 0 && (species.fruit.count > 0 || species.fruit.fallen > 0);
        const Ring under = bearing ? between(bough, level.start) : Ring{};
        tree.boughs.push_back(std::move(bough));
        if (bearing) {
            hang_fruit(species.fruit, under, dice, tree);
        }
    }
    return tree;
}

} // namespace hog::flora

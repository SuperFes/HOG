#include <hog/core/portable_maths.hpp>
#include <hog/formation/shores.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

namespace hog::formation {

namespace {

// Lengths are reckoned at Earth's scale, as the rivers' are.
constexpr double earth_radius = 6'371'000.0;
// A storm's sea grows with its fetch until it's fully grown, against its wind's speed
// (Hasselmann et al. 1973; Pierson and Moskowitz 1964).
constexpr double growth = 0.0016;
constexpr double grown = 0.2433;
// A fully grown sea peaks at 0.877 g / U radians a second, so its waves are this steep, height over
// deep water wavelength.
constexpr double grown_steepness = grown * 0.877 * 0.877 / (2.0 * std::numbers::pi);
// Ways about a coast that waves are looked for coming in along.
constexpr int ways = 16;
// Steps along the shore what's carried is followed before the rest is laid where it's got to.
constexpr int drift_steps = 64;
// Stones move only in the biggest storms, and go that much less far along the shore than sand.
constexpr double stones_slower = 4.0;
constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

double smoothstep(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// The share of what goes `distance` metres that's still there, most gone by `reach`; none ever
// goes for a reach of 0.
double lasting(double distance, double reach) {
    return reach > 0.0 ? maths::exp(-distance / reach) : 1.0;
}

bool in_sea(const Rivers& rivers, std::uint32_t point) {
    return rivers.receiver[point] == point;
}

DVec3 along_surface(const DVec3& vector, const DVec3& here) {
    return vector - (glm::dot(vector, here) * here);
}

// Metres of open water from a coast point the way `way` goes, until land or a fully grown sea.
double fetch_toward(const Sphere& sphere, const Rivers& rivers, std::uint32_t from,
                    const DVec3& way, double longest) {
    const DVec3 here = sphere.point(from);
    std::uint32_t near = from;
    double arc = 0.0;
    while (arc * earth_radius < longest) {
        const double ahead = arc + sphere.spacing();
        near = sphere.nearest((here * maths::cos(ahead)) + (way * maths::sin(ahead)), near);
        if (!in_sea(rivers, near)) {
            break;
        }
        arc = ahead;
    }
    return std::min(arc * earth_radius, longest);
}

struct Waves {
    double height = 0.0;
    double usual = 0.0;
    DVec3 coming{0.0}; // the way their power comes in, as long as its share of the whole
};

// The waves storms send in on a coast point from every way open water lies: the wind's own sea
// where it blows onshore, swell from elsewhere.
Waves waves_at(const Sphere& sphere, const Climate& climate, const Rivers& rivers, const Ice& ice,
               std::uint32_t point, const Body& body) {
    const DVec3 here = sphere.point(point);
    const DVec3 axis = std::abs(here.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(axis, here));
    const DVec3 north = glm::cross(here, east);
    const double longest =
        (grown / growth) * (grown / growth) * body.storm_wind * body.storm_wind / body.gravity;
    Waves waves;
    double power = 0.0;
    for (int at = 0; at < ways; ++at) {
        const double round = 2.0 * std::numbers::pi * at / ways;
        const DVec3 way = (east * maths::cos(round)) + (north * maths::sin(round));
        const double fetch = fetch_toward(sphere, rivers, point, way, longest);
        if (fetch <= 0.0) {
            continue;
        }
        const double onshore = std::max(glm::dot(climate.wind[point], -way), 0.0);
        const double share = body.swell_share + ((1.0 - body.swell_share) * onshore);
        const double height = wave_height(fetch, body) * share;
        waves.height = std::max(waves.height, height);
        waves.usual = std::max(waves.usual, wave_height(fetch, body.usual_wind, body) * share);
        waves.coming -= height * height * way;
        power += height * height;
    }
    if (power > 0.0) {
        waves.coming /= power;
    }
    // Sea ice lying on it keeps the waves off for its share of the year.
    waves.height *= 1.0 - ice.sea_ice[point];
    waves.usual *= 1.0 - ice.sea_ice[point];
    return waves;
}

// What reaches the shore, cubic metres a year by kind.
struct Kinds {
    double quartz = 0.0;
    double volcanic = 0.0;
    double carbonate = 0.0;
    double stones = 0.0;      // of the light crust's rock
    double dark_stones = 0.0; // of lava and basalt
    double mud = 0.0;
};

// Of what the rivers bring down to each sea point, the share that's sand, and of that the share
// worn off lava and basalt.
struct RiverSand {
    std::vector<double> sand;
    std::vector<double> volcanic;
};

// From the rock each point up the rivers wears: they drop the sand first where they slow, so what
// they bring the sea is the sand worn less the sand they've laid on the way.
RiverSand river_sand(const Sphere& sphere, const Rivers& rivers, const Bedrock& bedrock,
                     const Outcrop& outcrop, const Sediment& sediment, const Body& body) {
    const std::uint32_t size = sphere.size();
    std::vector<double> volcanic(size, 0.0);
    std::vector<double> grains(size, 0.0);
    std::vector<double> worn_all(size, 0.0);
    std::vector<double> dropped(size, 0.0);
    std::vector<std::uint32_t> inflow(size, 0);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (!in_sea(rivers, point)) {
            ++inflow[rivers.receiver[point]];
        }
    }
    std::vector<std::uint32_t> order;
    order.reserve(size);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (!in_sea(rivers, point) && inflow[point] == 0) {
            order.push_back(point);
        }
    }
    for (std::size_t at = 0; at < order.size(); ++at) {
        const std::uint32_t point = order[at];
        const Rock rock = outcrop.rock[point];
        const bool lava = rock == Rock::Lava || rock == Rock::SeaFloor;
        // Layered rock's soluble share goes off in the water, not as grains.
        const bool beds = rock == Rock::Layered || rock == Rock::Folded;
        const double worn = rivers.wear[point] * sphere.area(point) * earth_radius * earth_radius;
        const double grain = worn * (beds ? 1.0 - bedrock.soluble[point] : 1.0);
        grains[point] += grain;
        worn_all[point] += worn;
        volcanic[point] += lava ? grain : 0.0;
        dropped[point] += sediment.settling[point];
        const std::uint32_t down = rivers.receiver[point];
        // Glassy grains break down to mud as they go.
        const double hop =
            std::acos(std::clamp(glm::dot(sphere.point(point), sphere.point(down)), -1.0, 1.0)) *
            earth_radius;
        const double broken = volcanic[point] * (1.0 - lasting(hop, body.volcanic_reach));
        grains[down] += grains[point] - broken;
        worn_all[down] += worn_all[point];
        volcanic[down] += volcanic[point] - broken;
        dropped[down] += dropped[point];
        if (!in_sea(rivers, down) && --inflow[down] == 0) {
            order.push_back(down);
        }
    }
    // What comes down from no worn rock, such as ice's outwash, is as sandy as rock is.
    RiverSand arriving{.sand = std::vector<double>(size, body.rock_sand),
                       .volcanic = std::move(volcanic)};
    for (std::uint32_t point = 0; point < size; ++point) {
        const double brought = worn_all[point] - dropped[point];
        if (in_sea(rivers, point) && brought > 0.0) {
            const double sand =
                (body.rock_sand * grains[point]) - (body.sand_share * dropped[point]);
            arriving.sand[point] = std::clamp(sand / brought, 0.0, 1.0);
        }
        arriving.volcanic[point] =
            grains[point] > 0.0 ? arriving.volcanic[point] / grains[point] : 0.0;
    }
    return arriving;
}

// What `volume` cubic metres of the land's edge at `point` give the shore once the waves have
// broken them up: stones as much as the rock holds together, the rest sand and mud.
Kinds worn_off(const Bedrock& bedrock, const Outcrop& outcrop, const Ice& ice, std::uint32_t point,
               double strength, double volume, const Body& body) {
    if (ice.till[point] > 0.0) {
        const double fines = volume * (1.0 - body.till_stones);
        const double sand = fines * body.sand_share / 3.0;
        return {.quartz = sand, .stones = volume * body.till_stones, .mud = fines - sand};
    }
    const Rock rock = outcrop.rock[point];
    if (rock == Rock::Lava || rock == Rock::SeaFloor) {
        return {.volcanic = volume * (1.0 - strength), .dark_stones = volume * strength};
    }
    if (rock == Rock::Basement) {
        return {.quartz = volume * (1.0 - strength), .stones = volume * strength};
    }
    const double grains = volume * (1.0 - bedrock.soluble[point]);
    const double fines = grains * (1.0 - strength);
    return {.quartz = fines * body.sand_share,
            .stones = grains * strength,
            .mud = fines * (1.0 - body.sand_share)};
}

// The points just out to sea from a coast, the line the drift runs along.
struct Shore {
    std::vector<std::uint32_t> points;
    std::vector<std::uint32_t> index; // each point's place in the above, none off the shore
    std::vector<DVec3> drift; // the way and strength, 0 to 1, the waves drive what's on it along
    double length = 0.0;      // metres of coast each point holds
};

Shore shore_of(const Sphere& sphere, const Rivers& rivers) {
    Shore shore;
    shore.index.assign(sphere.size(), none);
    shore.length = sphere.spacing() * earth_radius;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const bool coast = in_sea(rivers, point) &&
                           std::ranges::any_of(sphere.neighbors(point), [&](std::uint32_t near) {
                               return !in_sea(rivers, near);
                           });
        if (coast) {
            shore.index[point] = static_cast<std::uint32_t>(shore.points.size());
            shore.points.push_back(point);
        }
    }
    shore.drift.assign(shore.points.size(), DVec3{0.0});
    return shore;
}

// The way waves coming in as `coming` drive what's on the shore along it, strongest where they
// come in at 45° to it: sin 2θ of that angle, by their share coming in one way at all.
DVec3 drift_of(const Sphere& sphere, const Rivers& rivers, std::uint32_t point,
               const DVec3& coming) {
    const DVec3 here = sphere.point(point);
    DVec3 seaward{0.0};
    for (const std::uint32_t near : sphere.neighbors(point)) {
        if (!in_sea(rivers, near)) {
            seaward += glm::normalize(here - sphere.point(near));
        }
    }
    seaward = along_surface(seaward, here);
    const double strength = glm::length(coming);
    if (glm::length(seaward) <= 0.0 || strength <= 0.0) {
        return DVec3{0.0};
    }
    seaward = glm::normalize(seaward);
    const DVec3 across = glm::dot(coming, seaward) * seaward;
    const DVec3 along = along_surface(coming, here) - across;
    const double sideways = glm::length(along);
    if (sideways <= 0.0) {
        return DVec3{0.0};
    }
    return (2.0 * sideways * glm::length(across) / strength) * along / sideways;
}

// The shares of what goes on from a shore point to each shore point beside it: along the drift
// where it runs, else either way alike. Empty where it has none.
struct Onward {
    std::vector<std::uint32_t> to; // places on the shore
    std::vector<double> share;
};

void onward_from(const Sphere& sphere, const Shore& shore, std::size_t at, Onward& onward) {
    onward.to.clear();
    onward.share.clear();
    const std::uint32_t point = shore.points[at];
    const DVec3 here = sphere.point(point);
    const DVec3 drift = shore.drift[at];
    double downdrift = 0.0;
    for (const std::uint32_t near : sphere.neighbors(point)) {
        if (shore.index[near] != none) {
            const double along =
                std::max(glm::dot(drift, glm::normalize(sphere.point(near) - here)), 0.0);
            onward.to.push_back(shore.index[near]);
            onward.share.push_back(along);
            downdrift += along;
        }
    }
    const double strength = downdrift > 0.0 ? glm::length(drift) : 0.0;
    const double spread = (1.0 - strength) / static_cast<double>(onward.to.size());
    for (double& share : onward.share) {
        share = spread + (strength > 0.0 ? strength * share / downdrift : 0.0);
    }
}

// How far what's put on the shore goes and what becomes of it on the way: going on a step `reach`
// metres before most has stayed, breaking down to what the sea carries off over `wearing` metres
// (never, for 0); where it stays, the share `kept` of it lies, the rest washed out to sea.
struct Drifting {
    double reach = 0.0;
    double wearing = 0.0;
    const std::vector<double>* kept = nullptr;
};

// Where what's put on the shore each year ends up, followed step by step along it. `passing`, if
// given, gathers all that goes by each point.
std::vector<double> drift_along(const Sphere& sphere, const Shore& shore,
                                const std::vector<double>& put, const Drifting& drifting,
                                std::vector<double>* passing) {
    const std::size_t count = shore.points.size();
    const std::vector<double>& kept = *drifting.kept;
    const double going = drifting.reach > 0.0 ? maths::exp(-shore.length / drifting.reach) : 0.0;
    const double lasts = lasting(shore.length, drifting.wearing);
    std::vector<double> carried = put;
    std::vector<double> laid(count, 0.0);
    std::vector<double> next(count, 0.0);
    Onward onward;
    for (int step = 0; step < drift_steps; ++step) {
        std::ranges::fill(next, 0.0);
        for (std::size_t at = 0; at < count; ++at) {
            const double amount = carried[at];
            if (amount <= 0.0) {
                continue;
            }
            if (passing != nullptr) {
                (*passing)[at] += amount;
            }
            onward_from(sphere, shore, at, onward);
            const double on = onward.to.empty() ? 0.0 : going;
            laid[at] += amount * (1.0 - on) * kept[at];
            for (std::size_t way = 0; way < onward.to.size(); ++way) {
                next[onward.to[way]] += amount * on * lasts * onward.share[way];
            }
        }
        std::swap(carried, next);
    }
    for (std::size_t at = 0; at < count; ++at) {
        laid[at] += carried[at] * kept[at];
    }
    return laid;
}

void add(Kinds& to, const Kinds& more) {
    to.quartz += more.quartz;
    to.volcanic += more.volcanic;
    to.carbonate += more.carbonate;
    to.stones += more.stones;
    to.dark_stones += more.dark_stones;
    to.mud += more.mud;
}

// The rivers' load at their mouths.
void bring_rivers(const Sphere& sphere, const Rivers& rivers, const Bedrock& bedrock,
                  const Outcrop& outcrop, const Sediment& sediment, const Body& body,
                  const Shore& shore, std::vector<Kinds>& put) {
    const RiverSand arriving = river_sand(sphere, rivers, bedrock, outcrop, sediment, body);
    for (std::size_t at = 0; at < shore.points.size(); ++at) {
        const std::uint32_t point = shore.points[at];
        const double brought = sediment.settling[point];
        const double sand = brought * arriving.sand[point];
        put[at].volcanic += sand * arriving.volcanic[point];
        put[at].quartz += sand * (1.0 - arriving.volcanic[point]);
        put[at].mud += brought - sand;
    }
}

// The land's edge the waves wear back, shared among the shore points it faces; how fast each
// land point's edge goes back into `retreat`.
void wear_coasts(const Sphere& sphere, const Rivers& rivers, const Bedrock& bedrock,
                 const Outcrop& outcrop, const Ice& ice, const Body& body, const Shore& shore,
                 Shores& shores, std::vector<Kinds>& put) {
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        int facing = 0;
        double waves = 0.0;
        for (const std::uint32_t near : sphere.neighbors(point)) {
            if (shore.index[near] != none) {
                ++facing;
                waves = std::max(waves, shores.waves[near]);
            }
        }
        // Where the land's ice comes down to the sea, it's the ice that breaks off.
        if (in_sea(rivers, point) || facing == 0 || ice.thickness[point] > 0.0) {
            continue;
        }
        const double strength = ice.till[point] > 0.0
                                    ? 0.0
                                    : rock_strength(outcrop.rock[point], bedrock.layering[point],
                                                    bedrock.soluble[point]);
        const double retreat =
            body.cliff_retreat * waves * waves * maths::pow(10.0, -body.cliff_hardness * strength);
        shores.retreat[point] = retreat;
        // Worn back into ground rising inland, the face grows as tall as the ground has risen
        // over what's been worn away; over the shore's years, half that on the whole, over a
        // beach's height.
        const double rising = std::max(rivers.height[point], 0.0) / (shore.length / 2.0);
        const double face = 2.0 + (0.5 * rising * retreat * body.shore_years);
        const Kinds worn = worn_off(bedrock, outcrop, ice, point, strength,
                                    retreat * face * shore.length / facing, body);
        for (const std::uint32_t near : sphere.neighbors(point)) {
            if (shore.index[near] != none) {
                add(put[shore.index[near]], worn);
            }
        }
    }
}

// Coral grows in warm, shallow, clear water, shells in any open sea.
void grow_shells(const Climate& climate, const Rivers& rivers, const Ice& ice, const Body& body,
                 const Shore& shore, const std::vector<double>& mud_passing, Shores& shores,
                 std::vector<Kinds>& put) {
    for (std::size_t at = 0; at < shore.points.size(); ++at) {
        const std::uint32_t point = shore.points[at];
        const double warm =
            smoothstep(body.reef_warmth - 5.0, body.reef_warmth, climate.temperature[point]);
        const double shallow =
            1.0 - smoothstep(body.shelf_depth, 2.0 * body.shelf_depth, -rivers.height[point]);
        const double clear =
            1.0 - smoothstep(0.0, body.muddiest_reef, mud_passing[at] / shore.length);
        const double open = 1.0 - ice.sea_ice[point];
        const double reef = warm * shallow * clear * open;
        shores.reef[point] = reef;
        put[at].carbonate += ((reef * body.reef_sand) + (body.shell_sand * open)) * shore.length;
    }
}

// What lies on the shore after drifting along it, cubic metres a year by kind.
struct Laid {
    std::vector<double> quartz;
    std::vector<double> volcanic;
    std::vector<double> carbonate;
    std::vector<double> shingle;
};

Laid lay_beaches(const Sphere& sphere, const Shore& shore, const std::vector<Kinds>& put,
                 const Body& body) {
    const std::size_t count = shore.points.size();
    const auto kind = [&](double Kinds::* member) {
        std::vector<double> amounts(count, 0.0);
        for (std::size_t at = 0; at < count; ++at) {
            amounts[at] = put[at].*member;
        }
        return amounts;
    };
    const std::vector<double> all(count, 1.0);
    const auto sand = [&](double Kinds::* member, double wearing) {
        return drift_along(sphere, shore, kind(member),
                           {.reach = body.drift_reach, .wearing = wearing, .kept = &all}, nullptr);
    };
    const auto stones = [&](double Kinds::* member) {
        return drift_along(sphere, shore, kind(member),
                           {.reach = body.drift_reach / stones_slower, .kept = &all}, nullptr);
    };
    Laid laid{.quartz = sand(&Kinds::quartz, 0.0),
              .volcanic = sand(&Kinds::volcanic, body.volcanic_reach),
              .carbonate = sand(&Kinds::carbonate, 0.0),
              .shingle = std::vector<double>(count, 0.0)};
    const std::vector<double> pale_stones = stones(&Kinds::stones);
    const std::vector<double> dark_stones = stones(&Kinds::dark_stones);
    // The surf grinds the stones lying on a beach to sand of their rock: of those it's been
    // brought evenly over the shore's years, the share still stones.
    const double lives = body.shore_years / std::max(body.stone_life, 1.0);
    const double left = lives > 0.0 ? (1.0 - maths::exp(-lives)) / lives : 1.0;
    for (std::size_t at = 0; at < count; ++at) {
        laid.shingle[at] = (pale_stones[at] + dark_stones[at]) * left;
        laid.quartz[at] += pale_stones[at] * (1.0 - left);
        laid.volcanic[at] += dark_stones[at] * (1.0 - left);
    }
    return laid;
}

// The land's side of the waterline has what lies on the shore it faces.
void share_ashore(const Sphere& sphere, const Rivers& rivers, const Shore& shore, Shores& shores) {
    const std::array made_of{&shores.quartz, &shores.volcanic, &shores.carbonate, &shores.shingle};
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (in_sea(rivers, point)) {
            continue;
        }
        int facing = 0;
        double beach = 0.0;
        for (const std::uint32_t near : sphere.neighbors(point)) {
            if (shore.index[near] == none) {
                continue;
            }
            ++facing;
            const double there = shores.beach[near];
            beach += there;
            shores.waves[point] = std::max(shores.waves[point], shores.waves[near]);
            shores.usual[point] = std::max(shores.usual[point], shores.usual[near]);
            shores.mud[point] += shores.mud[near];
            shores.reef[point] = std::max(shores.reef[point], shores.reef[near]);
            for (auto* share : made_of) {
                (*share)[point] += (*share)[near] * there;
            }
        }
        if (facing == 0) {
            continue;
        }
        shores.mud[point] /= facing;
        shores.beach[point] = beach / facing;
        for (auto* share : made_of) {
            (*share)[point] = beach > 0.0 ? (*share)[point] / beach : 0.0;
        }
    }
}

} // namespace

double wave_height(double fetch, const Body& body) {
    return wave_height(fetch, body.storm_wind, body);
}

double wave_height(double fetch, double wind, const Body& body) {
    return std::min(growth * wind * std::sqrt(std::max(fetch, 0.0) / body.gravity),
                    grown * wind * wind / body.gravity);
}

double wave_period(double height, const Body& body) {
    return std::sqrt(2.0 * std::numbers::pi * std::max(height, 0.0) /
                     (grown_steepness * body.gravity));
}

Shores shores_on(const Sphere& sphere, const Climate& climate, const Rivers& rivers,
                 const Bedrock& bedrock, const Outcrop& outcrop, const Ice& ice,
                 const Sediment& sediment, const Body& body) {
    Shores shores;
    for (auto* field :
         {&shores.waves, &shores.usual, &shores.beach, &shores.quartz, &shores.volcanic,
          &shores.carbonate, &shores.shingle, &shores.mud, &shores.reef, &shores.retreat}) {
        field->assign(sphere.size(), 0.0);
    }
    Shore shore = shore_of(sphere, rivers);
    const std::size_t count = shore.points.size();
    for (std::size_t at = 0; at < count; ++at) {
        const std::uint32_t point = shore.points[at];
        const Waves waves = waves_at(sphere, climate, rivers, ice, point, body);
        shores.waves[point] = waves.height;
        shores.usual[point] = waves.usual;
        shore.drift[at] = drift_of(sphere, rivers, point, waves.coming);
    }

    std::vector<Kinds> put(count);
    bring_rivers(sphere, rivers, bedrock, outcrop, sediment, body, shore, put);
    wear_coasts(sphere, rivers, bedrock, outcrop, ice, body, shore, shores, put);

    // Mud lies the less, the more power the waves stirring it have, their height squared.
    std::vector<double> sheltered(count, 0.0);
    std::vector<double> mud_put(count, 0.0);
    for (std::size_t at = 0; at < count; ++at) {
        const double stirring = shores.waves[shore.points[at]] / body.stirring_waves;
        sheltered[at] = 1.0 / (1.0 + (stirring * stirring));
        mud_put[at] = put[at].mud;
    }
    std::vector<double> mud_passing(count, 0.0);
    const std::vector<double> mud = drift_along(
        sphere, shore, mud_put, {.reach = body.drift_reach, .kept = &sheltered}, &mud_passing);
    grow_shells(climate, rivers, ice, body, shore, mud_passing, shores, put);
    const Laid laid = lay_beaches(sphere, shore, put, body);

    const double per_metre = body.shore_years / shore.length;
    for (std::size_t at = 0; at < count; ++at) {
        const std::uint32_t point = shore.points[at];
        const double beach =
            laid.quartz[at] + laid.volcanic[at] + laid.carbonate[at] + laid.shingle[at];
        shores.beach[point] = beach * per_metre;
        shores.mud[point] = mud[at] * per_metre;
        if (beach > 0.0) {
            shores.quartz[point] = laid.quartz[at] / beach;
            shores.volcanic[point] = laid.volcanic[at] / beach;
            shores.carbonate[point] = laid.carbonate[at] / beach;
            shores.shingle[point] = laid.shingle[at] / beach;
        }
    }
    share_ashore(sphere, rivers, shore, shores);
    return shores;
}

} // namespace hog::formation

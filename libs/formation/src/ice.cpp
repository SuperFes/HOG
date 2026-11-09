#include <hog/core/portable_maths.hpp>
#include <hog/formation/ice.hpp>
#include <hog/formation/sediment.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <queue>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace hog::formation {

namespace {

constexpr double days_a_year = 365.25;
constexpr double water_density = 1'000.0;
constexpr int most_rounds = 8;

double angle(const DVec3& a, const DVec3& b) noexcept {
    return maths::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

double asin(double x) noexcept {
    return maths::atan2(x, std::sqrt(std::max(0.0, 1.0 - (x * x))));
}

double acos(double x) noexcept {
    return maths::atan2(std::sqrt(std::max(0.0, 1.0 - (x * x))), x);
}

bool in_sea(const Rivers& rivers, std::uint32_t point) {
    return rivers.receiver[point] == point;
}

// Land points from the lowest up. A river's heights rise upstream, so each point comes after the
// one its water goes to.
std::vector<std::uint32_t> lowest_first(const Rivers& rivers) {
    std::vector<std::uint32_t> order;
    for (std::uint32_t point = 0; point < rivers.receiver.size(); ++point) {
        if (!in_sea(rivers, point)) {
            order.push_back(point);
        }
    }
    std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
        return rivers.height[a] < rivers.height[b];
    });
    return order;
}

// What ice is laid on and flows over: the rock, the ways down it, and the climate over it.
struct Land {
    const Sphere* sphere;
    const Crust* crust;
    const Climate* climate;
    const Rivers* rivers;
    const Body* body;
    std::vector<std::uint32_t> upward; // land points, each after the one downstream of it
};

// Ice lying under one climate.
struct Cover {
    std::vector<double> thickness;
    std::vector<double> surface;
    std::vector<double> balance;
    std::vector<double> reaching; // cubic metres of water a year arriving as ice from upstream
    std::vector<double> flux;
    std::vector<std::uint8_t> iced;
    std::vector<std::uint32_t> parent; // the point down the ice's top each iced point stands on
    std::vector<std::uint32_t> order;  // iced points, each after its parent
    double calving = 0.0;
};

// Snow's gain over melt at each land point, `cooling` colder than the climate, the air as cold as
// the ice's top makes it.
void balance(const Land& land, double cooling, Cover& cover) {
    const Body& body = *land.body;
    for (const std::uint32_t point : land.upward) {
        const double latitude = asin(std::clamp(land.sphere->point(point).z, -1.0, 1.0));
        const double swing = seasonal_swing(latitude, false, body);
        const double raised = cover.surface[point] - std::max(land.crust->base[point], 0.0);
        const double celsius =
            land.climate->temperature[point] - cooling - (body.lapse_rate * raised) - body.freezing;
        const double rain = std::max(land.climate->rain[point], 0.0);
        cover.balance[point] = (rain * cold_share(celsius, swing)) -
                               (body.melt_rate * degree_days(celsius, swing)) -
                               sublimated(celsius + body.freezing, rain, body);
    }
}

// The ice's top, or the land's or the sea's where there's no ice.
double top(const Land& land, const Cover& cover, std::uint32_t point) {
    return in_sea(*land.rivers, point) ? 0.0 : cover.surface[point];
}

// Land points from the ice's top down.
std::vector<std::uint32_t> downhill(const Land& land, const Cover& cover) {
    std::vector<std::uint32_t> downward = land.upward;
    std::ranges::sort(downward, [&](std::uint32_t a, std::uint32_t b) {
        return cover.surface[a] > cover.surface[b];
    });
    return downward;
}

// Shares `amount` leaving a point among the points below it on the ice's top, by how steeply
// each lies, so a sheet spreads what it carries and valleys gather it. A hollow in the surface
// the ice hasn't filled yet empties the way the water would.
template <typename Pass>
void share_down(const Land& land, const Cover& cover, std::uint32_t point, double amount,
                const Pass& pass) {
    const Sphere& sphere = *land.sphere;
    const double here = cover.surface[point];
    const auto steepness = [&](std::uint32_t next) {
        const double drop = here - top(land, cover, next);
        return drop > 0.0 ? drop / angle(sphere.point(point), sphere.point(next)) : 0.0;
    };
    double steepest = 0.0;
    for (const std::uint32_t next : sphere.neighbors(point)) {
        steepest += steepness(next);
    }
    if (steepest <= 0.0) {
        pass(land.rivers->receiver[point], amount);
        return;
    }
    for (const std::uint32_t next : sphere.neighbors(point)) {
        pass(next, amount * steepness(next) / steepest);
    }
}

// Ice gathered down its own surface from the highest: each point passes on what reaches it and
// what its snow adds, less what melts. What reaches the sea breaks off.
void gather(const Land& land, Cover& cover) {
    const Rivers& rivers = *land.rivers;
    const double cell_area = land.body->radius * land.body->radius;
    std::ranges::fill(cover.reaching, 0.0);
    cover.calving = 0.0;
    const auto pass = [&](std::uint32_t to, double amount) {
        if (in_sea(rivers, to)) {
            cover.calving += amount;
        } else {
            cover.reaching[to] += amount;
        }
    };
    for (const std::uint32_t point : downhill(land, cover)) {
        const double gained = cover.balance[point] * land.sphere->area(point) * cell_area;
        cover.iced[point] = cover.reaching[point] > 0.0 || gained > 0.0 ? 1 : 0;
        cover.flux[point] = std::max(0.0, cover.reaching[point] + gained);
        if (cover.flux[point] > 0.0) {
            share_down(land, cover, point, cover.flux[point], pass);
        }
    }
}

// Inwards from where the ice ends, lowest first, each iced point as thick as plastic ice stands
// over `rock` on the shortest way out to its edge: Nye's ice, its top falling steepest towards the
// margin as ice flows. Over water filling a hollow the ice floats, standing on the water.
void stand(const Land& land, std::span<const double> rock, Cover& cover) {
    const Sphere& sphere = *land.sphere;
    const Rivers& rivers = *land.rivers;
    const std::uint32_t size = sphere.size();
    const auto iced = [&](std::uint32_t point) {
        return !in_sea(rivers, point) && cover.iced[point] != 0;
    };
    const auto bed = [&](std::uint32_t point) {
        return rock[point] + (rivers.height[point] - rivers.ground[point]);
    };
    using Entry = std::pair<double, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    std::vector<double> best(size, std::numeric_limits<double>::infinity());
    std::vector<std::uint8_t> fixed(size, 0);
    for (const std::uint32_t point : land.upward) {
        if (!iced(point)) {
            cover.thickness[point] = 0.0;
            cover.surface[point] = bed(point);
        }
    }
    for (std::uint32_t point = 0; point < size; ++point) {
        if (iced(point)) {
            continue;
        }
        const auto edge = std::ranges::any_of(sphere.neighbors(point), iced);
        if (edge) {
            best[point] = top(land, cover, point);
            open.emplace(best[point], point);
        }
    }
    cover.order.clear();
    while (!open.empty()) {
        const auto [level, point] = open.top();
        open.pop();
        if (fixed[point] != 0) {
            continue;
        }
        fixed[point] = 1;
        if (iced(point)) {
            cover.order.push_back(point);
        }
        for (const std::uint32_t next : sphere.neighbors(point)) {
            if (fixed[next] != 0 || !iced(next)) {
                continue;
            }
            const double length =
                angle(sphere.point(point), sphere.point(next)) * land.body->radius;
            const double surface = bed(next) + ice_thickness(bed(next), level, length, *land.body);
            if (surface < best[next]) {
                best[next] = surface;
                cover.parent[next] = point;
                open.emplace(surface, next);
            }
        }
    }
    for (const std::uint32_t point : cover.order) {
        cover.surface[point] = best[point];
        cover.thickness[point] = best[point] - bed(point);
    }
}

// Ice under a climate `cooling` colder, on `rock`: the ice raises its own top into colder air, so
// it's worked out again until where it lies stops changing.
Cover cover_on(const Land& land, std::span<const double> rock, double cooling) {
    const std::size_t size = land.sphere->size();
    Cover cover{.thickness = std::vector<double>(size, 0.0),
                .surface = std::vector<double>(rock.begin(), rock.end()),
                .balance = std::vector<double>(size, 0.0),
                .reaching = std::vector<double>(size, 0.0),
                .flux = std::vector<double>(size, 0.0),
                .iced = std::vector<std::uint8_t>(size, 0),
                .parent = std::vector<std::uint32_t>(size, 0),
                .order = {}};
    for (int round = 0; round < most_rounds; ++round) {
        const std::vector<std::uint8_t> before = cover.iced;
        balance(land, cooling, cover);
        gather(land, cover);
        stand(land, rock, cover);
        if (round > 0 && cover.iced == before) {
            break;
        }
    }
    return cover;
}

// Rock the coldest ice carved away over the time it lay. Carving goes as the ice's sliding speed
// q / h to a power l, and deepens the ice as it goes, slowing it: dh/dt = K (q/h)^l, so
// h^(l+1) grows by (l+1) K q^l t. The flow is reckoned across the whole of each point's width;
// gathering it into troughs narrower than that is left to finer detail. Never deeper under the bed
// down-ice than the overdeepening allows, the sea's level standing as a sill where ice meets it.
std::vector<double> carve(const Land& land, const Cover& cold) {
    const Body& body = *land.body;
    const Rivers& rivers = *land.rivers;
    const double power = body.glacial_erosion_power + 1.0;
    const double across = land.sphere->spacing() * body.radius;
    std::vector<double> rock = rivers.ground;
    std::vector<double> carved(rock.size(), 0.0);
    for (const std::uint32_t point : cold.order) {
        // Ice floating on water filling a hollow doesn't touch the rock under it.
        if (rivers.height[point] - rivers.ground[point] > body.shallowest_lake) {
            continue;
        }
        const double through =
            std::max(cold.reaching[point], cold.flux[point]) * (water_density / body.ice_density);
        const double first = std::max(cold.thickness[point], 1.0);
        const double deepened = maths::pow(
            maths::pow(first, power) +
                (power * body.glacial_erosion *
                 maths::pow(through / across, body.glacial_erosion_power) * body.glaciated_years),
            1.0 / power);
        const double cut = deepened - first;
        const std::uint32_t down = cold.parent[point];
        const bool sea = in_sea(rivers, down);
        const double sill = sea ? 0.0 : rock[down];
        const double ice_below = sea ? 0.0 : std::max(0.0, cold.surface[down] - rock[down]);
        const double length =
            angle(land.sphere->point(point), land.sphere->point(down)) * body.radius;
        const double lowest = sill - deepest_dip(ice_below, length, body);
        const double ground = rivers.ground[point];
        rock[point] = std::max(ground - cut, std::min(ground, lowest));
        carved[point] = ground - rock[point];
    }
    return carved;
}

// Rock the ice carved, carried in it down its surface and let go as it melts away: each point
// lets go of the share of its load that it loses of its ice. Meltwater, gathered down the ice as
// it melts, washes off what it can carry as outwash for the rivers; the rest lies as a sheet of
// till through the melting reaches, and a moraine where the ice ends. Whatever reaches the sea
// settles on its floor.
void lay(const Land& land, const Cover& cold, Ice& ice) {
    const Sphere& sphere = *land.sphere;
    const Rivers& rivers = *land.rivers;
    const double cell_area = land.body->radius * land.body->radius;
    const double years = std::max(land.body->glaciated_years, 1.0);
    const std::uint32_t size = sphere.size();
    std::vector<double> carrying(size, 0.0); // cubic metres of rock a year arriving in the ice
    std::vector<double> water(size, 0.0);    // cubic metres of meltwater a year arriving
    std::vector<double> laid(size, 0.0);
    std::vector<double> washed(size, 0.0);
    std::vector<std::uint8_t> done(size, 0);
    for (const std::uint32_t point : downhill(land, cold)) {
        done[point] = 1;
        const double area = sphere.area(point) * cell_area;
        const double load = carrying[point] + (ice.carved[point] * area / years);
        const double gained = cold.balance[point] * area;
        const double arriving = cold.reaching[point] + std::max(gained, 0.0);
        const double kept =
            arriving > 0.0 ? std::clamp(cold.flux[point] / arriving, 0.0, 1.0) : 0.0;
        water[point] += std::max(0.0, arriving - cold.flux[point]);
        const double dropped = load * (1.0 - kept);
        const std::uint32_t down = rivers.receiver[point];
        const double below = in_sea(rivers, down) ? 0.0 : rivers.height[down];
        const double slope = std::max(0.0, rivers.height[point] - below) /
                             (angle(sphere.point(point), sphere.point(down)) * land.body->radius);
        // Meltwater runs under the ice in a few channels and sweeps up what melts out only where it
        // comes out from under the ice, at its edge.
        const bool edge = cold.flux[point] <= 0.0;
        washed[point] =
            edge ? std::min(dropped, carrying_capacity(water[point], slope, *land.body)) : 0.0;
        // Till heaps no higher than the ice's top: past that the ice rides over it and takes the
        // rest on, or at its edge leaves it to the meltwater.
        const double rock = rivers.ground[point] - ice.carved[point];
        const double room =
            std::max(0.0, (std::max(0.0, cold.surface[point] - rock) * area / years) - laid[point]);
        const double lying = std::min(dropped - washed[point], room);
        laid[point] += lying;
        double going = load * kept;
        if (edge) {
            washed[point] += dropped - washed[point] - lying;
        } else {
            going += dropped - washed[point] - lying;
        }
        const double flowing = water[point];
        if (going <= 0.0 && flowing <= 0.0) {
            continue;
        }
        const double total = going + flowing;
        share_down(land, cold, point, total, [&](std::uint32_t to, double amount) {
            const double rock = amount * going / total;
            if (in_sea(rivers, to)) {
                laid[to] += rock;
            } else if (done[to] != 0) {
                laid[point] += rock;
            } else {
                carrying[to] += rock;
                water[to] += amount * flowing / total;
            }
        });
    }
    ice.till.assign(size, 0.0);
    ice.outwash.assign(size, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        ice.till[point] = laid[point] * years / (sphere.area(point) * cell_area);
        ice.outwash[point] = washed[point] * years;
    }
}

} // namespace

double degree_days(double mean, double swing) {
    if (swing <= 0.0 || mean >= swing) {
        return days_a_year * std::max(mean, 0.0);
    }
    if (mean <= -swing) {
        return 0.0;
    }
    const double thawed = acos(-mean / swing);
    return days_a_year / std::numbers::pi *
           ((mean * thawed) + std::sqrt((swing * swing) - (mean * mean)));
}

double sublimated(double celsius, double rain, const Body& body) {
    // What the air could take as it's colder: Magnus's vapour pressure over ice, against
    // freezing's. It's the drier the less of that comes back as snow.
    const double below = std::min(celsius, 0.0);
    const double could = body.sublimation * maths::exp(22.46 * below / (272.62 + below));
    return could * could / (could + std::max(rain, 0.0));
}

double cold_share(double mean, double swing) {
    if (swing <= 0.0) {
        return mean < 0.0 ? 1.0 : 0.0;
    }
    return 1.0 - (acos(std::clamp(-mean / swing, -1.0, 1.0)) / std::numbers::pi);
}

double seasonal_swing(double latitude, bool sea, const Body& body) {
    const double earth_tilt = 23.44 * std::numbers::pi / 180.0;
    const double tilt = std::abs(body.tilt) * std::numbers::pi / 180.0;
    const double swing =
        body.season_swing * std::abs(maths::sin(latitude)) *
        (maths::sin(std::min(tilt, std::numbers::pi / 2.0)) / maths::sin(earth_tilt));
    return sea ? swing * body.sea_swing : swing;
}

namespace {

// Plastic ice's top slopes as its strength over its weight: thickness times rise is this, metres².
double held(double length, const Body& body) {
    return std::max(length, 0.0) * body.ice_strength /
           (body.ice_density * std::max(body.gravity, 1e-9));
}

} // namespace

double deepest_dip(double ice_below, double length, const Body& body) {
    // With the dip x, the ice over it is h = ice_below + x + x/k when the bed rises k times as
    // steeply as the top falls, and plastic ice holds h (h - ice_below - x) = held; so
    // (1 + 1/k) x² + ice_below x - k held = 0.
    const double k = std::max(body.overdeepening, 1e-9);
    const double a = 1.0 + (1.0 / k);
    const double b = std::max(ice_below, 0.0);
    return (std::sqrt((b * b) + (4.0 * a * k * held(length, body))) - b) / (2.0 * a);
}

double ice_thickness(double bed, double below, double length, const Body& body) {
    // The thickness h at which h · (bed + h - below) = held.
    const double holding = held(length, body);
    const double rise = bed - below;
    const double root = std::sqrt((rise * rise) + (4.0 * holding));
    return rise > 0.0 ? 2.0 * holding / (rise + root) : (root - rise) / 2.0;
}

Ice ice_on(const Sphere& sphere, const Crust& crust, const Climate& climate, const Rivers& rivers,
           const Body& body) {
    const Land land{.sphere = &sphere,
                    .crust = &crust,
                    .climate = &climate,
                    .rivers = &rivers,
                    .body = &body,
                    .upward = lowest_first(rivers)};
    const Cover cold = cover_on(land, rivers.ground, body.ice_cooling);

    Ice ice;
    ice.carved = carve(land, cold);
    ice.coldest = cold.thickness;
    lay(land, cold, ice);
    std::vector<double> rock = rivers.ground;
    for (std::size_t point = 0; point < rock.size(); ++point) {
        rock[point] += ice.till[point] - ice.carved[point];
    }
    Cover now = cover_on(land, rock, 0.0);
    ice.thickness = std::move(now.thickness);
    ice.surface = std::move(now.surface);
    ice.balance = std::move(now.balance);
    ice.flux = std::move(now.flux);
    ice.calving = now.calving;

    ice.sea_ice.assign(sphere.size(), 0.0);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (!in_sea(rivers, point)) {
            continue;
        }
        const double latitude = asin(std::clamp(sphere.point(point).z, -1.0, 1.0));
        ice.sea_ice[point] = cold_share(climate.temperature[point] - body.sea_freezing,
                                        seasonal_swing(latitude, true, body));
    }
    return ice;
}

namespace {

// Each land point's own water, as the routing `rivers` gathered it: what passes it less what ran
// in.
std::vector<double> own_share(const Rivers& rivers, std::span<const double> gathered) {
    std::vector<double> own(gathered.begin(), gathered.end());
    for (std::uint32_t point = 0; point < rivers.receiver.size(); ++point) {
        if (!in_sea(rivers, point)) {
            own[rivers.receiver[point]] -= gathered[point];
        }
    }
    for (std::uint32_t point = 0; point < rivers.receiver.size(); ++point) {
        own[point] = in_sea(rivers, point) ? 0.0 : std::max(own[point], 0.0);
    }
    return own;
}

// `own` gathered down the receivers, from the highest ground down.
std::vector<double> gathered_down(const Rivers& rivers, std::vector<double> own) {
    std::vector<std::uint32_t> order = lowest_first(rivers);
    for (const std::uint32_t point : std::views::reverse(order)) {
        own[rivers.receiver[point]] += own[point];
    }
    return own;
}

} // namespace

Rivers ice_shaped(const Sphere& sphere, const Rivers& rivers, const Ice& ice) {
    Rivers shaped = rivers;
    const std::uint32_t size = sphere.size();
    // Flooded inwards from the sea over the land as ice left it, lowest first: water finds the
    // lowest way out, and only a hollow truly walled in, by rock or ice, fills to its spill.
    std::vector<std::uint8_t> reached(size, 0);
    using Entry = std::pair<double, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    for (std::uint32_t point = 0; point < size; ++point) {
        if (in_sea(rivers, point)) {
            reached[point] = 1;
            open.emplace(0.0, point);
        }
    }
    while (!open.empty()) {
        const auto [level, point] = open.top();
        open.pop();
        const bool sea = in_sea(shaped, point);
        for (const std::uint32_t next : sphere.neighbors(point)) {
            if (reached[next] != 0) {
                continue;
            }
            reached[next] = 1;
            const double ground =
                rivers.ground[next] - ice.carved[next] + ice.till[next] + ice.thickness[next];
            shaped.ground[next] = ground;
            // The sea comes into troughs below its level that open onto it.
            if (sea && ground < 0.0) {
                shaped.receiver[next] = next;
                shaped.height[next] = ground;
                open.emplace(0.0, next);
                continue;
            }
            const double arc = angle(sphere.point(point), sphere.point(next));
            shaped.height[next] = std::max(ground, level + (1e-3 * arc));
            open.emplace(shaped.height[next], next);
        }
    }
    // Then each point sends its water down the steepest fall of that surface.
    for (std::uint32_t point = 0; point < size; ++point) {
        if (in_sea(shaped, point)) {
            continue;
        }
        double steepest = 0.0;
        for (const std::uint32_t next : sphere.neighbors(point)) {
            const double below = in_sea(shaped, next) ? 0.0 : shaped.height[next];
            const double fall =
                (shaped.height[point] - below) / angle(sphere.point(point), sphere.point(next));
            if (fall > steepest) {
                steepest = fall;
                shaped.receiver[point] = next;
            }
        }
    }
    shaped.discharge = gathered_down(shaped, own_share(rivers, rivers.discharge));
    shaped.flood = gathered_down(shaped, own_share(rivers, rivers.flood));
    return shaped;
}

} // namespace hog::formation

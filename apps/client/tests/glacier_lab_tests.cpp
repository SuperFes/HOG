#include "glacier_lab.hpp"

#include <hog/flow/glacier.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace hog::client::lab;
using Catch::Matchers::WithinRel;

TEST_CASE("snow falls above the snowline and melts below it", "[client][glacier_lab]") {
    const Climate climate;
    CHECK(balance(climate, climate.snowline + 100.0) > 0.0);
    CHECK(balance(climate, climate.snowline - 100.0) < 0.0);
    CHECK(balance(climate, climate.snowline) == 0.0);
    CHECK(balance(climate, 1.0e5) == climate.most_gain);
}

TEST_CASE("flow alone moves ice downhill and keeps all of it", "[client][glacier_lab]") {
    Bed bed = mountain(48, 10.0, 600.0, 3);
    for (int y = 10; y < 14; ++y) {
        for (int x = 10; x < 14; ++x) {
            bed.ice[bed.index(x, y)] = 30.0;
        }
    }
    const double before = total_ice(bed);
    const Climate climate;
    for (int step = 0; step < 200; ++step) {
        flow_step(bed, climate, 1.0);
    }
    CHECK_THAT(total_ice(bed), WithinRel(before, 1.0e-9));
    for (const double ice : bed.ice) {
        CHECK(ice >= 0.0);
    }
}

TEST_CASE("a mountain above the snowline grows a glacier that reaches below it",
          "[client][glacier_lab]") {
    Bed bed = mountain(48, 10.0, 700.0, 3);
    const Climate climate;
    REQUIRE(total_ice(bed) == 0.0);
    const auto began = std::chrono::steady_clock::now();
    run(bed, climate, 300);
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::printf("300 years on 48x48: %.3f s, ice %.0f, terminus %.0f m\n", took, total_ice(bed),
                terminus(bed));
    CHECK(total_ice(bed) > 0.0);
    CHECK(terminus(bed) < climate.snowline);
}

TEST_CASE("no snowline reached means no ice", "[client][glacier_lab]") {
    Bed bed = mountain(32, 10.0, 200.0, 3);
    run(bed, Climate{}, 100);
    CHECK(total_ice(bed) == 0.0);
}

TEST_CASE("ice wears the bed under it and leaves bare rock alone", "[client][glacier_lab]") {
    Bed bed = mountain(48, 10.0, 700.0, 3);
    const Bed start = bed;
    run(bed, Climate{.rockfall = 0.0, .englacial = 0.0}, 300);
    for (std::size_t i = 0; i < bed.rock.size(); ++i) {
        CHECK(bed.rock[i] <= start.rock[i]);
    }
}

TEST_CASE("feature builders shape the bed where asked", "[client][glacier_lab]") {
    Bed bed = flat(40, 10.0, 100.0);
    CHECK(bed.rock[bed.index(5, 5)] == 100.0);
    add_peak(bed, 20.0, 20.0, 80.0, 300.0);
    CHECK(bed.rock[bed.index(20, 20)] > 390.0);
    CHECK(bed.rock[bed.index(0, 0)] < 101.0);
    add_valley(bed, 0.0, 30.0, 39.0, 30.0, 60.0, 50.0);
    CHECK(bed.rock[bed.index(10, 30)] < 80.0);
    CHECK(bed.rock[bed.index(10, 39)] > bed.rock[bed.index(10, 30)]);
    add_basin(bed, 5.0, 5.0, 50.0, 40.0);
    CHECK(bed.rock[bed.index(5, 5)] < bed.rock[bed.index(20, 5)]);
    add_ridge(bed, 0.0, 10.0, 39.0, 10.0, 40.0, 100.0);
    CHECK(bed.rock[bed.index(30, 10)] > bed.rock[bed.index(30, 16)]);
    Bed ramp = flat(20, 10.0, 100.0);
    add_tilt(ramp, 90.0);
    CHECK(ramp.rock[ramp.index(0, 5)] == 100.0);
    CHECK(ramp.rock[ramp.index(19, 5)] == 10.0);
}

TEST_CASE("a peak over the snowline sheds ice into a valley cut below it", "[client][glacier_lab]") {
    Bed bed = flat(64, 10.0, 100.0);
    add_peak(bed, 20.0, 32.0, 120.0, 600.0);
    add_valley(bed, 20.0, 32.0, 63.0, 32.0, 50.0, 60.0);
    run(bed, Climate{}, 300);
    CHECK(total_ice(bed) > 0.0);
    // The ice runs down the cut: thicker in it than on the same slope beside it.
    CHECK(bed.ice[bed.index(30, 32)] > bed.ice[bed.index(30, 40)]);
}

TEST_CASE("a warming climate shrinks a glacier and a cooling one regrows it", "[client][glacier_lab]") {
    Bed bed = mountain(48, 10.0, 700.0, 3);
    const Climate climate;
    run(bed, climate, 300);
    const double grown = total_ice(bed);
    const double reach = terminus(bed);
    run_drifting(bed, climate, 200, 3.0);
    CHECK(total_ice(bed) < grown);
    CHECK(terminus(bed) > reach);
    const double shrunk = total_ice(bed);
    run_drifting(bed, Climate{.snowline = 380.0 + 600.0}, 200, -3.0);
    CHECK(total_ice(bed) > shrunk);
}

TEST_CASE("above the summit the snowline leaves no ice", "[client][glacier_lab]") {
    Bed bed = mountain(48, 10.0, 700.0, 3);
    run(bed, Climate{.rockfall = 0.0, .englacial = 0.0}, 300);
    REQUIRE(total_ice(bed) > 0.0);
    run(bed, Climate{.snowline = 1500.0, .rockfall = 0.0, .englacial = 0.0}, 400);
    CHECK(total_ice(bed) < 1.0);
}

TEST_CASE("noise roughens a bed repeatably and only as far as asked", "[client][glacier_lab]") {
    Bed a = flat(32, 10.0, 100.0);
    Bed b = a;
    roughen(a, 20.0, 100.0, 5);
    roughen(b, 20.0, 100.0, 5);
    CHECK(a.rock == b.rock);
    bool varies = false;
    for (const double h : a.rock) {
        CHECK(std::abs(h - 100.0) <= 20.0);
        varies = varies || h != a.rock[0];
    }
    CHECK(varies);
    Bed c = flat(32, 10.0, 100.0);
    roughen(c, 0.0, 100.0, 5);
    CHECK(c.rock == flat(32, 10.0, 100.0).rock);
}

TEST_CASE("a wandering valley leaves its straight line", "[client][glacier_lab]") {
    Bed straight = flat(40, 10.0, 100.0);
    Bed bent = straight;
    add_valley(straight, 0.0, 20.0, 39.0, 20.0, 60.0, 50.0);
    add_valley(bent, 0.0, 20.0, 39.0, 20.0, 60.0, 50.0, 80.0, 2);
    CHECK(straight.rock != bent.rock);
    CHECK(bent.rock[bent.index(20, 20)] < 100.0 + 1.0e-9);
}

TEST_CASE("a profile reads ice along a line", "[client][glacier_lab]") {
    Bed bed = flat(10, 10.0, 0.0);
    bed.ice[bed.index(5, 2)] = 7.0;
    const auto line = profile(bed, 5, 0, 5, 4, 5);
    REQUIRE(line.size() == 5);
    CHECK(line[2] == 7.0);
    CHECK(line[0] == 0.0);
}

TEST_CASE("flow carries debris with the ice and keeps all of it", "[client][glacier_lab]") {
    Bed bed = mountain(48, 10.0, 600.0, 3);
    for (int y = 10; y < 14; ++y) {
        for (int x = 10; x < 14; ++x) {
            bed.ice[bed.index(x, y)] = 30.0;
            bed.debris[bed.index(x, y)] = 0.5;
        }
    }
    const double before = total_debris(bed);
    for (int step = 0; step < 200; ++step) {
        flow_step(bed, Climate{}, 1.0);
    }
    CHECK_THAT(total_debris(bed), WithinRel(before, 1.0e-9));
}

TEST_CASE("a debris cover slows melt and bare rock walls shed debris onto ice", "[client][glacier_lab]") {
    const Climate climate{.snowline = 500.0};
    Bed clean = flat(8, 10.0, 100.0);
    Bed covered = clean;
    std::fill(clean.ice.begin(), clean.ice.end(), 20.0);
    std::fill(covered.ice.begin(), covered.ice.end(), 20.0);
    std::fill(covered.debris.begin(), covered.debris.end(), 1.0);
    add_balance(clean, climate, 1.0);
    add_balance(covered, climate, 1.0);
    CHECK(covered.ice[0] > clean.ice[0]);

    Bed wall = flat(8, 10.0, 100.0);
    wall.rock[wall.index(3, 4)] = 300.0;
    wall.ice[wall.index(4, 4)] = 10.0;
    rockfall(wall, climate, 1.0);
    CHECK(wall.debris[wall.index(4, 4)] > 0.0);
    CHECK(wall.debris[wall.index(6, 6)] == 0.0);
}

TEST_CASE("ice that melts away leaves its debris as moraine on the bed", "[client][glacier_lab]") {
    Bed bed = flat(6, 10.0, 100.0);
    bed.ice[bed.index(2, 2)] = 0.01;
    bed.debris[bed.index(2, 2)] = 0.4;
    add_balance(bed, Climate{.snowline = 900.0}, 1.0);
    CHECK(bed.ice[bed.index(2, 2)] == 0.0);
    CHECK(bed.debris[bed.index(2, 2)] == 0.0);
    CHECK_THAT(bed.rock[bed.index(2, 2)], WithinRel(100.4, 1.0e-5));
}

TEST_CASE("a glacier meeting the sea ends in a grounded face and calves the rest", "[client][glacier_lab]") {
    Bed bed = flat(64, 25.0, 300.0);
    add_tilt(bed, 450.0);
    add_ridge(bed, 4.0, 32.0, 24.0, 32.0, 250.0, 900.0);
    add_valley(bed, 14.0, 32.0, 63.0, 32.0, 150.0, 200.0);
    const Climate climate{.snowline = 420.0, .gain = 0.004, .most_gain = 1.0, .sea = 0.0};
    run(bed, climate, 400);
    const auto in_sea = [&](const Bed& b, const Climate& c) {
        double sum = 0.0;
        for (std::size_t i = 0; i < b.ice.size(); ++i) {
            sum += b.rock[i] < c.sea ? b.ice[i] : 0.0;
        }
        return sum;
    };
    Bed uncalved = bed;
    uncalved = flat(64, 25.0, 300.0);
    add_tilt(uncalved, 450.0);
    add_ridge(uncalved, 4.0, 32.0, 24.0, 32.0, 250.0, 900.0);
    add_valley(uncalved, 14.0, 32.0, 63.0, 32.0, 150.0, 200.0);
    Climate no_calving = climate;
    no_calving.calving = 0.0;
    run(uncalved, no_calving, 400);
    CHECK(in_sea(bed, climate) > 0.0);
    CHECK(in_sea(bed, climate) < in_sea(uncalved, no_calving));
}

TEST_CASE("colder air freezes more sea and warm air leaves it open", "[client][glacier_lab]") {
    const auto cover = [](double base) {
        Bed bed = flat(40, 100.0, -50.0);
        const Climate climate{.sea = 0.0};
        for (unsigned year = 0; year < 20; ++year) {
            sea_ice_year(bed, climate, Cold{.base = base, .per_row = 0.0, .patchy = 6.0}, 1);
        }
        return sea_ice_cover(bed, climate);
    };
    CHECK(cover(5.0) == 0.0);
    CHECK(cover(-6.0) > 0.0);
    CHECK(cover(-6.0) < 1.0);
    CHECK(cover(-12.0) > cover(-6.0));
    CHECK(cover(-40.0) == 1.0);
}

TEST_CASE("sea ice thickens more slowly the thicker it is and melts back", "[client][glacier_lab]") {
    Bed bed = flat(12, 100.0, -50.0);
    const Climate climate{.sea = 0.0};
    const Cold cold{.base = -30.0, .per_row = 0.0, .patchy = 0.0};
    sea_ice_year(bed, climate, cold, 1);
    const double first = bed.floe[bed.index(6, 6)];
    sea_ice_year(bed, climate, cold, 1);
    const double second = bed.floe[bed.index(6, 6)] - first;
    CHECK(second < first);
    for (int year = 0; year < 40; ++year) {
        sea_ice_year(bed, climate, Cold{.base = 10.0, .per_row = 0.0, .patchy = 0.0}, 1);
    }
    CHECK(sea_ice_cover(bed, climate) == 0.0);
}

TEST_CASE("an ice age cuts the valley floor deeper than its sides and leaves it when it warms",
          "[client][glacier_lab]") {
    Bed bed = flat(64, 25.0, 300.0);
    add_tilt(bed, 250.0);
    add_ridge(bed, 4.0, 32.0, 24.0, 32.0, 250.0, 900.0);
    add_valley(bed, 14.0, 32.0, 63.0, 32.0, 200.0, 150.0);
    const Bed before = bed;
    Climate cold{.snowline = 420.0, .gain = 0.004, .most_gain = 1.0, .wear = 1.0e-4};
    run(bed, cold, 300);
    const auto worn = [&](int x, int y) { return before.rock[bed.index(x, y)] - bed.rock[bed.index(x, y)]; };
    CHECK(worn(26, 32) > worn(26, 36));
    run_drifting(bed, cold, 400, 3.0);
    CHECK(total_ice(bed) < 1.0e4);
    CHECK(worn(26, 32) > 0.0);
}

// Pictures of a few beds before and after the climate has worked, written to $HOG_LOOK.
TEST_CASE("glacier beds, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const std::string dir{where};
    const auto show = [&](const char* name, Bed bed, const Climate& climate, int years) {
        write_ppm(bed, dir + "/" + name + "_0.ppm");
        run(bed, climate, years);
        std::printf("%s: ice %.0f, deepest %.0f m\n", name, total_ice(bed), *std::max_element(bed.ice.begin(), bed.ice.end()));
        write_ppm(bed, dir + "/" + name + "_" + std::to_string(years) + ".ppm");
    };
    show("massif", mountain(64, 50.0, 900.0, 3), Climate{.snowline = 620.0}, 300);
    const Climate wet{.snowline = 1000.0, .gain = 0.006, .most_gain = 2.0, .rockfall = 0.0};
    const auto range_bed = [](unsigned seed) {
        Bed bed = flat(48, 100.0, 500.0);
        add_tilt(bed, 500.0);
        roughen(bed, 40.0, 300.0, seed);
        add_ridge(bed, 2.0, 24.0, 16.0, 24.0, 700.0, 1800.0);
        add_valley(bed, 6.0, 24.0, 47.0, 24.0, 500.0, 500.0, 150.0, seed);
        return bed;
    };
    const Bed range = range_bed(4);
    Bed grown = range;
    run(grown, wet, 600);
    std::printf("valley ice along axis:");
    for (const double ice : profile(grown, 6, 24, 46, 24, 14)) {
        std::printf(" %.0f", ice);
    }
    std::printf("\n");
    show("range", range, wet, 600);
    Bed retreat = grown;
    run_drifting(retreat, wet, 300, 3.0);
    write_ppm(retreat, dir + "/range_warm.ppm");
    // An ice age, then warmth: where the sea ends up decides whether the valley becomes a fjord.
    Bed fjord = range_bed(21);
    Climate age = wet;
    age.snowline = 800.0;
    age.wear = 1.0e-4;
    age.sea = 0.0;
    run(fjord, age, 600);
    write_ppm(fjord, dir + "/fjord_age.ppm", 8, age.sea);
    run_drifting(fjord, age, 400, 4.0);
    write_ppm(fjord, dir + "/fjord_warm.ppm", 8, age.sea);
    Bed dirty = range_bed(7);
    Climate rocky = wet;
    rocky.rockfall = 0.002;
    run(dirty, rocky, 600);
    std::printf("debris %.1f\n", total_debris(dirty));
    write_ppm(dirty, dir + "/dirty_600.ppm");
    // Sea ice from open water at the warm edge to a pack at the cold one.
    Bed sea = flat(64, 100.0, -50.0);
    const Climate water{.sea = 0.0};
    for (unsigned year = 0; year < 30; ++year) {
        sea_ice_year(sea, water, Cold{.base = -2.0, .per_row = 0.2, .patchy = 5.0}, 3);
    }
    std::printf("sea cover %.2f\n", sea_ice_cover(sea, water));
    write_ppm(sea, dir + "/sea_ice.ppm", 8, water.sea);
}

namespace {

// The ground of a world over a square patch round `centre`, as a lab bed with heights over the sea
// (or the radius where there is none), plus the snowline its latitude gives.
struct WorldPatch {
    Bed bed;
    Climate climate;
};

WorldPatch patch_of(const hog::globe::Generator& generator, hog::DVec3 centre, int size, double cell) {
    const auto& planet = generator.planet();
    const double datum = generator.sea_level().value_or(planet.radius);
    const hog::DVec3 up = glm::normalize(centre);
    const hog::DVec3 east = glm::normalize(glm::cross(hog::DVec3{0.0, 0.0, 1.0}, up));
    const hog::DVec3 north = glm::cross(up, east);
    WorldPatch out{.bed = flat(size, cell, 0.0), .climate = {}};
    const double lod = hog::globe::voxel_lod(generator) * 8.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = (x - (size / 2.0)) * cell;
            const double dy = (y - (size / 2.0)) * cell;
            const hog::DVec3 at = glm::normalize((up * planet.radius) + (east * dx) + (north * dy));
            out.bed.rock[out.bed.index(x, y)] = generator.column(at, lod).ground() - datum;
        }
    }
    const double latitude = std::asin(std::clamp(up.z, -1.0, 1.0));
    const double kelvin = hog::globe::temperature_at(planet, latitude);
    out.climate.snowline = std::max((kelvin - 273.15) / 0.0065, 0.0);
    out.climate.sea = generator.sea_level() ? 0.0 : -1.0e9;
    return out;
}

} // namespace

TEST_CASE("glaciers on a world, to look at", "[.look]") {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts
    const char* where = std::getenv("HOG_LOOK");
    REQUIRE(where != nullptr);
    const std::string dir{where};
    auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    planet.radius = 20'000.0;
    const auto began = std::chrono::steady_clock::now();
    const hog::globe::Generator generator{planet};
    const auto lod = hog::globe::voxel_lod(generator) * 8.0;
    std::printf("generator: %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
    const double datum = generator.sea_level().value_or(planet.radius);
    const auto snowline_at = [&](const hog::DVec3& d) {
        return std::max((hog::globe::temperature_at(planet, std::asin(d.z)) - 273.15) / 0.0065, 0.0);
    };
    // Where the ground stands highest over its own snowline on a coarse sweep.
    const auto pick = [&](double min_lat, double max_lat, double colder) {
        hog::DVec3 best{1.0, 0.0, 0.0};
        double most = -1.0e30;
        for (double lat = min_lat; lat <= max_lat; lat += 4.0) {
            for (int lon = 0; lon < 360; lon += 4) {
                const double a = lat * 0.0174533;
                const double b = lon * 0.0174533;
                const hog::DVec3 d{std::cos(a) * std::cos(b), std::cos(a) * std::sin(b), std::sin(a)};
                const double over = generator.column(d, lod).ground() - datum - std::max(snowline_at(d) - colder, 0.0);
                if (over > most) {
                    most = over;
                    best = d;
                }
            }
        }
        return best;
    };
    const auto glaciate = [&](const char* name, hog::DVec3 at, double colder) {
        WorldPatch world = patch_of(generator, at, 64, 100.0);
        world.climate.snowline = std::max(world.climate.snowline - colder, 0.0);
        std::printf("%s: lat %.1f, ground %.0f..%.0f m, snowline %.0f m\n", name, std::asin(at.z) / 0.0174533,
                    *std::min_element(world.bed.rock.begin(), world.bed.rock.end()),
                    *std::max_element(world.bed.rock.begin(), world.bed.rock.end()), world.climate.snowline);
        write_ppm(world.bed, dir + "/" + name + "_0.ppm", 8, world.climate.sea);
        run(world.bed, world.climate, 500);
        std::printf("%s: ice %.0f, deepest %.0f m\n", name, total_ice(world.bed),
                    *std::max_element(world.bed.ice.begin(), world.bed.ice.end()));
        write_ppm(world.bed, dir + "/" + name + "_500.ppm", 8, world.climate.sea);
    };
    glaciate("today", pick(-80.0, 80.0, 0.0), 0.0);
    glaciate("iceage", pick(-35.0, 35.0, 2000.0), 2000.0);
}

TEST_CASE("the flow library moves ice as the lab does on the same bed", "[client][glacier_lab]") {
    Bed bed = mountain(40, 10.0, 600.0, 5);
    for (int y = 8; y < 14; ++y) {
        for (int x = 8; x < 14; ++x) {
            bed.ice[bed.index(x, y)] = 25.0;
            bed.debris[bed.index(x, y)] = 1.0;
        }
    }
    const Climate climate;
    const auto size = static_cast<std::size_t>(bed.size);
    hog::flow::Glacier glacier{size * size, {.cell = bed.cell, .flow = climate.flow, .wear = climate.wear}};
    const auto at = [&](int x, int y) {
        return x < 0 || y < 0 || x >= bed.size || y >= bed.size ? hog::flow::Glacier::none
                                                                 : static_cast<int>(bed.index(x, y));
    };
    for (int y = 0; y < bed.size; ++y) {
        for (int x = 0; x < bed.size; ++x) {
            const std::size_t i = bed.index(x, y);
            glacier[i] = {.rock = bed.rock[i], .ice = bed.ice[i], .debris = bed.debris[i],
                          .next = {at(x + 1, y), at(x, y + 1), at(x - 1, y), at(x, y - 1)}};
        }
    }
    for (int step = 0; step < 60; ++step) {
        const double lab = flow_step(bed, climate, 1.0);
        CHECK(glacier.step(1.0) == lab);
    }
    for (std::size_t i = 0; i < bed.ice.size(); ++i) {
        CHECK_THAT(glacier[i].ice, WithinRel(bed.ice[i], 1.0e-9));
        CHECK_THAT(glacier[i].rock, WithinRel(bed.rock[i], 1.0e-9));
        CHECK_THAT(glacier[i].debris, WithinRel(bed.debris[i], 1.0e-9));
    }
}

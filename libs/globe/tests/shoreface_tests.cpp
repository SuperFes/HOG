#include "../src/shoreface.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::globe::Across;
using hog::globe::across;
using hog::globe::as_built;
using hog::globe::Bars;
using hog::globe::bars_at;
using hog::globe::bars_on;
using hog::globe::beach_top;
using hog::globe::Built;
using hog::globe::built_along;
using hog::globe::built_on;
using hog::globe::closure_depth;
using hog::globe::face_slope;
using hog::globe::Grains;
using hog::globe::grains_of;
using hog::globe::Ground;
using hog::globe::held;
using hog::globe::Laid;
using hog::globe::lying_over;
using hog::globe::Material;
using hog::globe::mixed;
using hog::globe::out_at;
using hog::globe::out_to;
using hog::globe::Profile;
using hog::globe::profile_for;
using hog::globe::runup;
using hog::globe::Sea;
using hog::globe::Section;
using hog::globe::top_along;

namespace {

constexpr double gravity = 9.81;
// An open coast's: 8 m storms, an ordinary day's metre and a half of 12 s swell.
constexpr Sea open_sea{.storm = 8.0, .usual = 1.5, .period = 12.0};

double degrees(double slope) {
    return std::atan(slope) * 180.0 / std::numbers::pi;
}

// The sand on `ground` summed across the shore from the top of the beach, as the beach's top
// reads it.
double summed_across(const Profile& profile, const Ground& ground, const Built& built) {
    constexpr double step = 0.05;
    const double from = -profile.berm / ground.slope;
    double sum = 0.0;
    for (int at = 0; at < 2'000'000; ++at) {
        const double out = from + ((at + 0.5) * step);
        const std::optional<Across> top = beach_top(profile, ground, built, out);
        const double plane = std::max(-ground.slope * out, -ground.floor);
        if (top) {
            sum += std::max(0.0, top->height - plane) * step;
        } else if (out > 0.0) {
            break;
        }
    }
    return sum;
}

// Ground across a shore `height` metres high `out` metres out, from 1 km in to 1 km out.
template <typename Height>
Section section_of(const Height& height) {
    Section section;
    for (std::size_t at = 0; at < Section::samples; ++at) {
        const double out = -1'000.0 + (2'000.0 * static_cast<double>(at) /
                                       static_cast<double>(Section::samples - 1));
        section.out[at] = out;
        section.height[at] = height(out);
    }
    return section;
}

// The ground along a section `out` metres out, as it runs between its samples.
double along(const Section& section, double out) {
    std::size_t at = 0;
    while (at + 2 < Section::samples && section.out[at + 1] < out) {
        ++at;
    }
    const double part = (out - section.out[at]) / (section.out[at + 1] - section.out[at]);
    return section.height[at] + ((section.height[at + 1] - section.height[at]) * part);
}

// How thick the beach lies `out` metres out from the ground's waterline.
double thickness(const Profile& profile, const Ground& ground, double volume, double out) {
    const std::optional<Built> built = built_on(profile, ground, volume);
    if (!built) {
        return 0.0;
    }
    const std::optional<Across> top = beach_top(profile, ground, *built, out);
    const double plane = std::max(-ground.slope * out, -ground.floor);
    return top ? std::max(0.0, top->height - plane) : 0.0;
}

} // namespace

TEST_CASE("sand's grains are a fraction of a millimetre, shingle's stones centimetres",
          "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Grains shells = grains_of(Material::ShellSand, gravity);
    const Grains volcanic = grains_of(Material::VolcanicSand, gravity);
    const Grains shingle = grains_of(Material::Shingle, gravity);
    CHECK(sand.size > 0.1e-3);
    CHECK(sand.size < 0.5e-3);
    CHECK(shingle.size > 10e-3);
    // Dean's scale for the sand most beaches are made of is about a tenth.
    CHECK_THAT(sand.scale, WithinAbs(0.1, 0.03));
    CHECK(shells.scale < sand.scale);
    CHECK(volcanic.scale > sand.scale);
    CHECK(shingle.scale > 3.0 * sand.scale);
    const std::array grains{sand, shingle};
    const std::array shares{0.75, 0.25};
    const Grains mix = mixed(grains, shares);
    CHECK_THAT(mix.scale, WithinRel((0.75 * sand.scale) + (0.25 * shingle.scale), 1e-12));
}

TEST_CASE("a sand beach's face lies a few degrees, a shingle beach's far steeper",
          "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Grains shingle = grains_of(Material::Shingle, gravity);
    const double sandy = degrees(face_slope(open_sea, sand, gravity));
    const double stony = degrees(face_slope(open_sea, shingle, gravity));
    CHECK(sandy > 1.0);
    CHECK(sandy < 7.0);
    CHECK(stony > 8.0);
    CHECK(stony < 25.0);
    // Far flatter than the steepest the sand holds soaked.
    CHECK(face_slope(open_sea, sand, gravity) < 0.25 * sand.steepest);
    // Bigger waves flatten it.
    const Sea rough{.storm = 8.0, .usual = 3.0, .period = 12.0};
    CHECK(face_slope(rough, sand, gravity) < face_slope(open_sea, sand, gravity));
}

TEST_CASE("storms move the bottom about twice their waves' height down, ordinary waves build "
          "berms higher on steeper faces",
          "[globe][shoreface]") {
    CHECK_THAT(closure_depth(open_sea, gravity), WithinRel(2.0 * open_sea.storm, 0.15));
    // A sand beach's berm stands a metre or two over the sea, a shingle beach's ridge higher.
    const Grains sand = grains_of(Material::Sand, gravity);
    const Grains shingle = grains_of(Material::Shingle, gravity);
    const double sandy = runup(open_sea, face_slope(open_sea, sand, gravity), gravity);
    const double stony = runup(open_sea, face_slope(open_sea, shingle, gravity), gravity);
    CHECK(sandy > 0.5);
    CHECK(sandy < 3.0);
    CHECK(stony > 2.0 * sandy);
    CHECK(stony < open_sea.storm);
}

TEST_CASE("a beach falls from its berm down its face and shoreface, smoothly and ever out",
          "[globe][shoreface]") {
    const Profile profile = profile_for(open_sea, grains_of(Material::Sand, gravity), gravity);
    CHECK(across(profile, -1e5).height == profile.berm);
    CHECK_THAT(across(profile, -1.0).height, WithinRel(profile.face, 1e-12));
    CHECK(across(profile, 0.0).height == 0.0);
    double last = across(profile, -500.0).height;
    for (int at = 0; at < 11'000; ++at) {
        const Across here = across(profile, -500.0 + (0.5 * at));
        CHECK(here.height <= last + 1e-9);
        CHECK(here.height - last > -1.0);
        CHECK(here.slope <= 0.0);
        last = here.height;
    }
    // Down to its closure by its own reckoning, and Dean's far out.
    const double reach = out_to(profile, profile.closure);
    CHECK_THAT(across(profile, reach - 1e-6).height, WithinAbs(-profile.closure, 1e-4));
    CHECK_THAT(across(profile, out_to(profile, 3.0)).height, WithinAbs(-3.0, 1e-6));
    CHECK(reach > 300.0);
}

TEST_CASE("a beach holds what it's built of, further out the more there is", "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Profile profile = profile_for(open_sea, sand, gravity);
    const Ground steep{.slope = 0.15, .floor = 60.0};
    const Ground gentle{.slope = 0.02, .floor = 60.0};
    const Ground cliff{.slope = 1.5 * sand.steepest, .floor = 60.0};
    for (const Ground& ground : {steep, gentle, cliff}) {
        for (const double volume : {1.0, 50.0, 2'000.0, 40'000.0, 1.0e6}) {
            const std::optional<Built> built = built_on(profile, ground, volume);
            REQUIRE(built.has_value());
            const Built laid = built.value_or(Built{});
            CHECK_THAT(held(as_built(profile, laid), ground, laid.ahead, laid.top),
                       WithinRel(volume, 1e-6));
            CHECK_THAT(summed_across(profile, ground, laid),
                       WithinAbs(volume, (0.01 * volume) + 0.1));
        }
        const Built less = built_on(profile, ground, 2'000.0).value_or(Built{});
        const Built more = built_on(profile, ground, 40'000.0).value_or(Built{});
        CHECK(((more.ahead > less.ahead) || (more.down > less.down) || (more.top > less.top)));
    }
    // With little, the beach's face lies whole at the waterline, its shoreface stopping short,
    // even on ground far steeper than it; only plenty builds it out to sea.
    const Built short_of = built_on(profile, steep, 2'000.0).value_or(Built{});
    CHECK(short_of.down < profile.closure);
    CHECK(short_of.down > 0.0);
    const std::optional<Across> ashore = beach_top(profile, steep, short_of, -1.0);
    REQUIRE(ashore.has_value());
    CHECK(ashore.value_or(Across{}).height > steep.slope);
    CHECK(built_on(profile, steep, 1.0e6).value_or(Built{}).ahead > 2.0 * short_of.ahead);
    const Built least = built_on(profile, steep, 1.0).value_or(Built{});
    CHECK(least.ahead < short_of.ahead);
    CHECK(least.down == 0.0);
    // A trace on ground gentler than the beach's face lies only low on it.
    CHECK(built_on(profile, gentle, 1e-3).value_or(Built{}).top < profile.berm);
    CHECK_FALSE(built_on(profile, steep, 0.0));
    CHECK_FALSE(built_on(profile, {.slope = 0.1, .floor = 0.0}, 100.0));
}

TEST_CASE("on ground steeper than sand holds soaked, it gathers at the foot, reaching the shore "
          "only with plenty",
          "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Profile profile = profile_for(open_sea, sand, gravity);
    const Ground cliff{.slope = 1.5 * sand.steepest, .floor = 100.0};
    const Built little = built_on(profile, cliff, 500.0).value_or(Built{});
    CHECK(little.top < -50.0);
    CHECK(little.top > -cliff.floor);
    // None at the waterline, some down at the foot.
    CHECK(thickness(profile, cliff, 500.0, 1.0) == 0.0);
    CHECK(thickness(profile, cliff, 500.0, 0.98 * cliff.floor / cliff.slope) > 0.0);
    CHECK(built_on(profile, cliff, 1.0e6).value_or(Built{}).top > profile.berm);
}

TEST_CASE("a beach's berm falls away inland as steep as dry sand holds, its toe out to sea as "
          "steep as soaked",
          "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Profile profile = profile_for(open_sea, sand, gravity);
    const Ground ground{.slope = 0.1, .floor = 60.0};
    const Built built = built_on(profile, ground, 5'000.0).value_or(Built{});
    const double inland = -profile.berm / ground.slope;
    const std::optional<Across> back = beach_top(profile, ground, built, inland - 2.0);
    REQUIRE(back.has_value());
    CHECK_THAT(back.value_or(Across{}).height, WithinRel(profile.berm - (2.0 * sand.dry), 1e-9));
    CHECK(sand.dry > sand.steepest);
    const Profile laid = as_built(profile, built);
    const double past = built.ahead + out_to(laid, laid.closure) + 5.0;
    const std::optional<Across> toe = beach_top(profile, ground, built, past);
    REQUIRE(toe.has_value());
    CHECK_THAT(toe.value_or(Across{}).slope, WithinRel(-sand.steepest, 1e-9));
    // Its toe runs on down to whatever ground it meets.
    const std::optional<Across> far = beach_top(profile, ground, built, 1e5);
    REQUIRE(far.has_value());
    CHECK(far.value_or(Across{}).height < -ground.floor);
}

TEST_CASE("a shore's ground tells how far out it's as high, running on past its ends",
          "[globe][shoreface]") {
    const Section section = section_of([](double out) { return -0.1 * out; });
    CHECK_THAT(out_at(section, 0.0).height, WithinAbs(0.0, 1e-9));
    CHECK_THAT(out_at(section, -12.5).height, WithinAbs(125.0, 1e-9));
    CHECK_THAT(out_at(section, -12.5).slope, WithinAbs(10.0, 1e-9));
    CHECK_THAT(out_at(section, -500.0).height, WithinAbs(5'000.0, 1e-6));
    CHECK_THAT(out_at(section, 500.0).height, WithinAbs(-5'000.0, 1e-6));
}

TEST_CASE("a beach laid along its shore is as thick as its top over the ground that high",
          "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Profile profile = profile_for(open_sea, sand, gravity);
    const Ground ground{.slope = 0.08, .floor = 60.0};
    const Section section = section_of([&](double out) { return -ground.slope * out; });
    const Laid laid{.profile = profile,
                    .built = built_along(profile, section, 20'000.0).value_or(Built{}),
                    .bars = bars_on(profile, open_sea, sand, gravity),
                    .section = section};
    double last = lying_over(laid, 25.0, 0.3).height;
    for (int at = 50; at >= -400; --at) {
        const double height = 0.5 * at;
        const Across lying = lying_over(laid, height, 0.3);
        CHECK(lying.height >= 0.0);
        // No faster than the ground falls and a slope as steep as soaked sand holds.
        CHECK(std::abs(lying.height - last) < 0.5 + (0.5 / ground.slope * sand.steepest));
        last = lying.height;
        // Its rate is how it thickens a little higher.
        const double higher = lying_over(laid, height + 1e-5, 0.3).height;
        const double lower = lying_over(laid, height - 1e-5, 0.3).height;
        if (lying.height > 0.01 && higher > 0.0 && lower > 0.0) {
            CHECK_THAT(lying.slope, WithinAbs((higher - lower) / 2e-5, 1e-3));
        }
    }
    CHECK(lying_over(laid, 0.0, 0.0).height > 0.0);
    // Its toe meets the ground, none past.
    CHECK(lying_over(laid, -1'000.0, 0.0).height == 0.0);
}

TEST_CASE("a beach built along its shore's ground holds what it's built of", "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Profile profile = profile_for(open_sea, sand, gravity);
    // Steepening out to sea, then flattening onto a floor.
    const Section section = section_of([](double out) {
        return out < 0.0 ? -0.05 * out : std::max((-0.05 * out) - (2e-4 * out * out), -120.0);
    });
    for (const double volume : {10.0, 500.0, 5'000.0, 50'000.0}) {
        const std::optional<Built> built = built_along(profile, section, volume);
        REQUIRE(built.has_value());
        const Built laid = built.value_or(Built{});
        constexpr double step = 0.5;
        double sum = 0.0;
        for (int at = 0; at < 4'000; ++at) {
            const double out = -1'000.0 + ((at + 0.5) * step);
            sum +=
                std::max(0.0, top_along(profile, section, laid, out).height - along(section, out)) *
                step;
        }
        CHECK_THAT(sum, WithinAbs(volume, (0.03 * volume) + 2.0));
    }
    // None off ground that never goes under the sea.
    CHECK_FALSE(
        built_along(profile, section_of([](double out) { return 10.0 - (out * 1e-3); }), 100.0));
}

TEST_CASE("down a face steeper than sand holds, its top stays no steeper, closing on the floor "
          "below",
          "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Profile profile = profile_for(open_sea, sand, gravity);
    // A cliff falling 0.9 a metre to a floor 60 m down.
    const auto cliff = [](double out) {
        return out < 66.7 ? -0.9 * out : -60.0 - (0.01 * (out - 66.7));
    };
    const Section section = section_of(cliff);
    const Laid laid{.profile = profile,
                    .built = built_along(profile, section, 5'000.0).value_or(Built{}),
                    .bars = {},
                    .section = section};
    double top_before = 0.0;
    double out_before = 0.0;
    bool first = true;
    bool any = false;
    for (int at = 0; at <= 700; ++at) {
        const double height = -0.1 * at;
        const double thick = lying_over(laid, height, 0.0).height;
        const double out = out_at(laid.section, height).height;
        any = any || thick > 0.0;
        if (!first && out > out_before + 1e-6) {
            CHECK((height + thick - top_before) / (out - out_before) > -1.05 * sand.steepest);
        }
        top_before = height + thick;
        out_before = out;
        first = false;
    }
    CHECK(any);
}

TEST_CASE("fine sand under big waves lies in several bars, shingle in none", "[globe][shoreface]") {
    const Grains shells = grains_of(Material::ShellSand, gravity);
    const Grains shingle = grains_of(Material::Shingle, gravity);
    const Bars fine = bars_on(profile_for(open_sea, shells, gravity), open_sea, shells, gravity);
    const Bars stony = bars_on(profile_for(open_sea, shingle, gravity), open_sea, shingle, gravity);
    CHECK(stony.count == 0);
    REQUIRE(fine.count >= 2);
    for (int bar = 1; bar < fine.count; ++bar) {
        const auto at = static_cast<std::size_t>(bar);
        CHECK(fine.crest[at] > fine.crest[at - 1]);
        CHECK(fine.relief[at] > fine.relief[at - 1]);
    }
    // The innermost where an ordinary day's waves break.
    const Profile profile = profile_for(open_sea, shells, gravity);
    CHECK_THAT(across(profile, fine.crest[0]).height, WithinRel(-open_sea.usual / 0.78, 1e-6));
}

TEST_CASE("bars move sand from their troughs to their crests, none from elsewhere",
          "[globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Profile profile = profile_for(open_sea, sand, gravity);
    const Bars bars = bars_on(profile, open_sea, sand, gravity);
    REQUIRE(bars.count >= 1);
    for (const double shift : {0.0, 0.4, -0.4}) {
        double sum = 0.0;
        double highest = 0.0;
        constexpr double step = 0.01;
        const auto steps = static_cast<int>(2.0 * out_to(profile, profile.closure) / step);
        for (int at = 0; at < steps; ++at) {
            const double height = bars_at(bars, (at + 0.5) * step, shift).height;
            sum += height * step;
            highest = std::max(highest, height);
        }
        const auto outer = static_cast<std::size_t>(bars.count - 1);
        CHECK_THAT(sum, WithinAbs(0.0, 1e-3 * bars.relief[outer] * bars.width[outer]));
        CHECK_THAT(highest, WithinRel(0.5 * bars.relief[outer], 0.01));
    }
    CHECK(bars_at(bars, -10.0, 0.0).height == 0.0);
    // Its slope is its height's rate out.
    const double out = bars.crest[0] - (0.3 * bars.width[0]);
    const double rate =
        (bars_at(bars, out + 1e-4, 0.0).height - bars_at(bars, out - 1e-4, 0.0).height) / 2e-4;
    CHECK_THAT(bars_at(bars, out, 0.0).slope, WithinRel(rate, 1e-4));
}

TEST_CASE("bars are cut crescent on beaches between steep and flat, straight on the flattest",
          "[globe][shoreface]") {
    const auto rhythm = [](Material material, const Sea& sea) {
        const Grains grains = grains_of(material, gravity);
        return bars_on(profile_for(sea, grains, gravity), sea, grains, gravity).rhythm;
    };
    // Medium sand under an ordinary metre of swell lies between.
    CHECK(rhythm(Material::Sand, {.storm = 8.0, .usual = 1.0, .period = 12.0}) == 1.0);
    // Fine shell sand under big waves lies flat.
    CHECK(rhythm(Material::ShellSand, {.storm = 8.0, .usual = 3.0, .period = 12.0}) == 0.0);
}

TEST_CASE("building a beach on the ground", "[.bench][globe][shoreface]") {
    const Grains sand = grains_of(Material::Sand, gravity);
    const Profile profile = profile_for(open_sea, sand, gravity);
    constexpr int builds = 100'000;
    double sink = 0.0;
    const auto started = std::chrono::steady_clock::now();
    for (int at = 0; at < builds; ++at) {
        const Ground ground{.slope = 0.01 + (0.6 * (at % 97) / 97.0), .floor = 200.0};
        sink += built_on(profile, ground, 10.0 * (1 + (at % 1'009))).value_or(Built{}).ahead;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    WARN(seconds / builds * 1e6 << " us a beach (" << sink << ")");
}

#include <hog/core/profile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("profiling is on only where the build asks for it") {
#ifdef HOG_PROFILE
    CHECK(hog::profile::enabled);
#else
    CHECK_FALSE(hog::profile::enabled);
#endif
}

TEST_CASE("profiling marks leave what they mark alone, with no profiler connected") {
    HOG_THREAD("profile test");
    int counted = 0;
    for (int frame = 0; frame < 3; ++frame) {
        HOG_ZONE("frame");
        for (int part = 0; part < 4; ++part) {
            HOG_ZONE("part");
            HOG_ZONE_TEXT(std::string_view{"labelled"});
            ++counted;
        }
        HOG_PLOT("counted", static_cast<double>(counted));
        HOG_FRAME();
    }
    CHECK(counted == 12);
}

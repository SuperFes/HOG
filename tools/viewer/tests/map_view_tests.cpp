#include <hog/viewer/map_view.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using hog::DVec2;
using hog::DVec3;
using hog::viewer::MapView;

namespace {

void check_near(DVec2 actual, DVec2 expected) {
    CHECK(actual.x == Catch::Approx(expected.x).margin(1e-12));
    CHECK(actual.y == Catch::Approx(expected.y).margin(1e-12));
}

void check_near(DVec3 actual, DVec3 expected) {
    CHECK(actual.x == Catch::Approx(expected.x).margin(1e-12));
    CHECK(actual.y == Catch::Approx(expected.y).margin(1e-12));
    CHECK(actual.z == Catch::Approx(expected.z).margin(1e-12));
}

} // namespace

TEST_CASE("the whole map shows at first", "[viewer][map]") {
    const MapView view;
    check_near(view.shown().low, DVec2{0.0, 0.0});
    check_near(view.shown().high, DVec2{1.0, 1.0});
    check_near(hog::viewer::map_point(view.shown(), DVec2{0.25, 0.75}), DVec2{0.25, 0.75});
}

TEST_CASE("zooming keeps the point under the cursor still", "[viewer][map]") {
    MapView view;
    const DVec2 at{0.3, 0.6};
    const DVec2 before = hog::viewer::map_point(view.shown(), at);
    view.zoom_by(4.0, at);
    check_near(hog::viewer::shown_point(view.shown(), before), at);
    CHECK(view.zoom() == 4.0);
    check_near(hog::viewer::map_point(view.shown(), at), before);
    const DVec2 size = view.shown().high - view.shown().low;
    check_near(size, DVec2{0.25, 0.25});
}

TEST_CASE("the view never leaves the map", "[viewer][map]") {
    MapView view;
    view.zoom_by(0.5, DVec2{0.5, 0.5});
    CHECK(view.zoom() == 1.0);
    view.zoom_by(1'000.0, DVec2{0.0, 0.0});
    CHECK(view.zoom() == MapView::most_zoom);
    check_near(view.shown().low, DVec2{0.0, 0.0});

    view.pan(DVec2{5.0, 5.0});
    check_near(view.shown().low, DVec2{0.0, 0.0});
    view.pan(DVec2{-100.0, -100.0});
    check_near(view.shown().high, DVec2{1.0, 1.0});
}

TEST_CASE("panning moves the map with the cursor", "[viewer][map]") {
    MapView view;
    view.zoom_by(2.0, DVec2{0.5, 0.5});
    const DVec2 before = view.shown().low;
    // Dragging right by a tenth of the shown area brings in what was to the left.
    view.pan(DVec2{0.1, 0.0});
    check_near(view.shown().low, before - DVec2{0.05, 0.0});
    view.reset();
    CHECK(view.zoom() == 1.0);
}

TEST_CASE("pixels sample the middle of their share of the shown area", "[viewer][map]") {
    const hog::viewer::MapRect shown{.low = {0.25, 0.5}, .high = {0.75, 0.75}};
    check_near(hog::viewer::pixel_point(shown, 4, 2, 0, 0), DVec2{0.3125, 0.5625});
    check_near(hog::viewer::pixel_point(shown, 4, 2, 3, 1), DVec2{0.6875, 0.6875});
}

TEST_CASE("map points face out from the planet by latitude and longitude", "[viewer][map]") {
    check_near(hog::viewer::map_direction(DVec2{0.5, 0.5}), DVec3{1.0, 0.0, 0.0});
    check_near(hog::viewer::map_direction(DVec2{0.75, 0.5}), DVec3{0.0, 1.0, 0.0});
    check_near(hog::viewer::map_direction(DVec2{0.3, 0.0}), DVec3{0.0, 0.0, 1.0});
    check_near(hog::viewer::map_direction(DVec2{0.0, 0.5}), DVec3{-1.0, 0.0, 0.0});
}

TEST_CASE("directions find their way back to the map point under them", "[viewer][map]") {
    for (const DVec2 point :
         {DVec2{0.5, 0.5}, DVec2{0.1, 0.2}, DVec2{0.9, 0.7}, DVec2{0.3, 0.95}}) {
        check_near(hog::viewer::direction_point(hog::viewer::map_direction(point)), point);
    }
    check_near(hog::viewer::direction_point(DVec3{0.0, 0.0, 3.0}), DVec2{0.5, 0.0});
}

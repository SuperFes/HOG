#include <hog/window/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

namespace {

// Windows that draw nowhere, so the tests run without a display.
const hog::window::Sdl& offscreen() {
    static const bool hinted = SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    static const hog::window::Sdl sdl;
    (void)hinted;
    return sdl;
}

} // namespace

TEST_CASE("a window's drawable size is its size in pixels", "[window]") {
    REQUIRE(offscreen().started());
    const hog::window::Window window{SDL_CreateWindow("test", 320, 200, 0)};
    // Some drivers start but make no windows without a display, as macOS's runners have none.
    if (!window) {
        SKIP(SDL_GetError());
    }
    const VkExtent2D extent = hog::window::drawable(window.get());
    CHECK(extent.width == 320);
    CHECK(extent.height == 200);
}

TEST_CASE("a device for a window keeps what was asked and adds the surface's needs", "[window]") {
    if (!offscreen().vulkan()) {
        SKIP("no Vulkan");
    }
    const hog::window::Window window{SDL_CreateWindow("test", 64, 64, SDL_WINDOW_VULKAN)};
    REQUIRE(window);
    const auto options = hog::window::device_options(
        window.get(), {.app_name = "test", .validation = true, .instance_extensions = {"mine"}});
    REQUIRE(options);
    CHECK(options->app_name == "test");
    CHECK(options->validation);
    CHECK(options->instance_extensions.front() == "mine");
    CHECK(std::ranges::contains(options->instance_extensions, std::string{"VK_KHR_surface"}));
    CHECK(options->make_surface);
}

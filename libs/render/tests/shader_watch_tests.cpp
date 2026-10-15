#include "test_device.hpp"

#include <hog/render/device.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/shader_watch.hpp>
#include <hog/shaders/fill.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using hog::render::read_depfile;
using hog::render::ShaderCatalogue;
using hog::render::ShaderSource;
using hog::render::ShaderWatch;
using hog::testing::TemporaryDirectory;

namespace {

constexpr std::uint32_t spirv_magic = 0x07230203;

constexpr std::string_view shine = R"(import colour;

[shader("vertex")]
float4 vertex_main(uint id: SV_VertexID) : SV_Position {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * 2.0 - 1.0, 0.5, 1.0);
}

[shader("fragment")]
float4 fragment_main() : SV_Target {
    return colour();
}
)";

constexpr std::string_view red = R"(module colour;

public float4 colour() {
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

constexpr std::string_view green = R"(module colour;

public float4 colour() {
    return float4(0.0, 1.0, 0.0, 1.0);
}
)";

// Written, and later than anything written before, however coarse the file system's times.
void write(const std::filesystem::path& path, std::string_view text) {
    static auto time = std::filesystem::file_time_type::clock::now();
    std::ofstream{path, std::ios::binary} << text;
    time += std::chrono::seconds{2};
    std::filesystem::last_write_time(path, time);
}

// Polls until nothing is compiling, gathering what changed.
ShaderWatch::Changes settle(ShaderWatch& watch) {
    ShaderWatch::Changes all;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    do {
        ShaderWatch::Changes changes = watch.poll();
        for (auto& reloaded : changes.reloaded) {
            all.reloaded.push_back(std::move(reloaded));
        }
        for (auto& failed : changes.failed) {
            all.failed.push_back(std::move(failed));
        }
        if (watch.compiling()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    } while (watch.compiling() && std::chrono::steady_clock::now() < give_up);
    return all;
}

// A shader named shine in a directory of its own, importing colour beside it.
struct Shine {
    explicit Shine(bool with_depfile = false) {
        write(directory.path() / "colour.slang", red);
        write(directory.path() / "shine.slang", shine);
        if (with_depfile) {
            write(depfile, std::format("{}: {} {}\n", (directory.path() / "shine.spv").string(),
                                       source, (directory.path() / "colour.slang").string()));
        }
        sources = {ShaderSource{.name = "shine",
                                .source = source,
                                .depends = with_depfile ? std::string_view{depfile} : "",
                                .embedded = hog::shaders::fill}};
    }

    [[nodiscard]] ShaderCatalogue catalogue() const {
        const ShaderCatalogue& built = hog::render::shader_catalogue();
        return {.compiler = built.compiler, .shaders = sources, .scratch = scratch};
    }

    TemporaryDirectory directory;
    std::string source = (directory.path() / "shine.slang").string();
    std::string depfile = (directory.path() / "shine.spv.d").string();
    std::string scratch = (directory.path() / "compiled").string();
    std::array<ShaderSource, 1> sources{};
};

} // namespace

TEST_CASE("a depfile names what its targets depend on", "[render][shader_watch]") {
    CHECK(read_depfile("").empty());
    CHECK(read_depfile("out.spv: a.slang b.slang\n") ==
          std::vector<std::filesystem::path>{"a.slang", "b.slang"});
    CHECK(read_depfile("out.spv: a.slang \\\n  b.slang\\\r\n c.slang") ==
          std::vector<std::filesystem::path>{"a.slang", "b.slang", "c.slang"});
    CHECK(read_depfile("my\\ out.spv: my\\ shader.slang") ==
          std::vector<std::filesystem::path>{"my shader.slang"});
}

TEST_CASE("the renderer's shaders are catalogued as they were built", "[render][shader_watch]") {
    const ShaderCatalogue& catalogue = hog::render::shader_catalogue();
    CHECK_FALSE(catalogue.compiler.slangc.empty());
    CHECK_FALSE(catalogue.scratch.empty());
    REQUIRE_FALSE(catalogue.shaders.empty());
    for (const ShaderSource& shader : catalogue.shaders) {
        CAPTURE(shader.name);
        CHECK(std::filesystem::exists(shader.source));
        CHECK(std::filesystem::exists(shader.depends));
        REQUIRE_FALSE(shader.embedded.empty());
        CHECK(shader.embedded.front() == spirv_magic);
    }
}

TEST_CASE("a watched shader is compiled again when it changes", "[render][shader_watch]") {
    const Shine shader;
    ShaderWatch watch{shader.catalogue()};
    CHECK(settle(watch).reloaded.empty());

    write(shader.source, std::string{shine} + "\n// A comment\n");
    // Other than the fill shader standing in for what's embedded.
    const auto changed = settle(watch);
    CHECK(changed.failed.empty());
    REQUIRE(changed.reloaded.size() == 1);
    CHECK(changed.reloaded[0].name == "shine");
    CHECK(changed.reloaded[0].embedded.data() == hog::shaders::fill.data());
    REQUIRE_FALSE(changed.reloaded[0].spirv.empty());
    CHECK(changed.reloaded[0].spirv.front() == spirv_magic);

    // Comes out as it was.
    write(shader.source, std::string{shine} + "\n// Another comment\n");
    const auto same = settle(watch);
    CHECK(same.failed.empty());
    CHECK(same.reloaded.empty());

    // Then a module it imports changes, which its first compile found it importing.
    write(shader.directory.path() / "colour.slang", green);
    const auto imported = settle(watch);
    CHECK(imported.failed.empty());
    CHECK(imported.reloaded.size() == 1);
}

TEST_CASE("a shader that doesn't compile is reported, and kept watching",
          "[render][shader_watch]") {
    const Shine shader;
    ShaderWatch watch{shader.catalogue()};
    write(shader.source, "this isn't Slang");
    const auto broken = settle(watch);
    CHECK(broken.reloaded.empty());
    REQUIRE(broken.failed.size() == 1);
    CHECK(broken.failed[0].name == "shine");
    CHECK(broken.failed[0].message.find("error") != std::string::npos);

    write(shader.source, shine);
    const auto fixed = settle(watch);
    CHECK(fixed.failed.empty());
    CHECK(fixed.reloaded.size() == 1);
}

TEST_CASE("a shader's build depfile says what it imports", "[render][shader_watch]") {
    const Shine shader{true};
    ShaderWatch watch{shader.catalogue()};
    CHECK(settle(watch).reloaded.empty());
    write(shader.directory.path() / "colour.slang", green);
    const auto imported = settle(watch);
    CHECK(imported.failed.empty());
    CHECK(imported.reloaded.size() == 1);
}

TEST_CASE("a shader changed since it was built is compiled again at once",
          "[render][shader_watch]") {
    const Shine shader{true};
    // Later than its depfile.
    write(shader.source, std::string{shine} + "\n// Edited since\n");
    ShaderWatch watch{shader.catalogue()};
    const auto changed = settle(watch);
    CHECK(changed.failed.empty());
    CHECK(changed.reloaded.size() == 1);
}

TEST_CASE("the pipelines of a shader compiled again are made again",
          "[render][gpu][shader_watch]") {
    const auto device = hog::render::testing::test_device();
    constexpr std::uint32_t fill_push_bytes = 20;
    const auto pipeline = hog::render::make_pipeline(
        *device, {.spirv = hog::shaders::fill, .push_constant_bytes = fill_push_bytes});
    REQUIRE(pipeline);
    auto* const was = pipeline->handle();
    const Shine shader;
    ShaderWatch watch{shader.catalogue()};
    write(shader.source, std::string{shine} + "\n// A comment\n");
    std::size_t remade = 0;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    do {
        remade += hog::render::reload_changed(watch, device->pipelines());
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    } while (watch.compiling() && std::chrono::steady_clock::now() < give_up);
    remade += hog::render::reload_changed(watch, device->pipelines());
    CHECK(remade >= 1);
    CHECK(pipeline->handle() != was);
    CHECK(device->validation_errors() == 0);
}

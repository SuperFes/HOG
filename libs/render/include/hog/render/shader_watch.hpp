#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hog::render {

// Where a shader built into the binary came from.
struct ShaderSource {
    std::string_view name;
    std::string_view source; // its .slang file
    // The depfile its build wrote, naming the modules it imports too; it needn't be there.
    std::string_view depends;
    std::span<const std::uint32_t> embedded;
};

struct ShaderCompiler {
    std::string_view slangc;
    // Between a shader's source and its output.
    std::span<const std::string_view> arguments;
};

struct ShaderCatalogue {
    ShaderCompiler compiler;
    std::span<const ShaderSource> shaders;
    std::string_view scratch; // a directory to compile into
};

// The renderer's own shaders, as they were built.
[[nodiscard]] const ShaderCatalogue& shader_catalogue() noexcept;

// The files a make-style depfile says its targets depend on.
[[nodiscard]] std::vector<std::filesystem::path> read_depfile(std::string_view text);

struct ReloadedShader {
    std::string_view name;
    std::span<const std::uint32_t> embedded; // what it was built into the binary as
    std::vector<std::uint32_t> spirv;
};

struct ShaderFailure {
    std::string_view name;
    std::string message;
};

// Watches shaders' sources and what they import, compiling them again apart from the caller when
// they change.
class ShaderWatch {
public:
    explicit ShaderWatch(const ShaderCatalogue& catalogue);
    ShaderWatch(const ShaderWatch&) = delete;
    ShaderWatch& operator=(const ShaderWatch&) = delete;
    ShaderWatch(ShaderWatch&&) = delete;
    ShaderWatch& operator=(ShaderWatch&&) = delete;
    ~ShaderWatch() = default;

    struct Changes {
        std::vector<ReloadedShader> reloaded;
        std::vector<ShaderFailure> failed;
    };

    // What's finished compiling since last asked, leaving out shaders that came out as they were;
    // then, if nothing is compiling, starts on any that changed since.
    [[nodiscard]] Changes poll();

    // Whether shaders are compiling still.
    [[nodiscard]] bool compiling() const noexcept { return compiling_.valid(); }

private:
    struct Watched {
        const ShaderSource* shader = nullptr;
        std::vector<std::filesystem::path> files;
        std::vector<std::filesystem::file_time_type> times;
        std::vector<std::uint32_t> spirv; // last compiled, empty for what's embedded
    };
    struct Job {
        std::size_t shader = 0;
        std::string name;
        std::string source;
    };
    struct Compiled {
        std::size_t shader = 0;
        std::vector<std::uint32_t> spirv; // empty where it failed
        std::string message;
        std::vector<std::filesystem::path> files;
    };

    void collect(Compiled compiled, Changes& changes);
    // Compiles the shaders changed since last looked at, if any, apart from the caller.
    void start();
    [[nodiscard]] static Compiled compile(const ShaderCompiler& compiler,
                                          const std::filesystem::path& scratch, const Job& job);

    ShaderCompiler compiler_;
    std::filesystem::path scratch_;
    std::vector<Watched> watched_;
    std::future<std::vector<Compiled>> compiling_;
};

} // namespace hog::render

#include <hog/core/log.hpp>
#include <hog/core/text_file.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/shader_watch.hpp>
#include <hog/shaders/catalogue.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace hog::render {

namespace {

const log::Category shaders_log{"shaders"};

std::string read_text(const std::filesystem::path& path) {
    return read_text_file(path).value_or(std::string{});
}

std::filesystem::file_time_type modified(const std::filesystem::path& path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? std::filesystem::file_time_type::min() : time;
}

// A shader's files: those its depfile names, or its source alone without one.
std::vector<std::filesystem::path> files_of(const ShaderSource& shader) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!shader.depends.empty() && std::filesystem::exists(shader.depends, error)) {
        files = read_depfile(read_text(shader.depends));
    }
    if (std::ranges::find(files, std::filesystem::path{shader.source}) == files.end()) {
        files.emplace_back(shader.source);
    }
    return files;
}

std::string shell_quoted(std::string_view argument) {
#ifdef _WIN32
    return std::string{"\""} + std::string{argument} + "\"";
#else
    std::string quoted = "'";
    for (const char c : argument) {
        quoted += c == '\'' ? std::string{"'\\''"} : std::string{c};
    }
    return quoted + "'";
#endif
}

} // namespace

const ShaderCatalogue& shader_catalogue() noexcept {
    return shaders::catalogue;
}

std::vector<std::filesystem::path> read_depfile(std::string_view text) {
    std::vector<std::filesystem::path> files;
    std::string word;
    // Targets end with a colon; what they depend on doesn't, though a path may hold one escaped,
    // as Windows' drive letters do.
    bool target = false;
    const auto finish = [&] {
        if (!word.empty() && !target) {
            files.emplace_back(word);
        }
        word.clear();
        target = false;
    };
    // As slangc escapes them.
    constexpr std::string_view escaped = " #:[]\\";
    for (std::size_t at = 0; at < text.size(); ++at) {
        const char c = text[at];
        const char next = at + 1 < text.size() ? text[at + 1] : '\0';
        if (c == '\\' && escaped.contains(next)) {
            word += next;
            target = false;
            ++at;
            continue;
        }
        if (c == '\\' && (next == '\n' || next == '\r')) {
            finish();
            ++at;
            continue;
        }
        if (c == '$' && next == '$') {
            word += c;
            target = false;
            ++at;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            finish();
        } else {
            word += c;
            target = c == ':';
        }
    }
    finish();
    return files;
}

ShaderWatch::ShaderWatch(const ShaderCatalogue& catalogue)
    : compiler_(catalogue.compiler), scratch_(catalogue.scratch) {
    for (const ShaderSource& shader : catalogue.shaders) {
        Watched watched{.shader = &shader, .files = files_of(shader), .times = {}, .spirv = {}};
        const auto built = shader.depends.empty() ? std::filesystem::file_time_type::max()
                                                  : modified(shader.depends);
        for (const auto& file : watched.files) {
            const auto time = modified(file);
            // Changed since it was built, so it's compiled again on the first poll.
            watched.times.push_back(time > built ? std::filesystem::file_time_type::min() : time);
        }
        watched_.push_back(std::move(watched));
    }
}

ShaderWatch::Changes ShaderWatch::poll() {
    Changes changes;
    if (compiling_.valid()) {
        if (compiling_.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return changes;
        }
        for (Compiled& compiled : compiling_.get()) {
            collect(std::move(compiled), changes);
        }
    }
    start();
    return changes;
}

void ShaderWatch::collect(Compiled compiled, Changes& changes) {
    Watched& watched = watched_[compiled.shader];
    if (compiled.spirv.empty()) {
        changes.failed.push_back(
            {.name = watched.shader->name, .message = std::move(compiled.message)});
        return;
    }
    // What it imports may have changed with it.
    for (auto& file : compiled.files) {
        if (std::ranges::find(watched.files, file) == watched.files.end()) {
            watched.times.push_back(modified(file));
            watched.files.push_back(std::move(file));
        }
    }
    const std::span<const std::uint32_t> was =
        watched.spirv.empty() ? watched.shader->embedded : watched.spirv;
    if (std::ranges::equal(compiled.spirv, was)) {
        return;
    }
    watched.spirv = compiled.spirv;
    changes.reloaded.push_back({.name = watched.shader->name,
                                .embedded = watched.shader->embedded,
                                .spirv = std::move(compiled.spirv)});
}

void ShaderWatch::start() {
    std::vector<Job> jobs;
    for (std::size_t shader = 0; shader < watched_.size(); ++shader) {
        Watched& watched = watched_[shader];
        bool changed = false;
        for (std::size_t file = 0; file < watched.files.size(); ++file) {
            const auto time = modified(watched.files[file]);
            changed = changed || time != watched.times[file];
            watched.times[file] = time;
        }
        if (changed) {
            jobs.push_back({.shader = shader,
                            .name = std::string{watched.shader->name},
                            .source = std::string{watched.shader->source}});
        }
    }
    if (jobs.empty()) {
        return;
    }
    compiling_ = std::async(std::launch::async,
                            [compiler = compiler_, scratch = scratch_, jobs = std::move(jobs)] {
                                std::error_code ignored;
                                std::filesystem::create_directories(scratch, ignored);
                                std::vector<Compiled> compiled;
                                compiled.reserve(jobs.size());
                                for (const Job& job : jobs) {
                                    compiled.push_back(compile(compiler, scratch, job));
                                }
                                return compiled;
                            });
}

ShaderWatch::Compiled ShaderWatch::compile(const ShaderCompiler& compiler,
                                           const std::filesystem::path& scratch, const Job& job) {
    const auto spirv = scratch / (job.name + ".spv");
    const auto depfile = scratch / (job.name + ".spv.d");
    const auto log = scratch / (job.name + ".log");
    std::error_code ignored;
    std::filesystem::remove(spirv, ignored);
    std::string command = shell_quoted(compiler.slangc) + " " + shell_quoted(job.source);
    for (const std::string_view argument : compiler.arguments) {
        command += " " + shell_quoted(argument);
    }
    command += " -o " + shell_quoted(spirv.string()) + " -depfile " +
               shell_quoted(depfile.string()) + " > " + shell_quoted(log.string()) + " 2>&1";
#ifdef _WIN32
    // cmd strips the outermost quotes.
    command = "\"" + command + "\"";
#endif
    Compiled result{.shader = job.shader, .spirv = {}, .message = {}, .files = {}};
    // The build's own slangc on its own shaders, one at a time.
    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c,concurrency-mt-unsafe)
    const std::string bytes = std::system(command.c_str()) == 0 ? read_text(spirv) : std::string{};
    if (bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) {
        result.message = read_text(log);
        if (result.message.empty()) {
            result.message = "slangc gave no SPIR-V";
        }
        return result;
    }
    result.spirv.resize(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(result.spirv.data(), bytes.data(), bytes.size());
    result.files = read_depfile(read_text(depfile));
    return result;
}

std::size_t reload_changed(ShaderWatch& watch, PipelineRegistry& pipelines) {
    const ShaderWatch::Changes changes = watch.poll();
    for (const ShaderFailure& failed : changes.failed) {
        log::error(shaders_log, "{} doesn't compile:\n{}", failed.name, failed.message);
    }
    std::size_t remade = 0;
    for (const ReloadedShader& reloaded : changes.reloaded) {
        const auto made = pipelines.remake(reloaded.embedded, reloaded.spirv);
        if (!made) {
            log::error(shaders_log, "can't reload {}: {}", reloaded.name, made.error());
            continue;
        }
        log::info(shaders_log, "reloaded {} into {} pipeline{}", reloaded.name, *made,
                  *made == 1 ? "" : "s");
        remade += *made;
    }
    return remade;
}

} // namespace hog::render

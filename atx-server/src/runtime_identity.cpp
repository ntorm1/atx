#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#include <atx/server/runtime_identity.hpp>
#include <atx/core/sha256.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace atx::server {
namespace {
constexpr std::uintmax_t file_limit = 512ULL * 1024ULL * 1024ULL;
constexpr std::uintmax_t total_limit = 1024ULL * 1024ULL * 1024ULL;
std::string utf8_name(const std::filesystem::path& path) {
    const auto text = path.filename().u8string();
    return {text.begin(), text.end()};
}
bool native_library(const std::filesystem::path& path) {
    auto name = utf8_name(path);
    for (auto& ch : name)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return name.ends_with(".dll") || name.ends_with(".dylib") || name.ends_with(".so") ||
        name.find(".so.") != std::string::npos;
}
nlohmann::json hash_artifact(const std::filesystem::path& path, std::uintmax_t& total) {
    const auto size = std::filesystem::file_size(path);
    const auto modified = std::filesystem::last_write_time(path);
    if (size == 0 || size > file_limit || size > total_limit - total)
        throw std::runtime_error("runtime artifact exceeds content receipt limits");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("runtime artifact is unreadable");
    atx::core::Sha256 hash;
    std::vector<char> buffer(64U * 1024U);
    std::uintmax_t consumed{};
    for (; consumed < size;) {
        const auto remaining = std::min<std::uintmax_t>(buffer.size(), size - consumed);
        stream.read(buffer.data(), static_cast<std::streamsize>(remaining));
        if (stream.gcount() != static_cast<std::streamsize>(remaining) || stream.bad())
            throw std::runtime_error("runtime artifact read was incomplete");
        const auto count = static_cast<std::size_t>(remaining);
        const auto status = hash.update(std::as_bytes(std::span(buffer.data(), count)));
        if (!status) throw std::runtime_error("runtime artifact hashing failed");
        consumed += remaining;
    }
    if (stream.peek() != std::char_traits<char>::eof() || stream.bad() ||
        std::filesystem::file_size(path) != size ||
        std::filesystem::last_write_time(path) != modified)
        throw std::runtime_error("runtime artifact changed while being fingerprinted");
    const auto digest = hash.finalize();
    if (!digest) throw std::runtime_error("runtime artifact hashing failed");
    std::string hex;
    hex.reserve(64);
    for (const auto byte : *digest) {
        const auto value = std::to_integer<unsigned>(byte);
        hex.push_back("0123456789abcdef"[value >> 4]);
        hex.push_back("0123456789abcdef"[value & 15]);
    }
    total += size;
    return {{"name", utf8_name(path)}, {"bytes", size}, {"sha256", hex}};
}
} // namespace

RuntimeBuildIdentity identify_runtime(const std::filesystem::path& executable) {
    const auto absolute = std::filesystem::canonical(executable);
    if (!std::filesystem::is_regular_file(absolute))
        throw std::runtime_error("runtime executable is not a regular file");
    std::vector<std::filesystem::path> files{absolute};
    std::size_t examined{};
    for (const auto& entry : std::filesystem::directory_iterator(absolute.parent_path())) {
        if (++examined > 4096)
            throw std::runtime_error("runtime directory exceeds content receipt entry limit");
        if (entry.path() == absolute || !native_library(entry.path())) continue;
        if (!entry.is_regular_file())
            throw std::runtime_error("runtime library is not a regular file");
        if (files.size() >= 64)
            throw std::runtime_error("runtime distribution exceeds artifact count limit");
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return utf8_name(left) < utf8_name(right);
    });
    std::uintmax_t total{};
    auto artifacts = nlohmann::json::array();
    for (const auto& file : files) artifacts.push_back(hash_artifact(file, total));
    auto manifest = nlohmann::json{{"schema_version", "atx.runtime-build/v1"},
        {"scope", "executable_and_adjacent_native_libraries"},
        {"system_libraries_included", false}, {"artifacts", std::move(artifacts)}}.dump();
    const auto digest = atx::core::sha256_hex(manifest);
    if (!digest) throw std::runtime_error("runtime manifest hashing failed");
    return {*digest, std::move(manifest)};
}

std::filesystem::path current_executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        throw std::runtime_error("cannot resolve current executable path");
    return std::filesystem::canonical(std::filesystem::path(path.data(), path.data() + length));
#elif defined(__linux__)
    return std::filesystem::canonical("/proc/self/exe");
#elif defined(__APPLE__)
    std::array<char, 32768> path{};
    auto length = static_cast<std::uint32_t>(path.size());
    if (_NSGetExecutablePath(path.data(), &length) != 0)
        throw std::runtime_error("cannot resolve current executable path");
    return std::filesystem::canonical(path.data());
#else
    throw std::runtime_error("runtime executable discovery is unsupported on this platform");
#endif
}
} // namespace atx::server

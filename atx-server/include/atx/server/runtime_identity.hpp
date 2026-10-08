#pragma once

#include <filesystem>
#include <string>

namespace atx::server {
struct RuntimeBuildIdentity {
    std::string sha256;
    std::string manifest_json;
};
// Content identity of the executable and adjacent native distribution libraries.
// Excludes operating-system libraries, absolute paths and file timestamps.
// Bounds: 64 artifacts, 4096 directory entries, 512 MiB per artifact, 1 GiB total.
// Throws on missing, unreadable, changing or oversized files; never returns a partial receipt.
[[nodiscard]] RuntimeBuildIdentity identify_runtime(const std::filesystem::path& executable);
// Resolves the currently running process image without trusting a PATH lookup.
[[nodiscard]] std::filesystem::path current_executable_path();
} // namespace atx::server

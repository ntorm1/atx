#pragma once

// atx::engine::research::store::catalog -- what the catalog may open, and how it spells a path
// (P9 SQL2; sql-design section 3.9, ruling SQL-7).
//
// Seal backstop: a root-relative path holding a standalone year token 2024-2099 (the regex
// (^|\D)20(2[4-9]|[3-9]\d)(\D|$) over the '/'-separated path) is never opened, and a directory
// so named is never descended into; it is listed by name (skipped_path, reason seal-name).
// Size: a file over kMaxOpenBytes is never opened; its SHA-256 is the one a holder declares
// (artifact.sha_source = declared), or it is listed (declared-only). Only --verify-payloads
// hashes such a file (and still never a sealed one).
//
// Paths: every catalogued path is root-relative with '/' separators (the `relpath` CHECK: no
// '\', no leading '/', no '..' segment). path_key = that path with ASCII letters lower-cased
// (NTFS folds case; the research_gc C-12 precedent).

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "atx/core/types.hpp"

namespace atx::engine::research::store::catalog {

inline constexpr u64 kMaxOpenBytes = 16ULL * 1024ULL * 1024ULL;

// True when `path` holds a standalone year token 2024-2099 (see the header comment).
[[nodiscard]] bool has_sealed_year(std::string_view path) noexcept;

// True for a non-empty root-relative '/'-path without '\', a leading '/' or a '..' segment.
[[nodiscard]] bool is_relpath(std::string_view path) noexcept;

// `path` with ASCII letters lower-cased.
[[nodiscard]] std::string path_key(std::string_view path);

// A UTF-8 path text as a filesystem path, and a filesystem path as '/'-separated UTF-8.
[[nodiscard]] std::filesystem::path fs_path(std::string_view utf8);
[[nodiscard]] std::string utf8_path(const std::filesystem::path &path);

// The root-relative '/'-path a holder's path text names, or nullopt when it names nothing
// inside `root`: `text` may be absolute (compared with `root` case-insensitively, lexically,
// without touching the file system) or relative to the root; '\' is read as '/'; '.' and '..'
// are resolved lexically; a result that leaves the root is nullopt.
// @param root an absolute, lexically normal path (see normal_root).
[[nodiscard]] std::optional<std::string> root_relative(const std::filesystem::path &root,
                                                       std::string_view text);

// `root` made absolute and lexically normal, without a trailing separator.
[[nodiscard]] std::filesystem::path normal_root(const std::filesystem::path &root);

// The parent directory of a '/'-path ("" for a top-level name) and its last segment.
[[nodiscard]] std::string parent_of(std::string_view path);
[[nodiscard]] std::string leaf_of(std::string_view path);

// `a` + "/" + `b`, or `b` when `a` is empty.
[[nodiscard]] std::string join_rel(std::string_view a, std::string_view b);

} // namespace atx::engine::research::store::catalog

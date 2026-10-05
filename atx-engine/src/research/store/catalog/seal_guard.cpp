// The seal backstop and the catalog's path spelling (seal_guard.hpp).

#include "atx/engine/research/store/catalog/seal_guard.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "atx/core/types.hpp"

namespace atx::engine::research::store::catalog {
namespace {

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string slashes(std::string_view text) {
  std::string out{text};
  for (char &c : out) {
    if (c == '\\') {
      c = '/';
    }
  }
  return out;
}

[[nodiscard]] std::string trim_trailing_slash(std::string text) {
  while (text.size() > 1 && text.back() == '/') {
    text.pop_back();
  }
  return text;
}

// The most ancestors repo_top visits: far more segments than any path this host can name, so the
// walk always ends at the file-system root first; the bound only makes the loop's end obvious.
constexpr usize kMaxAncestors = 4096;

// True when `dir` holds a `.git` entry (a directory in a clone, a file in a worktree). A file
// system error reads as no entry, which keeps the whole-path check (fail closed).
[[nodiscard]] bool holds_git_entry(const std::filesystem::path &dir) {
  std::error_code ec;
  const bool found = std::filesystem::exists(dir / ".git", ec);
  return found && !ec;
}

// The OUTERMOST directory among `normal` and its ancestors that holds a `.git` entry, or nullopt.
// Outermost, not nearest: a `.git` planted inside a sealed directory must not exempt it.
[[nodiscard]] std::optional<std::filesystem::path> repo_top(const std::filesystem::path &normal) {
  std::optional<std::filesystem::path> top;
  std::filesystem::path dir = normal;
  for (usize i = 0; i < kMaxAncestors; ++i) {
    if (holds_git_entry(dir)) {
      top = dir;
    }
    std::filesystem::path parent = dir.parent_path();
    if (parent.empty() || parent == dir) {
      break;
    }
    dir = std::move(parent);
  }
  return top;
}

} // namespace

bool has_sealed_year(std::string_view path) noexcept {
  const usize n = path.size();
  for (usize i = 0; i + 4 <= n; ++i) {
    if (path[i] != '2' || path[i + 1] != '0') {
      continue;
    }
    if (i > 0 && is_digit(path[i - 1])) {
      continue;
    }
    const char c = path[i + 2];
    const char d = path[i + 3];
    if (!is_digit(c) || !is_digit(d)) {
      continue;
    }
    const bool year = (c == '2' && d >= '4') || c >= '3';
    const bool ends = i + 4 == n || !is_digit(path[i + 4]);
    if (year && ends) {
      return true;
    }
  }
  return false;
}

bool is_relpath(std::string_view path) noexcept {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos) {
    return false;
  }
  usize start = 0;
  // Bounded by the path length: each pass consumes one segment.
  while (start <= path.size()) {
    const usize slash = path.find('/', start);
    const usize end = slash == std::string_view::npos ? path.size() : slash;
    if (path.substr(start, end - start) == "..") {
      return false;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

std::string path_key(std::string_view path) {
  std::string out{path};
  for (char &c : out) {
    c = ascii_lower(c);
  }
  return out;
}

std::filesystem::path fs_path(std::string_view utf8) {
  std::u8string text;
  text.reserve(utf8.size());
  for (const char c : utf8) {
    text.push_back(static_cast<char8_t>(c));
  }
  return std::filesystem::path{text};
}

std::string utf8_path(const std::filesystem::path &path) {
  const std::u8string text = path.generic_u8string();
  std::string out;
  out.reserve(text.size());
  for (const char8_t c : text) {
    out.push_back(static_cast<char>(c));
  }
  return out;
}

std::filesystem::path normal_root(const std::filesystem::path &root) {
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(root, ec);
  if (ec) {
    abs = root;
  }
  return fs_path(trim_trailing_slash(utf8_path(abs.lexically_normal())));
}

bool sealed_root(const std::filesystem::path &root) {
  const std::filesystem::path normal = normal_root(root);
  const std::optional<std::filesystem::path> top = repo_top(normal);
  if (!top) {
    return has_sealed_year(utf8_path(normal));
  }
  // SQL2-S7 (ruling W3-4): the repository top's own name and its ancestors are the host's
  // environment (a dated sprint dir, a pytest-2024 temp dir); only the part below it is scanned.
  const std::filesystem::path below = normal.lexically_relative(*top);
  if (below.empty()) {
    return has_sealed_year(utf8_path(normal)); // not expressible below the top: fail closed
  }
  const std::string rest = utf8_path(below);
  return rest != "." && has_sealed_year(rest);
}

std::string sealed_root_message(const std::filesystem::path &root) {
  return "the root " + utf8_path(normal_root(root)) +
         " holds a year token 2024-2099: nothing under it is opened (seal)";
}

std::optional<std::string> root_relative(const std::filesystem::path &root, std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path given = fs_path(slashes(text));
  if (given.is_absolute()) {
    const std::string whole = trim_trailing_slash(utf8_path(given.lexically_normal()));
    const std::string base = utf8_path(root);
    if (whole.size() <= base.size() + 1 ||
        path_key(whole.substr(0, base.size())) != path_key(base) || whole[base.size()] != '/') {
      return std::nullopt;
    }
    std::string rel = whole.substr(base.size() + 1);
    return is_relpath(rel) ? std::optional<std::string>{std::move(rel)} : std::nullopt;
  }
  if (given.has_root_name() || given.has_root_directory()) {
    return std::nullopt; // "C:x" or "/x": not a root-relative spelling
  }
  std::string rel = trim_trailing_slash(utf8_path(given.lexically_normal()));
  if (rel == "." || rel.empty() || !is_relpath(rel)) {
    return std::nullopt;
  }
  return rel;
}

std::string parent_of(std::string_view path) {
  const usize slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0, slash)};
}

std::string leaf_of(std::string_view path) {
  const usize slash = path.rfind('/');
  return std::string{slash == std::string_view::npos ? path : path.substr(slash + 1)};
}

std::string join_rel(std::string_view a, std::string_view b) {
  if (a.empty()) {
    return std::string{b};
  }
  std::string out{a};
  out += '/';
  out += b;
  return out;
}

} // namespace atx::engine::research::store::catalog

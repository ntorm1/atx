// The seal backstop and the catalog's path spelling (seal_guard.hpp).

#include "atx/engine/research/store/catalog/seal_guard.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

#pragma once
// ── atx::vol::alpha — panel schema as data, not as an enumerator ────────────
//
// WHAT THIS REPLACES. `src/analytics/vrp_panel.hpp` identifies a panel by
// `enum class VrpPanelSchema { V1, V2, V3, V4 }`, with four hand-maintained
// `kVrpPanelColumnsVN` arrays and a reader that switches on a
// `# schema=vrp_panel_vN` comment. Every new column costs an enumerator, an
// array, a `kVrpPanelColumnCountVN`, a CLI branch, and a reader branch -- five
// edits before the column means anything.
//
// THE RULE HERE: THE HEADER LINE IS THE SCHEMA. Columns are resolved by NAME
// at load. A panel that gains a column gains a FINGERPRINT, not a version, and
// a reader that does not need the new column never notices it exists.
//
// What a version enum bought that a name map must keep buying:
//
//   * PROVENANCE -- "which columns was this artifact written with". Kept, and
//     strengthened: `fingerprint()` hashes the ORDERED name list, so two runs
//     agree only if they agree on names AND order, where `V3` only ever meant
//     "someone said V3".
//   * REFUSAL -- "this file lacks a column I require". Kept by `require()`,
//     which reports EVERY missing name at once rather than failing on the
//     first, because a caller fixing a column list wants the whole list.
//   * ORDER STABILITY -- byte-deterministic output across stitched runs. Kept:
//     `PanelSchema` preserves insertion order and never sorts.
//
// What it stops buying, on purpose: the belief that column order is a frozen
// contract. It is a RECORDED fact here. A reader resolving by name survives a
// reordering; the fingerprint still notices one happened.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol::alpha {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// FNV-1a over the ordered, NUL-separated column names. Chosen over a real hash
// because this is an identity tag for humans reading run metadata, not a
// security boundary: it must be stable across platforms and cheap enough to
// stamp on every artifact. The separator is what stops {"ab","c"} colliding
// with {"a","bc"}.
[[nodiscard]] inline std::uint64_t fingerprint_of(const std::vector<std::string> &names) noexcept {
  constexpr std::uint64_t kOffset = 1469598103934665603ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t h = kOffset;
  for (const std::string &name : names) {
    for (const char c : name) {
      h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
      h *= kPrime;
    }
    h ^= 0x00ULL;
    h *= kPrime;
  }
  return h;
}

class PanelSchema {
public:
  PanelSchema() = default;

  // Duplicate names are rejected: a panel with two `f4_term_slope` columns
  // cannot be resolved by name, and silently taking the first is how a run
  // trains on the wrong column.
  [[nodiscard]] static Result<PanelSchema> from_columns(std::vector<std::string> names) {
    if (names.empty()) {
      return Err(ErrorCode::InvalidArgument, "alpha::PanelSchema: no columns");
    }
    PanelSchema schema;
    schema.names_ = std::move(names);
    schema.index_.reserve(schema.names_.size());
    for (std::size_t i = 0; i < schema.names_.size(); ++i) {
      const std::string &name = schema.names_[i];
      if (name.empty()) {
        return Err(ErrorCode::ParseError,
                   "alpha::PanelSchema: empty column name at position " + std::to_string(i));
      }
      const auto [it, inserted] = schema.index_.emplace(name, i);
      (void)it;
      if (!inserted) {
        return Err(ErrorCode::AlreadyExists,
                   "alpha::PanelSchema: duplicate column '" + name + "' at position " +
                       std::to_string(i));
      }
    }
    schema.fingerprint_ = fingerprint_of(schema.names_);
    return Ok(std::move(schema));
  }

  // Split a TAB-separated header line. A leading '#' is stripped so a panel
  // that comments its header parses the same as one that does not; trailing
  // \r is stripped so a CRLF file parses the same as an LF one (the
  // SpiderRock reference TSVs in this repo are CRLF).
  [[nodiscard]] static Result<PanelSchema> from_header(std::string_view line, char delim = '\t') {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.remove_suffix(1);
    }
    if (!line.empty() && line.front() == '#') {
      line.remove_prefix(1);
      while (!line.empty() && line.front() == ' ') {
        line.remove_prefix(1);
      }
    }
    if (line.empty()) {
      return Err(ErrorCode::ParseError, "alpha::PanelSchema: empty header line");
    }
    std::vector<std::string> names;
    std::size_t start = 0;
    // Bounded: at most line.size() + 1 iterations.
    for (std::size_t i = 0; i <= line.size(); ++i) {
      if (i == line.size() || line[i] == delim) {
        names.emplace_back(line.substr(start, i - start));
        start = i + 1;
      }
    }
    return from_columns(std::move(names));
  }

  [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }
  [[nodiscard]] bool empty() const noexcept { return names_.empty(); }
  [[nodiscard]] const std::vector<std::string> &names() const noexcept { return names_; }
  [[nodiscard]] std::uint64_t fingerprint() const noexcept { return fingerprint_; }

  [[nodiscard]] bool has(std::string_view name) const noexcept {
    return index_.find(std::string(name)) != index_.end();
  }

  // `npos`-free: an absent column is not a position, so it does not get one.
  [[nodiscard]] Result<std::size_t> index_of(std::string_view name) const {
    const auto it = index_.find(std::string(name));
    if (it == index_.end()) {
      return Err(ErrorCode::NotFound,
                 "alpha::PanelSchema: no column '" + std::string(name) + "'");
    }
    return Ok(it->second);
  }

  [[nodiscard]] std::string_view name_at(std::size_t i) const noexcept {
    return i < names_.size() ? std::string_view(names_[i]) : std::string_view{};
  }

  // Resolve a required column list to positions. Reports EVERY missing name in
  // one error: a caller repairing a column list wants the full set, not a
  // one-at-a-time bisection across N rebuilds.
  [[nodiscard]] Result<std::vector<std::size_t>>
  require(const std::vector<std::string> &wanted) const {
    std::vector<std::size_t> out;
    out.reserve(wanted.size());
    std::string missing;
    for (const std::string &name : wanted) {
      const auto it = index_.find(name);
      if (it == index_.end()) {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += name;
        continue;
      }
      out.push_back(it->second);
    }
    if (!missing.empty()) {
      return Err(ErrorCode::NotFound,
                 "alpha::PanelSchema: panel is missing required column(s): " + missing);
    }
    return Ok(std::move(out));
  }

  // The header line this schema would write. Round-trips `from_header`.
  [[nodiscard]] std::string header_line(char delim = '\t') const {
    std::string out;
    for (std::size_t i = 0; i < names_.size(); ++i) {
      if (i != 0) {
        out.push_back(delim);
      }
      out += names_[i];
    }
    return out;
  }

  // Columns present here and absent from `other`. The diagnostic a reader
  // wants when a panel it was handed is older than the one it expects.
  [[nodiscard]] std::vector<std::string> columns_missing_from(const PanelSchema &other) const {
    std::vector<std::string> out;
    for (const std::string &name : names_) {
      if (!other.has(name)) {
        out.push_back(name);
      }
    }
    return out;
  }

private:
  std::vector<std::string> names_;
  std::unordered_map<std::string, std::size_t> index_;
  std::uint64_t fingerprint_{0};
};

} // namespace atx::vol::alpha

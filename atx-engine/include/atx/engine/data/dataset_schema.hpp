#pragma once

// atx::engine::data — DatasetSchema: vocabulary type for all S6 units.
//
// DatasetSchema describes the SHAPE and SEMANTICS of a Dataset:
//   * what columns it carries (names + dtypes),
//   * what role those columns play in the research pipeline (Price / Feature /
//     Signal / Reference),
//   * point-in-time metadata: pit_delay (reporting-lag days), as-of versioning
//     flag, region, and universe_tag.
//
// ALL later S6 units (S6.2 catalog, S6.3 PIT rail, S6.4 Dataset → Panel bridge,
// etc.) depend on this header verbatim. Keep it minimal and stable.
//
// Free helper: schema_is_coherent — the single "dtype/role coherence" check.

#include <algorithm>
#include <string>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::data {

// Opaque, strictly-ordered date key (caller-supplied, e.g. YYYYMMDD or
// epoch-day). Only requirement: comparable (<=) — as-of resolution in S6.2
// reads date_key <= canonical-date.
using DateKey = atx::i64;

// Opaque instrument id.
using InstKey = atx::u32;

// The semantic role this dataset plays in the research pipeline.
enum class Role : atx::u8 {
  Price,
  Feature,
  Signal,
  Reference,
};

// Storage dtype for a column.
// F64 is the workhorse; I64 and Category are stored as f64 (widened, like
// Panel group/classifier fields).
enum class ColumnDType : atx::u8 {
  F64,
  I64,
  Category,
};

// Point-in-time versioning flag (minimal; resolution deferred to S6.2 catalog).
struct AsOfPolicy {
  bool effective_dated = false; // does this dataset carry effective-date / restatement versioning?
};

// Provenance tag attached to a Dataset at construction time.
struct DatasetProvenance {
  std::string source;      // origin tag, e.g. "user:prices_v1" or "external:sentiment"
  std::string description; // free-text human note
};

// Schema describing the structure and metadata of a Dataset.
struct DatasetSchema {
  std::vector<std::string> columns; // column names, in storage order
  std::vector<ColumnDType> dtypes;  // one per column; columns.size()==dtypes.size()
  Role role = Role::Reference;
  atx::u16 pit_delay = 0; // reporting delay in days (stored; enforced by S6.3)
  std::string region;
  std::string universe_tag;
  AsOfPolicy as_of{};
};

// Returns true iff the schema satisfies the minimal dtype/role coherence rule:
//   * columns is non-empty, AND
//   * columns.size() == dtypes.size(), AND
//   * column names are unique (no duplicates).
//
// Uniqueness matters foundationally: S6.2 (catalog) and S6.3 (PIT rail) index
// datasets by column name, so a duplicate admitted here would alias silently
// downstream. We sort a copy + std::adjacent_find rather than mutate `s`.
[[nodiscard]] inline bool schema_is_coherent(const DatasetSchema &s) noexcept {
  if (s.columns.empty() || s.columns.size() != s.dtypes.size()) {
    return false;
  }
  std::vector<std::string> sorted = s.columns;
  std::sort(sorted.begin(), sorted.end());
  return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
}

} // namespace atx::engine::data

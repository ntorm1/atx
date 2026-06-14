#pragma once

// atx::engine::data — DatasetCatalog: named-dataset registry with as-of
// lookup and lineage tracking.
//
// DatasetCatalog owns a collection of Dataset objects keyed by string name.
// It provides:
//   * register_dataset  — admit a Dataset under a name (validate strictly
//                         ascending dates — required for as-of binary search).
//   * resolve           — look up a Dataset by name; returns a stable const
//                         ref wrapper (valid while the catalog lives and the
//                         entry is not erased — std::map nodes don't relocate).
//   * names             — ALL registered names in ascending order (map
//                         iteration is ordered — deterministic with no extra
//                         sort step).
//   * role_of           — schema role for a registered dataset.
//   * derive            — record that one dataset derives from a set of parents
//                         (direct lineage, not transitive — YAGNI).
//   * lineage           — return the direct parent names (ascending,
//                         de-duplicated). Empty Ok if no derivation recorded.
//   * value_at          — PIT as-of lookup: greatest DateKey ≤ canonical_date.
//                         Returns NaN (not Err) when canonical_date precedes
//                         the first row — the caller distinguishes "no data
//                         yet" from "dataset unknown".
//
// Storage: std::map<string,Dataset> for ascending-name iteration (invariant:
// names() is always deterministically ordered without an extra sort).
// Lineage: std::map<string, vector<string>> (parents sorted-ascending,
// de-duplicated on insert via derive()).
//
// All methods are const-correct; catalog is NOT thread-safe (caller
// synchronises if needed).

#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

class DatasetCatalog {
public:
  DatasetCatalog() = default;

  // -----------------------------------------------------------------------
  //  Registration
  // -----------------------------------------------------------------------

  // Admit `ds` under `name`.
  //
  // Rejects (Err InvalidArgument) if:
  //   * name is empty,
  //   * name is already registered (use replace semantics via a new catalog if
  //     needed — duplicates are almost always a caller bug),
  //   * ds.dates() is NOT strictly ascending (as-of binary search requires an
  //     ordered timeline — validate here once rather than at every value_at).
  //
  // On success, `ds` is moved into the catalog. Subsequent resolve() calls
  // return a reference into the map node — stable for the catalog's lifetime.
  [[nodiscard]] atx::core::Status register_dataset(std::string name, Dataset ds);

  // -----------------------------------------------------------------------
  //  Lookup
  // -----------------------------------------------------------------------

  // Return a stable const reference to the Dataset registered under `name`.
  //
  // Lifetime: the reference_wrapper is valid while:
  //   (a) this DatasetCatalog object is alive, AND
  //   (b) the entry has not been erased.
  // std::map nodes do NOT move on further inserts, so the pointer is stable.
  //
  // Err(NotFound) if name is absent.
  [[nodiscard]] atx::core::Result<std::reference_wrapper<const Dataset>>
  resolve(std::string_view name) const;

  // All registered names in ASCENDING order. std::map iterates in key order,
  // so this is O(n) with no extra sort. Calling twice yields identical results.
  [[nodiscard]] std::vector<std::string> names() const;

  // Convenience: schema role of a registered dataset.
  // Err(NotFound) if absent.
  [[nodiscard]] atx::core::Result<Role> role_of(std::string_view name) const;

  // -----------------------------------------------------------------------
  //  Lineage
  // -----------------------------------------------------------------------

  // Record that `child_name` derives directly from `parent_names`.
  //
  // Rejects (Err InvalidArgument / NotFound) if:
  //   * parent_names is empty,
  //   * child_name is not registered,
  //   * any name in parent_names is not registered.
  //
  // Parents are stored sorted-ascending + de-duplicated (deterministic).
  // Calling derive() twice for the same child REPLACES the previous entry.
  [[nodiscard]] atx::core::Status derive(std::string child_name,
                                         std::vector<std::string> parent_names);

  // Return the direct parents of `name` (ascending, de-duplicated).
  // If no derivation was recorded for `name`, returns Ok({}) — NOT an error.
  // Err(NotFound) only if `name` is not registered at all.
  [[nodiscard]] atx::core::Result<std::vector<std::string>> lineage(std::string_view name) const;

  // -----------------------------------------------------------------------
  //  PIT as-of lookup
  // -----------------------------------------------------------------------

  // Return the value visible AT `canonical_date` for column `col` and
  // instrument `inst` in dataset `name`.
  //
  // Resolution rule (truncation-invariant — no look-ahead):
  //   * Find the greatest DateKey d in dates() such that d ≤ canonical_date.
  //   * Return Ok(column[d_row * num_instruments + inst_idx]).
  //   * If canonical_date < dates()[0] (no qualifying row exists), return
  //     Ok(NaN) — not an error. The caller distinguishes "no data yet" from
  //     "dataset absent" by the Err/Ok shape.
  //
  // Errors (Err NotFound):
  //   * dataset name not registered,
  //   * column name not in schema,
  //   * inst not in the instruments axis.
  [[nodiscard]] atx::core::Result<atx::f64> value_at(std::string_view name, std::string_view col,
                                                     DateKey canonical_date, InstKey inst) const;

private:
  std::map<std::string, Dataset> datasets_;
  // Lineage: child_name -> sorted-ascending de-dup'd parent names.
  std::map<std::string, std::vector<std::string>> lineage_;
};

} // namespace atx::engine::data

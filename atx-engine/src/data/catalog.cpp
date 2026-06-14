// atx::engine::data — DatasetCatalog implementation.
//
// See catalog.hpp for the full API contract and lifetime notes.
// All validation is explicit and returns Result/Status; no exceptions thrown.

#include "atx/engine/data/catalog.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "atx/core/error.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

// ---------------------------------------------------------------------------
//  Helpers (file-local)
// ---------------------------------------------------------------------------

// Look up key in a std::map<string,...> using string_view (avoids allocation).
template <class Map>
static typename Map::const_iterator find_sv(const Map &m, std::string_view key) {
  // std::map::find requires the key type; construct a temporary string only
  // when the key is not found in the map as a string_view-comparable lookup.
  // C++14 heterogeneous lookup requires a transparent comparator; std::map
  // uses std::less<Key> by default which is NOT transparent for string/sv.
  // We use a temporary std::string for the lookup — catalog is cold-path.
  return m.find(std::string{key});
}

// ---------------------------------------------------------------------------
//  register_dataset
// ---------------------------------------------------------------------------

atx::core::Status DatasetCatalog::register_dataset(std::string name, Dataset ds) {
  if (name.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "DatasetCatalog::register_dataset: name must not be empty");
  }
  if (datasets_.count(name) != 0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "DatasetCatalog::register_dataset: duplicate name '" + name + "'");
  }
  if (!is_strictly_ascending(ds.dates())) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "DatasetCatalog::register_dataset: dates() for '" + name +
                              "' are not strictly ascending; as-of resolution requires an "
                              "ordered timeline");
  }
  datasets_.emplace(std::move(name), std::move(ds));
  return atx::core::Ok();
}

// ---------------------------------------------------------------------------
//  resolve
// ---------------------------------------------------------------------------

atx::core::Result<std::reference_wrapper<const Dataset>>
DatasetCatalog::resolve(std::string_view name) const {
  auto it = find_sv(datasets_, name);
  if (it == datasets_.end()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "DatasetCatalog::resolve: dataset '" +
                                                              std::string{name} +
                                                              "' not registered");
  }
  return atx::core::Ok(std::cref(it->second));
}

// ---------------------------------------------------------------------------
//  names
// ---------------------------------------------------------------------------

std::vector<std::string> DatasetCatalog::names() const {
  std::vector<std::string> out;
  out.reserve(datasets_.size());
  for (const auto &[k, _] : datasets_) {
    out.push_back(k);
  }
  return out; // std::map iterates in ascending key order — no extra sort needed
}

// ---------------------------------------------------------------------------
//  role_of
// ---------------------------------------------------------------------------

atx::core::Result<Role> DatasetCatalog::role_of(std::string_view name) const {
  auto it = find_sv(datasets_, name);
  if (it == datasets_.end()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "DatasetCatalog::role_of: dataset '" +
                                                              std::string{name} +
                                                              "' not registered");
  }
  return atx::core::Ok(it->second.role());
}

// ---------------------------------------------------------------------------
//  derive
// ---------------------------------------------------------------------------

atx::core::Status DatasetCatalog::derive(std::string child_name,
                                         std::vector<std::string> parent_names) {
  if (parent_names.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "DatasetCatalog::derive: parent_names must not be empty");
  }
  if (datasets_.count(child_name) == 0) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "DatasetCatalog::derive: child '" + child_name + "' not registered");
  }
  for (const auto &p : parent_names) {
    if (datasets_.count(p) == 0) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "DatasetCatalog::derive: parent '" + p + "' not registered");
    }
  }
  // Sort ascending + de-duplicate for determinism.
  std::sort(parent_names.begin(), parent_names.end());
  parent_names.erase(std::unique(parent_names.begin(), parent_names.end()), parent_names.end());

  lineage_[std::move(child_name)] = std::move(parent_names);
  return atx::core::Ok();
}

// ---------------------------------------------------------------------------
//  lineage
// ---------------------------------------------------------------------------

atx::core::Result<std::vector<std::string>> DatasetCatalog::lineage(std::string_view name) const {
  // Name must be registered (not-registered is a different error from
  // "registered but no derivation recorded").
  if (find_sv(datasets_, name) == datasets_.end()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "DatasetCatalog::lineage: dataset '" +
                                                              std::string{name} +
                                                              "' not registered");
  }
  auto it = find_sv(lineage_, name);
  if (it == lineage_.end()) {
    return atx::core::Ok(std::vector<std::string>{});
  }
  return atx::core::Ok(it->second); // copy of the sorted parent list
}

// ---------------------------------------------------------------------------
//  value_at
// ---------------------------------------------------------------------------

atx::core::Result<atx::f64> DatasetCatalog::value_at(std::string_view name, std::string_view col,
                                                     DateKey canonical_date, InstKey inst) const {
  // 1. Resolve dataset.
  auto ds_it = find_sv(datasets_, name);
  if (ds_it == datasets_.end()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "DatasetCatalog::value_at: dataset '" +
                                                              std::string{name} +
                                                              "' not registered");
  }
  const Dataset &ds = ds_it->second;

  // 2. Resolve the column by name (Dataset::column_by_name does the scan and
  //    returns the flat date-major span directly — DRY, no local index).
  auto col_res = ds.column_by_name(col);
  if (!col_res.has_value()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "DatasetCatalog::value_at: column '" + std::string{col} +
                              "' not found in dataset '" + std::string{name} + "'");
  }
  const std::span<const atx::f64> flat = *col_res;

  // 3. Resolve instrument index (O(n_instruments) linear scan).
  std::span<const InstKey> insts = ds.instruments();
  atx::usize inst_idx = insts.size(); // sentinel
  for (atx::usize i = 0; i < insts.size(); ++i) {
    if (insts[i] == inst) {
      inst_idx = i;
      break;
    }
  }
  if (inst_idx == insts.size()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "DatasetCatalog::value_at: instrument not found in dataset '" +
                              std::string{name} + "'");
  }

  // 4. PIT as-of: greatest row with DateKey ≤ canonical_date (shared helper;
  //    dates() is strictly ascending, enforced at registration).
  const std::optional<atx::usize> row = as_of_index(ds.dates(), canonical_date);
  if (!row) {
    // No row qualifies — return NaN, not an error.
    return atx::core::Ok(std::numeric_limits<atx::f64>::quiet_NaN());
  }

  return atx::core::Ok(flat[*row * ds.num_instruments() + inst_idx]);
}

} // namespace atx::engine::data

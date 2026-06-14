#pragma once

// atx::engine::data — catalog_report (S6.9): a DETERMINISTIC, headless
// catalog/lineage report writer.
//
// write_catalog_report serializes a DatasetCatalog into a byte-reproducible text
// artifact (<out_dir>/catalog_report.txt) that lists, in ASCENDING dataset-name
// order, each dataset's:
//   * name + role,
//   * schema — columns + dtypes, pit_delay, region, universe_tag, as-of policy,
//   * provenance source (the external-signal / ingest provenance linkage),
//   * lineage edges (the direct `derive` parents).
//
// REPRODUCIBILITY CONTRACT: the output contains NO timestamps, NO RNG, NO absolute
// paths, and NO hash-order iteration — only the catalog's deterministically-ordered
// content. Writing the same catalog twice produces byte-identical files. This makes
// the report a stable provenance/lineage record that can be diffed across runs.
//
// Cold path; the report is a one-shot headless artifact, not on any hot path.

#include <string>

#include "atx/core/error.hpp" // Status

#include "atx/engine/data/catalog.hpp" // DatasetCatalog

namespace atx::engine::data {

// Write a deterministic catalog/lineage report to <out_dir>/catalog_report.txt.
//
// Creates out_dir (and parents) if absent. The file lists every registered dataset
// in ascending name order with its role, schema, provenance source, and direct
// lineage parents. Byte-reproducible: same catalog => identical bytes.
//
// Errors:
//   * Err(IoError) if out_dir cannot be created or the file cannot be written.
//   * propagates the catalog's Err if a dataset / lineage probe fails.
[[nodiscard]] atx::core::Status write_catalog_report(const DatasetCatalog &catalog,
                                                     const std::string &out_dir);

} // namespace atx::engine::data

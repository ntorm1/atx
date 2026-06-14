// atx::engine::data — Dataset::create validation body + column_by_name.
//
// Construction is COLD-PATH; validation is explicit and returns Result<Dataset>
// on any contract violation. Hot-path accessors are all inline in dataset.hpp.

#include "atx/engine/data/dataset.hpp"

#include <string>

#include "atx/core/error.hpp"

namespace atx::engine::data {

// static
atx::core::Result<Dataset> Dataset::create(DatasetSchema schema, std::vector<DateKey> dates,
                                           std::vector<InstKey> instruments,
                                           std::vector<std::vector<atx::f64>> columns,
                                           std::vector<std::uint8_t> mask,
                                           DatasetProvenance provenance) {
  // 1. Schema dtype/role coherence.
  if (!schema_is_coherent(schema)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Dataset::create: schema is not coherent (columns empty or "
                          "columns.size() != dtypes.size())");
  }

  // 2. Caller-supplied columns count must match schema column count.
  if (columns.size() != schema.columns.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Dataset::create: columns.size() (" + std::to_string(columns.size()) +
                              ") != schema.columns.size() (" +
                              std::to_string(schema.columns.size()) + ")");
  }

  // 3. Every column must have exactly dates*instruments cells (no ragged).
  const atx::usize expected_cells = dates.size() * instruments.size();
  for (atx::usize c = 0; c < columns.size(); ++c) {
    if (columns[c].size() != expected_cells) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Dataset::create: column[" + std::to_string(c) + "] has " +
                                std::to_string(columns[c].size()) + " cells; expected " +
                                std::to_string(expected_cells) +
                                " (dates=" + std::to_string(dates.size()) +
                                " * instruments=" + std::to_string(instruments.size()) + ")");
    }
  }

  // 4. Mask, when non-empty, must match dates*instruments.
  if (!mask.empty() && mask.size() != expected_cells) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Dataset::create: mask.size() (" + std::to_string(mask.size()) +
                              ") != dates*instruments (" + std::to_string(expected_cells) + ")");
  }

  Dataset ds;
  ds.schema_ = std::move(schema);
  ds.dates_ = std::move(dates);
  ds.instruments_ = std::move(instruments);
  ds.columns_ = std::move(columns);
  ds.mask_ = std::move(mask);
  ds.provenance_ = std::move(provenance);
  return atx::core::Ok(std::move(ds));
}

atx::core::Result<std::span<const atx::f64>> Dataset::column_by_name(std::string_view name) const {
  const std::vector<std::string> &names = schema_.columns;
  for (atx::usize i = 0; i < names.size(); ++i) {
    if (names[i] == name) {
      return atx::core::Ok(std::span<const atx::f64>{columns_[i]});
    }
  }
  return atx::core::Err(atx::core::ErrorCode::NotFound,
                        std::string{"Dataset::column_by_name: unknown column '"} +
                            std::string{name} + "'");
}

} // namespace atx::engine::data

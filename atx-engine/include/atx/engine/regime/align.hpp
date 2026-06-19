#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/regime/series.hpp"

namespace atx::engine::regime {

struct NamedSeries {
  std::string name;
  std::vector<std::pair<atx::i64, atx::f64>> obs;  // sorted-ascending by date
};

// Sorted-unique union of every series' observation dates, keeping dates >= floor.
[[nodiscard]] std::vector<atx::i64> build_master_axis(std::span<const NamedSeries> series,
                                                      atx::i64 min_date_nanos);

// Reindex `obs` onto `axis` with forward-fill: axis[i] takes the value of the
// latest obs whose date <= axis[i]; NaN before the first obs.
[[nodiscard]] std::vector<atx::f64>
forward_fill(std::span<const std::pair<atx::i64, atx::f64>> obs, std::span<const atx::i64> axis);

// Append a derived column `spec.name = lhs OP rhs` computed elementwise from the
// existing columns named spec.lhs / spec.rhs. NaN propagates; '/' by 0 -> NaN.
// Err(NotFound) if an operand name is absent; Err(InvalidArgument) on bad op.
[[nodiscard]] atx::core::Status apply_derived(std::vector<std::string> &names,
                                              std::vector<std::vector<atx::f64>> &cols,
                                              const DerivedSpec &spec);

}  // namespace atx::engine::regime

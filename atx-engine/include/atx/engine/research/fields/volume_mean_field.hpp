#pragma once

// atx::engine::research::fields -- field vol_126, ported from research_fields_price.py
// volume_mean_rows (platform v8 F-1; migration slice 1). A price/volume field on the role's own
// rows: row t is the mean raw daily share volume over the role sessions t-126..t-1 with present ==
// 1 and a finite volume >= 0, NaN when fewer than 63 such sessions or t < 126 (TrailingMean(n, 126,
// 63)). Clock role-close-lag1-v1: row t is written before session t is read. The payload bytes,
// coverage and formula fingerprint equal the Python builder's (gtest ResearchFieldsFixture.*).

#include <filesystem>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {

inline constexpr usize kVolumeMeanWindow = 126;
inline constexpr usize kVolumeMeanMinSessions = 63;

// The declared spec (verbatim text of research_fields_price.py's vol_126 entry).
[[nodiscard]] const FieldSpec &vol_126_spec();

struct BuiltField {
  WrittenField field;
  std::vector<SourceRecord> sources; // the inputs, in the Python entry's order
};

// Writes `output_dir`/vol_126.f64 (output_dir exists). Reads the role's volume.f64 and present.u8
// streams, each verified against the role manifest after its last row.
[[nodiscard]] core::Result<BuiltField> build_vol_126(const RoleAxes &role,
                                                     const std::filesystem::path &output_dir);

} // namespace atx::engine::research::fields

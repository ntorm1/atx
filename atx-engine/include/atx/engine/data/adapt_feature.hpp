#pragma once

// atx::engine::data — adapt_feature (S6.4b): FeatureDataset → Panel merge.
//
// `merge_features_into_panel` aligns a feature Dataset onto the canonical price
// axis (via align_onto) and APPENDS its aligned columns to an existing Panel as
// new named fields. The result is a Panel that carries every original field
// UNCHANGED (same id, same values, same order) plus one new field per feature
// column — so a feature can be referenced by name in a learn::FeatureSpec's
// raw_fields and flows into a FeatureMatrix exactly like a native price field.
//
// PIT discipline is inherited wholesale from align_onto: a feature cell the plug
// does not cover (instrument absent, or no plug row on/before that date) is NaN,
// never imputed. The universe mask is carried over from the input Panel verbatim.
//
// The merge is OPT-IN: a price-only path that never calls this function sees the
// original Panel untouched (the boundary-pin for S6.8 / the price-only context).
//
// Cold path; copies are intentional.

#include <span>

#include "atx/core/error.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

// Align `feature` onto the price axis and append its columns to `panel_in` as new
// named fields (referenceable by name in FeatureSpec::raw_fields). Existing fields
// and their values are preserved in order, so a price-only path (no feature merge)
// is unaffected. Feature cells uncovered by the plug are NaN (never imputed).
//
// Errors:
//   * propagates align_onto's Err (e.g. non-ascending dates -> InvalidArgument).
//   * Err(InvalidArgument) if the aligned axis disagrees with panel_in's shape
//     (defensive — they agree when panel_in came from price_to_panel(price)).
//   * Err(InvalidArgument) if any feature column name collides with an existing
//     panel field name.
[[nodiscard]] atx::core::Result<alpha::Panel>
merge_features_into_panel(const alpha::Panel &panel_in, const Dataset &price,
                          const Dataset &feature);

} // namespace atx::engine::data

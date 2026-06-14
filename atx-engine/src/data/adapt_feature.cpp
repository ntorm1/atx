// atx::engine::data — adapt_feature (S6.4b): FeatureDataset → Panel merge.
//
// Implementation of merge_features_into_panel declared in data/adapt_feature.hpp.
// Cold path; every copy here is intentional (std::vector ownership transfer).

#include "atx/engine/data/adapt_feature.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/align.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

namespace {

// Materialize panel_in's universe into a date-major {0,1} mask of size
// dates*instruments (so Panel::create rebuilds the same membership). Reading
// every cell via in_universe means an empty/all-in-universe input still produces
// an explicit all-ones mask — harmless and shape-correct.
[[nodiscard]] std::vector<std::uint8_t> rebuild_universe(const alpha::Panel &panel_in) {
  const atx::usize nd = panel_in.dates();
  const atx::usize ni = panel_in.instruments();
  std::vector<std::uint8_t> universe(nd * ni, std::uint8_t{0});
  for (atx::usize d = 0; d < nd; ++d) {
    for (atx::usize i = 0; i < ni; ++i) {
      universe[d * ni + i] = panel_in.in_universe(d, i) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return universe;
}

} // namespace

[[nodiscard]] atx::core::Result<alpha::Panel>
merge_features_into_panel(const alpha::Panel &panel_in, const Dataset &price,
                          const Dataset &feature) {
  // 1. Align the feature plug onto the canonical price axis (propagate Err).
  ATX_TRY(AlignedView av, align_onto(price, feature));

  // Defensive: the aligned axis must match the input Panel's shape. They agree
  // when panel_in came from price_to_panel(price).
  if (av.num_dates != panel_in.dates() || av.num_instruments != panel_in.instruments()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "merge_features_into_panel: aligned axis disagrees with panel shape");
  }

  // 2. Combined field set: existing panel fields first (unchanged, in order),
  //    then the aligned feature columns. Reject a name collision.
  const atx::usize nf = panel_in.num_fields();
  std::vector<std::string> field_names;
  std::vector<std::vector<atx::f64>> field_data;
  field_names.reserve(nf + av.columns.size());
  field_data.reserve(nf + av.columns.size());
  for (atx::usize f = 0; f < nf; ++f) {
    const std::span<const atx::f64> col = panel_in.field_all(static_cast<alpha::FieldId>(f));
    field_names.emplace_back(panel_in.field_name(static_cast<alpha::FieldId>(f)));
    field_data.emplace_back(col.begin(), col.end());
  }
  for (atx::usize c = 0; c < av.columns.size(); ++c) {
    for (const std::string &existing : field_names) {
      if (existing == av.columns[c]) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              std::string{"merge_features_into_panel: feature column '"} +
                                  av.columns[c] + "' collides with an existing panel field");
      }
    }
    field_names.push_back(std::move(av.columns[c]));
    field_data.push_back(std::move(av.aligned_columns[c]));
  }

  // 3. Carry the universe mask over from the input Panel.
  std::vector<std::uint8_t> universe = rebuild_universe(panel_in);

  // 4. Rebuild the Panel with the combined field set.
  return alpha::Panel::create(panel_in.dates(), panel_in.instruments(), std::move(field_names),
                              std::move(field_data), std::move(universe));
}

} // namespace atx::engine::data

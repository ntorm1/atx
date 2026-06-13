#include "atx/engine/learn/feature_matrix.hpp"

#include <cmath>  // std::isfinite
#include <span>   // std::span
#include <utility> // std::move
#include <vector> // std::vector

#include "atx/core/error.hpp" // Result, Ok, ATX_TRY
#include "atx/core/macro.hpp" // ATX_CHECK
#include "atx/core/types.hpp" // f64, u8, usize

#include "atx/engine/alpha/panel.hpp"   // alpha::Panel, FieldId
#include "atx/engine/combine/store.hpp" // combine::AlphaStore, combine::AlphaId

namespace atx::engine::learn {

namespace detail {

// Write the feature row for cell (date, inst) into X[row*n_features ..]: the raw
// fields (in id order) then the pool alphas (in id order). Returns true iff every
// written feature is finite (the row_valid flag). SAFETY: `raw_cs[f]` aliases the
// Panel's date-major column for date `date`; it is valid for the life of `panel`
// and is read at `inst` < instruments (the caller bounds the loop). Pool-alpha
// values come from store.positions(id, date)[inst]; `date` < n_periods and
// `inst` < n_instruments are bounded by the caller, so the deref is in range.
bool
write_feature_row(std::span<atx::f64> X, atx::usize row, atx::usize n_features,
                  const std::vector<std::span<const atx::f64>> &raw_cs,
                  const combine::AlphaStore &store, const FeatureSpec &spec, atx::usize date,
                  atx::usize inst) {
  const atx::usize base = row * n_features;
  ATX_CHECK(base + n_features <= X.size());
  atx::usize f = 0;
  bool all_finite = true;
  for (const std::span<const atx::f64> &cs : raw_cs) {
    const atx::f64 v = cs[inst];
    X[base + f] = v;
    all_finite = all_finite && std::isfinite(v);
    ++f;
  }
  for (const combine::AlphaId id : spec.pool_alphas) {
    // The stored stream is already PIT-aligned: its period axis IS the date axis,
    // so positions(id, date) is the cross-section knowable at date `date`.
    const std::span<const atx::f64> cs = store.positions(id, date);
    const atx::f64 v = cs[inst];
    X[base + f] = v;
    all_finite = all_finite && std::isfinite(v);
    ++f;
  }
  return all_finite;
}

} // namespace detail

atx::core::Result<FeatureMatrix>
build_features(const alpha::Panel &panel, const combine::AlphaStore &store,
               const FeatureSpec &spec) {
  ATX_TRY(const std::vector<alpha::FieldId> raw_ids, detail::resolve_raw_fields(panel, spec));
  ATX_TRY(const alpha::FieldId close_id, panel.field_id("close"));

  FeatureMatrix fm;
  fm.n_dates = panel.dates();
  fm.n_instruments = panel.instruments();
  fm.n_features = spec.raw_fields.size() + spec.pool_alphas.size();
  fm.Y.assign(spec.horizons.size(), {});

  const std::span<const atx::f64> close_all = panel.field_all(close_id);

  // Emit in (date, instrument) order — a single nested loop, no map iteration.
  for (atx::usize d = 0; d < fm.n_dates; ++d) {
    // The raw cross-sections for date d alias the Panel's columns (PIT: date d).
    std::vector<std::span<const atx::f64>> raw_cs;
    raw_cs.reserve(raw_ids.size());
    for (const alpha::FieldId fid : raw_ids) {
      raw_cs.push_back(panel.field_cross_section(fid, d));
    }
    for (atx::usize i = 0; i < fm.n_instruments; ++i) {
      if (!panel.in_universe(d, i)) {
        continue; // out-of-universe: NO row (M8 — not zero-filled)
      }
      const atx::usize row = fm.push_row(d, i);
      fm.X.resize((row + 1) * fm.n_features);
      const bool valid = detail::write_feature_row(std::span<atx::f64>{fm.X}, row, fm.n_features,
                                                   raw_cs, store, spec, d, i);
      fm.row_valid.push_back(static_cast<atx::u8>(valid ? 1 : 0));
      for (atx::usize h = 0; h < spec.horizons.size(); ++h) {
        fm.Y[h].push_back(detail::forward_return(close_all, fm.n_dates, fm.n_instruments, d, i,
                                                 spec.horizons[h]));
      }
    }
  }
  return atx::core::Ok(std::move(fm));
}

} // namespace atx::engine::learn

#pragma once

// atx::engine::learn — FeatureMatrix + FeatureSpec + build_features (S5-1).
//
// =====================================================================
//  What this header is
// =====================================================================
//  The deterministic, point-in-time (date x instrument) x feature TENSOR plus
//  multi-horizon forward-return LABELS that every learned model in Sprint 5
//  trains on. It is the data plane for the learn layer exactly as alpha::Panel
//  is for the alpha VM: an immutable value type built once (cold path) from the
//  as-built engine seams (raw alpha::Panel fields + already-aligned stored pool
//  streams in combine::AlphaStore), never re-derived on a hot path.
//
// =====================================================================
//  The four load-bearing invariants this unit must honor (M1, M2, M8, §0.6)
// =====================================================================
//  PIT build (M2). A feature at (date d, instrument i) reads ONLY rows whose
//  date <= d. Raw fields come straight from the Panel cross-section at d; pool
//  alpha features come from the already-PIT-aligned stored stream at d. Adding
//  later dates to the source can NEVER change a feature value at an earlier
//  date — the truncation-invariance test pins this.
//
//  Deterministic emit order (M1). Rows are emitted in (date, instrument) order —
//  a single nested loop, NO map iteration on the emit path. The (date,inst)->row
//  lookup map is built ALONGSIDE the dense vectors but never drives iteration, so
//  the row layout is run-to-run byte-identical.
//
//  Universe / NaN policy (M8). An out-of-universe (d,i) emits NO row (it is
//  skipped, not zero-filled). row_valid[row] == 1 iff every feature at that row
//  is finite; an invalid row is DROPPED from a downstream fit (not coerced to 0).
//  NaN is carried verbatim, never silently replaced.
//
//  Forward labels (§0.6 / M8). Labels read FORWARD — they are the prediction
//  TARGET, never a feature. Y[h][row] = close[d+H[h]]/close[d] - 1 when
//  d + H[h] < n_dates, else NaN (the tail is unknowable, NOT zero).
//
// =====================================================================
//  Layout
// =====================================================================
//  X is row-major-by-row: feature f of row r lives at X[r * n_features + f].
//  Y[h] is one f64 per row (the horizon-H[h] forward return). row_date / row_inst
//  map a row back to its (date, instrument). The feature column order is fixed:
//  the raw_fields (in spec order) first, then the pool_alphas (in spec order).
//
// Header-only; every member / free function is defined inline. Construction is a
// COLD path (once per training window), so std::vector allocation is fine (M7).

#include <cmath>         // std::isfinite
#include <limits>        // std::numeric_limits<f64>::quiet_NaN
#include <span>          // std::span (aliasing Panel / store cross-sections)
#include <string>        // std::string (raw field names)
#include <unordered_map> // (date,inst)->row lookup (built off the hot path)
#include <utility>       // std::move
#include <vector>        // std::vector (owned dense storage)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/macro.hpp" // ATX_CHECK
#include "atx/core/types.hpp" // f64, u8, u16, usize

#include "atx/engine/alpha/panel.hpp"   // alpha::Panel, FieldId
#include "atx/engine/combine/store.hpp" // combine::AlphaStore, combine::AlphaId

namespace atx::engine::learn {

// ===========================================================================
//  FeatureSpec — the declarative recipe for one FeatureMatrix.
//
//  raw_fields : Panel field names, in fixed column order (e.g. {"close",
//               "volume"}). Each must resolve via Panel::field_id.
//  pool_alphas: stored pool alphas used as features — their per-date position
//               cross-sections from the AlphaStore, in fixed column order after
//               the raw fields. May be empty (the S5-1 default tests pass {}).
//  horizons   : forward-return label horizons in days; default {1,5,21}. {1}
//               recovers the single-horizon case.
//  max_lookback: the largest history (in dates) any feature reads — the PIT
//               bound. Raw-field-only features that read just date d use 0.
// ===========================================================================
struct FeatureSpec {
  std::vector<std::string> raw_fields;
  std::vector<combine::AlphaId> pool_alphas;
  std::vector<atx::u16> horizons{1, 5, 21};
  atx::u16 max_lookback{0};
};

// ===========================================================================
//  FeatureMatrix — the immutable, deterministic, PIT (date x instrument) x
//  feature tensor + multi-horizon forward-return labels.
//
//  Emitted in (date, instrument) order: row 0 is the first in-universe cell of
//  date 0, and so on. Out-of-universe cells produce NO row (has_row false).
// ===========================================================================
struct FeatureMatrix {
  atx::usize n_dates{0};
  atx::usize n_instruments{0};
  atx::usize n_features{0};

  std::vector<atx::usize> row_date; // row -> date index   (one per emitted row)
  std::vector<atx::usize> row_inst; // row -> instrument index
  std::vector<atx::f64> X;          // [n_rows * n_features], X[r*n_features + f]
  std::vector<std::vector<atx::f64>> Y; // Y[h] = [n_rows] fwd-return at H[h]; NaN where unknowable
  std::vector<atx::u8> row_valid;       // 1 iff all features finite at the row

  // Number of emitted rows (in-universe cells).
  [[nodiscard]] atx::usize n_rows() const noexcept { return row_date.size(); }

  // True iff (date, inst) was emitted as a row (i.e. was in-universe).
  [[nodiscard]] bool has_row(atx::usize date, atx::usize inst) const {
    return row_lookup_.find(key_of(date, inst)) != row_lookup_.end();
  }

  // Row index of (date, inst). PRECONDITION: has_row(date, inst). The deref of
  // the iterator sits OUTSIDE the checked condition and must hold under NDEBUG,
  // so this is ATX_CHECK (not ATX_ASSERT): an absent cell aborts in every build
  // rather than dereferencing end().
  [[nodiscard]] atx::usize row_of(atx::usize date, atx::usize inst) const {
    const auto it = row_lookup_.find(key_of(date, inst));
    ATX_CHECK(it != row_lookup_.end());
    return it->second;
  }

  // --- construction plumbing (used only by build_features) ------------------

  // Append a row for (date, inst): record its provenance and remember its index
  // in the (date,inst)->row lookup. Returns the new row index. The lookup is
  // built here, OFF the feature-emit hot loop, so emit order stays deterministic.
  atx::usize push_row(atx::usize date, atx::usize inst) {
    const atx::usize row = row_date.size();
    row_date.push_back(date);
    row_inst.push_back(inst);
    row_lookup_.emplace(key_of(date, inst), row);
    return row;
  }

private:
  // Pack (date, inst) into one u64 key. SAFETY: n_instruments fits well inside
  // 32 bits for any realistic panel; the shift cannot collide two distinct cells
  // because inst < n_instruments <= 2^32 and date occupies the high half.
  [[nodiscard]] static atx::u64 key_of(atx::usize date, atx::usize inst) noexcept {
    return (static_cast<atx::u64>(date) << 32U) | static_cast<atx::u64>(inst);
  }

  std::unordered_map<atx::u64, atx::usize> row_lookup_; // (date,inst)->row; built off the emit loop
};

namespace detail {

// Resolve every raw field name in `spec` to a Panel FieldId, in spec order.
// Propagates Panel::field_id's Err(NotFound) for an unknown field.
[[nodiscard]] inline atx::core::Result<std::vector<alpha::FieldId>>
resolve_raw_fields(const alpha::Panel &panel, const FeatureSpec &spec) {
  std::vector<alpha::FieldId> ids;
  ids.reserve(spec.raw_fields.size());
  for (const std::string &name : spec.raw_fields) {
    ATX_TRY(const alpha::FieldId fid, panel.field_id(name));
    ids.push_back(fid);
  }
  return atx::core::Ok(std::move(ids));
}

// Write the feature row for cell (date, inst) into X[row*n_features ..]: the raw
// fields (in id order) then the pool alphas (in id order). Returns true iff every
// written feature is finite (the row_valid flag). SAFETY: `raw_cs[f]` aliases the
// Panel's date-major column for date `date`; it is valid for the life of `panel`
// and is read at `inst` < instruments (the caller bounds the loop). Pool-alpha
// values come from store.positions(id, date)[inst]; `date` < n_periods and
// `inst` < n_instruments are bounded by the caller, so the deref is in range.
[[nodiscard]] inline bool
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

// Forward-return label for cell (date, inst) at horizon H: close[d+H]/close[d]-1
// when d+H < n_dates, else quiet NaN (the tail is unknowable — §0.6 / M8).
// `close_all` is the whole close field, date-major (length dates*instruments).
[[nodiscard]] inline atx::f64 forward_return(std::span<const atx::f64> close_all,
                                             atx::usize n_dates, atx::usize n_instruments,
                                             atx::usize date, atx::usize inst, atx::u16 horizon) {
  const atx::usize ahead = date + static_cast<atx::usize>(horizon);
  if (ahead >= n_dates) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  const atx::f64 now = close_all[date * n_instruments + inst];
  const atx::f64 fut = close_all[ahead * n_instruments + inst];
  return fut / now - 1.0;
}

} // namespace detail

// ===========================================================================
//  build_features — assemble the PIT FeatureMatrix from the as-built seams.
//
//  PURE, deterministic. Rows are emitted in (date, instrument) order; an
//  out-of-universe cell emits no row (M8). Features read only date <= d (M2 —
//  raw fields from the Panel cross-section at d, pool alphas from the PIT-aligned
//  store stream at d). Labels read FORWARD with NaN at the tail (§0.6). The close
//  field (required for labels) must exist in the Panel — its absence is
//  Err(NotFound); an unknown raw field is likewise Err(NotFound).
// ===========================================================================
[[nodiscard]] inline atx::core::Result<FeatureMatrix>
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

#include "atx/engine/learn/sequence_features.hpp"

#include <cmath>   // std::isfinite
#include <limits>  // std::numeric_limits
#include <utility> // std::move
#include <vector>  // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, u8, usize

#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix

namespace atx::engine::learn {

namespace {

// True iff instrument `i` has an emitted FeatureMatrix row for EVERY date in the
// trailing window [t-L+1, t]. Window completeness (M8): requires no date
// underflow (t+1 >= L) AND has_row at all L cells — a universe dropout mid-window
// is a gap and makes the window incomplete. TRAILING by construction (R2): never
// inspects a date > t.
[[nodiscard]] bool window_complete(const FeatureMatrix &fm, atx::usize t, atx::usize i,
                                   atx::usize L) {
  if (t + 1 < L) {
    return false; // insufficient history (would underflow t-L+1)
  }
  const atx::usize first = t + 1 - L;
  for (atx::usize d = first; d <= t; ++d) {
    if (!fm.has_row(d, i)) {
      return false; // gap: instrument missing on date d
    }
  }
  return true;
}

// Pack the COMPLETE trailing window for anchor (t, i) into st.x at sample `s`:
// x[(s*L + l)*F + f] = fm.X[ row_of(t-L+1+l, i) * F + f ] for l in [0,L), f in
// [0,F), ascending. Returns true iff every packed value is finite (the
// sample_valid flag — mirrors row_valid; NaN is NOT coerced). PRECONDITION:
// window_complete(fm, t, i, L) — every cell has a row.
[[nodiscard]] bool pack_window(SequenceTensor &st, const FeatureMatrix &fm, atx::usize s,
                               atx::usize t, atx::usize i, atx::usize L, atx::usize F) {
  const atx::usize first = t + 1 - L;
  const atx::usize base = (s * L) * F; // == (s*L + 0)*F
  bool all_finite = true;
  for (atx::usize l = 0; l < L; ++l) {
    const atx::usize fm_row = fm.row_of(first + l, i);
    const atx::usize src = fm_row * F;
    const atx::usize dst = base + l * F;
    for (atx::usize f = 0; f < F; ++f) {
      const atx::f64 v = fm.X[src + f];
      st.x[dst + f] = v; // carry verbatim — never replace NaN (no survivorship)
      all_finite = all_finite && std::isfinite(v);
    }
  }
  return all_finite;
}

// Fill the window for sample `s` with quiet NaN ("no opinion") — the
// drop_incomplete=false path for an incomplete anchor. No row is read (there is
// no complete history to read), so nothing is borrowed from another instrument.
void fill_nan_window(SequenceTensor &st, atx::usize s, atx::usize L, atx::usize F) {
  const atx::usize base = (s * L) * F;
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  for (atx::usize k = 0; k < L * F; ++k) {
    st.x[base + k] = nan;
  }
}

// Append one sample's bookkeeping (grow x, carry labels + provenance + validity).
// The window bytes for slot `s` are written by the caller (pack_window /
// fill_nan_window) AFTER x has been grown here.
void begin_sample(SequenceTensor &st, const FeatureMatrix &fm, atx::usize t, atx::usize i,
                  atx::usize L, atx::usize F) {
  const atx::usize s = st.n_samples;
  st.x.resize((s + 1) * L * F);
  st.date_of.push_back(t);
  st.inst_of.push_back(i);
  const atx::usize anchor = fm.row_of(t, i); // anchor is an emitted row (caller ensured)
  for (atx::usize h = 0; h < fm.Y.size(); ++h) {
    st.y[h].push_back(fm.Y[h][anchor]);
  }
  ++st.n_samples;
}

} // namespace

atx::core::Result<SequenceTensor>
build_sequences(const FeatureMatrix &fm, const SeqFeatureSpec &spec) {
  const atx::usize L = spec.lookback;
  const atx::usize F = fm.n_features;
  if (L == 0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "lookback must be >= 1");
  }
  if (F == 0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "fm.n_features must be > 0");
  }

  SequenceTensor st;
  st.lookback = L;
  st.n_features = F;
  st.y.assign(fm.Y.size(), {});

  // Iterate anchor cells in ascending (date t, instrument i). Only emitted rows
  // (has_row) are candidate anchors — they carry the label + provenance.
  for (atx::usize t = 0; t < fm.n_dates; ++t) {
    for (atx::usize i = 0; i < fm.n_instruments; ++i) {
      if (!fm.has_row(t, i)) {
        continue; // out-of-universe anchor: NO sample
      }
      const bool complete = window_complete(fm, t, i, L);
      if (!complete && spec.drop_incomplete) {
        continue; // incomplete history/gap, drop mode: NO sample
      }
      const atx::usize s = st.n_samples;
      begin_sample(st, fm, t, i, L, F);
      bool valid = false;
      if (complete) {
        valid = pack_window(st, fm, s, t, i, L, F);
      } else {
        fill_nan_window(st, s, L, F); // "no opinion" window, never zero-padded
      }
      st.sample_valid.push_back(static_cast<atx::u8>(valid ? 1 : 0));
    }
  }
  return atx::core::Ok(std::move(st));
}

} // namespace atx::engine::learn

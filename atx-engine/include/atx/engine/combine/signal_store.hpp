#pragma once

// atx::engine::combine — SignalStore: the signal-space substrate of the zoo combiner
// (Lane 5).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  AlphaStore (store.hpp) holds each alpha's REALIZED PnL stream; combining in that
//  space ("PnL-proxy" weights) throws away the cross-section. SignalStore instead
//  holds, per alpha, the full date × instrument FORECAST panel (cross-sectionally
//  z-scored on insert) plus ONE shared date × instrument panel of forward residual
//  returns. Every signal-space combiner (signal_combiner.hpp), the orthogonalizers
//  (orthogonalize.hpp), walk_forward_combiner.hpp and decay_fit.hpp read from it.
//
//  Layout (contiguous, so a future mmap-backed arena is a drop-in):
//    signals_  alpha-major, then date-major: element (a, t, i) at
//              a·T·N + t·N + i
//    fwd_      date-major: element (t, i) at t·N + i
//  fwd(t, i) is the forward return realized AFTER date t (from t to t+h). A combiner
//  that fits on the window [begin, end) therefore consumes returns realized up to
//  end−1+h; walk_forward_combiner.hpp owns the embargo that keeps that PIT.
//
//  Missing data is NaN and is never coerced: every statistic below uses the cells
//  where BOTH the signal and the forward return are finite.
//
//  Determinism: no RNG; all reductions run in ascending (date, instrument) order.

#include <algorithm> // std::clamp
#include <cmath>     // std::isfinite, std::sqrt
#include <limits>    // std::numeric_limits
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, Status
#include "atx/core/types.hpp" // f64, u8, u32, usize

namespace atx::engine::combine {

// Half-open window of DATE rows [begin, end).
struct FitWindow {
  atx::usize begin = 0U;
  atx::usize end = 0U;
  [[nodiscard]] constexpr atx::usize size() const noexcept { return end > begin ? end - begin : 0U; }
};

// Insert-time normalization of a raw forecast panel.
enum class SignalNormalize : atx::u8 {
  None,   // store verbatim (caller already produced a z-score)
  ZScore, // per-date cross-sectional z-score, winsorized at ±winsor σ, re-standardized
};

inline constexpr atx::f64 kSignalNaN = std::numeric_limits<atx::f64>::quiet_NaN();

namespace signal_detail {

// Per-row cross-sectional z-score in place: z = (x − mean)/sd over finite cells, clip
// to ±winsor, then re-center / re-scale the clipped values (so the stored row has mean
// 0 and unit population sd over its finite cells). A row with < 2 finite cells or
// zero dispersion carries no cross-sectional information → its finite cells become 0.
inline void zscore_row(std::span<atx::f64> row, atx::f64 winsor) noexcept {
  const auto moments = [&row]() noexcept {
    atx::f64 sum = 0.0;
    atx::usize n = 0U;
    for (const atx::f64 x : row) {
      if (std::isfinite(x)) {
        sum += x;
        ++n;
      }
    }
    const atx::f64 mean = (n == 0U) ? 0.0 : sum / static_cast<atx::f64>(n);
    atx::f64 ss = 0.0;
    for (const atx::f64 x : row) {
      if (std::isfinite(x)) {
        ss += (x - mean) * (x - mean);
      }
    }
    const atx::f64 sd = (n < 2U) ? 0.0 : std::sqrt(ss / static_cast<atx::f64>(n));
    return std::pair<atx::f64, atx::f64>{mean, sd};
  };
  for (int pass = 0; pass < 2; ++pass) { // pass 0: z + clip; pass 1: re-standardize
    const auto [mean, sd] = moments();
    for (atx::f64 &x : row) {
      if (!std::isfinite(x)) {
        x = kSignalNaN;
        continue;
      }
      if (sd <= 0.0) {
        x = 0.0;
        continue;
      }
      const atx::f64 z = (x - mean) / sd;
      x = (pass == 0) ? std::clamp(z, -winsor, winsor) : z;
    }
  }
}

} // namespace signal_detail

// ===========================================================================
//  SignalStore
// ===========================================================================
class SignalStore {
public:
  // Empty store of fixed shape; forward returns start all-NaN. Err on a zero dim.
  [[nodiscard]] static atx::core::Result<SignalStore> create(atx::usize n_dates,
                                                             atx::usize n_instruments) {
    if (n_dates == 0U || n_instruments == 0U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "SignalStore::create: n_dates and n_instruments must be > 0");
    }
    SignalStore s;
    s.n_dates_ = n_dates;
    s.n_inst_ = n_instruments;
    s.fwd_.assign(n_dates * n_instruments, kSignalNaN);
    return atx::core::Ok(std::move(s));
  }

  // Copy a date-major T×N forecast panel in as alpha id n_alphas() (returned). With
  // SignalNormalize::ZScore each date row is z-scored (see zscore_row). Err on a size
  // mismatch or winsor <= 0.
  [[nodiscard]] atx::core::Result<atx::u32> add_signal(std::span<const atx::f64> panel,
                                                       SignalNormalize mode = SignalNormalize::ZScore,
                                                       atx::f64 winsor = 3.0) {
    if (panel.size() != cells()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "SignalStore::add_signal: panel size != n_dates*n_instruments");
    }
    if (!(winsor > 0.0)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "SignalStore::add_signal: winsor must be > 0");
    }
    if (n_alphas_ >= static_cast<atx::usize>(std::numeric_limits<atx::u32>::max())) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange, "SignalStore::add_signal: full");
    }
    const atx::usize base = signals_.size();
    signals_.insert(signals_.end(), panel.begin(), panel.end());
    if (mode == SignalNormalize::ZScore) {
      for (atx::usize t = 0U; t < n_dates_; ++t) {
        signal_detail::zscore_row(std::span<atx::f64>(signals_.data() + base + t * n_inst_, n_inst_),
                                  winsor);
      }
    }
    // SAFETY: bounded by the u32-max guard above.
    const auto id = static_cast<atx::u32>(n_alphas_);
    ++n_alphas_;
    return atx::core::Ok(id);
  }

  // Replace the shared forward-return panel (date-major T×N, NaN = missing).
  [[nodiscard]] atx::core::Status set_forward_returns(std::span<const atx::f64> fwd) {
    if (fwd.size() != cells()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "SignalStore::set_forward_returns: size != n_dates*n_instruments");
    }
    fwd_.assign(fwd.begin(), fwd.end());
    return atx::core::Ok();
  }

  [[nodiscard]] atx::usize n_alphas() const noexcept { return n_alphas_; }
  [[nodiscard]] atx::usize n_dates() const noexcept { return n_dates_; }
  [[nodiscard]] atx::usize n_instruments() const noexcept { return n_inst_; }
  [[nodiscard]] atx::usize cells() const noexcept { return n_dates_ * n_inst_; }

  // Alpha a's whole T×N panel. Precondition: a < n_alphas().
  [[nodiscard]] std::span<const atx::f64> signal(atx::usize a) const noexcept {
    return {signals_.data() + a * cells(), cells()};
  }
  // Alpha a's cross-section at date t. Preconditions: a < n_alphas(), t < n_dates().
  [[nodiscard]] std::span<const atx::f64> signal_row(atx::usize a, atx::usize t) const noexcept {
    return {signals_.data() + a * cells() + t * n_inst_, n_inst_};
  }
  [[nodiscard]] std::span<const atx::f64> forward_returns() const noexcept { return fwd_; }
  [[nodiscard]] std::span<const atx::f64> fwd_row(atx::usize t) const noexcept {
    return {fwd_.data() + t * n_inst_, n_inst_};
  }

private:
  SignalStore() = default;

  atx::usize n_dates_ = 0U;
  atx::usize n_inst_ = 0U;
  atx::usize n_alphas_ = 0U;
  std::vector<atx::f64> signals_;
  std::vector<atx::f64> fwd_;
};

// ===========================================================================
//  Per-date cross-sectional statistics (the inputs every combiner consumes).
// ===========================================================================

// Pearson correlation of x and y over their jointly-finite cells; NaN when fewer than
// 3 joint cells or either side has zero dispersion.
[[nodiscard]] inline atx::f64 cross_section_corr(std::span<const atx::f64> x,
                                                 std::span<const atx::f64> y) noexcept {
  const atx::usize n = std::min(x.size(), y.size());
  atx::f64 sx = 0.0;
  atx::f64 sy = 0.0;
  atx::usize k = 0U;
  for (atx::usize i = 0U; i < n; ++i) {
    if (std::isfinite(x[i]) && std::isfinite(y[i])) {
      sx += x[i];
      sy += y[i];
      ++k;
    }
  }
  if (k < 3U) {
    return kSignalNaN;
  }
  const atx::f64 mx = sx / static_cast<atx::f64>(k);
  const atx::f64 my = sy / static_cast<atx::f64>(k);
  atx::f64 sxx = 0.0;
  atx::f64 syy = 0.0;
  atx::f64 sxy = 0.0;
  for (atx::usize i = 0U; i < n; ++i) {
    if (std::isfinite(x[i]) && std::isfinite(y[i])) {
      const atx::f64 dx = x[i] - mx;
      const atx::f64 dy = y[i] - my;
      sxx += dx * dx;
      syy += dy * dy;
      sxy += dx * dy;
    }
  }
  if (sxx <= 0.0 || syy <= 0.0) {
    return kSignalNaN;
  }
  return sxy / std::sqrt(sxx * syy);
}

// Factor-mimicking ("alpha") return of forecast x against returns y at one date: the
// no-intercept slope Σ x·y / Σ x² over jointly-finite cells. NaN when Σx² == 0.
[[nodiscard]] inline atx::f64 cross_section_slope(std::span<const atx::f64> x,
                                                  std::span<const atx::f64> y) noexcept {
  const atx::usize n = std::min(x.size(), y.size());
  atx::f64 sxx = 0.0;
  atx::f64 sxy = 0.0;
  for (atx::usize i = 0U; i < n; ++i) {
    if (std::isfinite(x[i]) && std::isfinite(y[i])) {
      sxx += x[i] * x[i];
      sxy += x[i] * y[i];
    }
  }
  return (sxx > 0.0) ? sxy / sxx : kSignalNaN;
}

// Window IC matrix, row-major (window.size() × n_alphas): ic[r·K + a] = IC of alpha a
// at date window.begin + r. NaN where undefined. Precondition: window within store.
[[nodiscard]] inline std::vector<atx::f64> ic_matrix(const SignalStore &s, FitWindow w) {
  const atx::usize k = s.n_alphas();
  std::vector<atx::f64> out(w.size() * k, kSignalNaN);
  for (atx::usize r = 0U; r < w.size(); ++r) {
    const auto fwd = s.fwd_row(w.begin + r);
    for (atx::usize a = 0U; a < k; ++a) {
      out[r * k + a] = cross_section_corr(s.signal_row(a, w.begin + r), fwd);
    }
  }
  return out;
}

// Window alpha-return matrix, row-major (window.size() × n_alphas), via
// cross_section_slope. NaN where undefined.
[[nodiscard]] inline std::vector<atx::f64> alpha_return_matrix(const SignalStore &s, FitWindow w) {
  const atx::usize k = s.n_alphas();
  std::vector<atx::f64> out(w.size() * k, kSignalNaN);
  for (atx::usize r = 0U; r < w.size(); ++r) {
    const auto fwd = s.fwd_row(w.begin + r);
    for (atx::usize a = 0U; a < k; ++a) {
      out[r * k + a] = cross_section_slope(s.signal_row(a, w.begin + r), fwd);
    }
  }
  return out;
}

// Combined forecast at date t: out[i] = Σ_a w[a]·z_a(t, i), NaN signal cells treated
// as 0 (no view). Preconditions: w.size() == n_alphas(), out.size() == n_instruments().
inline void combine_forecast(const SignalStore &s, std::span<const atx::f64> w, atx::usize t,
                             std::span<atx::f64> out) noexcept {
  for (atx::f64 &x : out) {
    x = 0.0;
  }
  for (atx::usize a = 0U; a < s.n_alphas() && a < w.size(); ++a) {
    if (w[a] == 0.0) {
      continue;
    }
    const auto row = s.signal_row(a, t);
    for (atx::usize i = 0U; i < out.size(); ++i) {
      if (std::isfinite(row[i])) {
        out[i] += w[a] * row[i];
      }
    }
  }
}

} // namespace atx::engine::combine

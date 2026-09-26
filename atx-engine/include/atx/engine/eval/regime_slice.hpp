#pragma once

// atx::engine::eval — regime / walk-forward survival slicing + RobustnessVerdict
// (S4.4a). The SECOND half of the robustness measurement layer (the first being
// synthetic_alpha.hpp's planted-signal recovery).
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  A full-sample out-of-sample Sharpe hides regime-conditional fragility: an
//  alpha can post a healthy aggregate number while quietly bleeding in every
//  high-volatility stretch (the regime that matters most for survival). This
//  header slices a realized OOS PnL stream two independent ways and asks the
//  same question of each slice:
//
//    * regime_labels  — a deterministic VOLATILITY-TERCILE partition of the
//      dates {low, mid, high} from the panel's trailing realized vol.
//    * per_regime_sharpe — the per-period Sharpe of the alpha's PnL WITHIN each
//      regime's dates.
//    * walk_forward_sharpe — the per-period Sharpe over n contiguous, disjoint
//      rolling windows (a time partition orthogonal to the vol partition).
//    * robustness_verdict — folds both into a RobustnessVerdict: an alpha is
//      ROBUST iff its per-regime OOS Sharpe stays >= min_regime_sharpe in EVERY
//      regime AND across EVERY walk-forward window (not merely full-sample).
//
//  RobustnessVerdict is a plain, copyable value type — it is the artifact the
//  S4.5 pipeline gate consumes (and the S4.4b driver hook reads). It carries an
//  optional recovery_corr slot (synthetic_alpha.hpp's planted-signal recovery)
//  so a downstream gate can fold both robustness signals from one struct.
//
// ===========================================================================
//  FIT / APPLY FIREWALL (load-bearing) — every slice is OOS
// ===========================================================================
//  The input `alpha_pnl` is ALREADY the firewalled OOS realized-return stream
//  (extract_streams output over a causal VM eval; no look-ahead, w[t-1] earns
//  ret[t]). This header NEVER refits anything: a regime label and a walk-forward
//  window are a PURE PARTITION of that one OOS stream. No walk-forward window
//  trains on its own test slice — there is no training here at all, only a
//  disjoint slice of an out-of-sample series. The vol-tercile labels are derived
//  from the panel's price history (a property of the market, not of the alpha).
//  The default ExpandingPastV2 also fits cuts on STRICTLY PRIOR volatility dates;
//  neither current returns nor future cut calibration affects a label. Explicit
//  LegacyFullSampleV1 retains the old retrospective full-sample rank partition.
//  (Where this header is used
//  to slice a CPCV TEST-fold PnL, the CPCV embargo — cpcv.hpp, embargo=0.01 —
//  already separates each test block from the train labels; the slicing here
//  inherits that firewall and adds no leakage of its own.)
//
// ===========================================================================
//  Determinism (load-bearing)
// ===========================================================================
//  No RNG, no clock. ExpandingPastV2 keeps prior finite vols ordered by (vol,date)
//  and uses empirical tercile cuts, with equality assigned to the middle regime.
//  A flat history remains middle; there is no forced ex-post balance. The legacy
//  rule preserves its full-sample date-rank tie break. Walk-forward windows are
//  fixed contiguous index ranges. Every
//  Sharpe reduction (eval::mean_std_pop) walks its slice in ascending index.
//
// ===========================================================================
//  Recorded richer alternative (shipped: the tercile partition)
// ===========================================================================
//  A regime model could instead label each date by argmax over an HMM's
//  posterior state probability (a `regime_posterior_at(t)`-argmax fork — a
//  Hamilton/Baum-Welch latent-vol model). That is the richer alternative; it
//  needs a fitted transition/emission model (extra state, extra determinism
//  surface, a fit/apply firewall of its OWN). S4.4a ships the cheaper, fully-
//  deterministic, fit-free VOLATILITY-TERCILE partition — it needs only the
//  panel's own price history and introduces no fitted state. The HMM fork is
//  recorded here as the deferred upgrade.

#include <algorithm> // std::sort
#include <bit>
#include <array>     // std::array (fixed-K per-regime Sharpe)
#include <cmath>     // std::isnan
#include <cstddef>
#include <cstdint>   // (u8 sentinel)
#include <limits>    // std::numeric_limits (kNoRegime sentinel)
#include <span>      // std::span
#include <set>
#include <string_view>
#include <utility>
#include <vector>    // std::vector

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // atx::f64, atx::u8, atx::usize

#include "atx/engine/alpha/panel.hpp"    // alpha::Panel (close field for vol)
#include "atx/engine/eval/stats_ext.hpp" // eval::mean_std_pop, MeanStd, median

namespace atx::engine::eval {

// ===========================================================================
//  kNumRegimes — the shipped tercile partition has exactly three regimes
//  {0=low, 1=mid, 2=high}. A fixed compile-time K so per_regime_sharpe can
//  return a stack std::array and RobustnessVerdict can carry a fixed-size slot.
// ===========================================================================
inline constexpr atx::usize kNumRegimes = 3;

// ===========================================================================
//  kNoRegime — the sentinel label for an UNLABELLED date: a warm-up date with
//  fewer than `vol_window` prior bars has no trailing realized vol, so it is
//  excluded from every regime slice (its PnL contributes to no per-regime
//  Sharpe). A u8 value strictly >= kNumRegimes (so `label < kNumRegimes` is the
//  "is labelled" predicate). 255 is unmistakable and never a real tercile id.
// ===========================================================================
inline constexpr atx::u8 kNoRegime = std::numeric_limits<atx::u8>::max();

enum class RegimeSliceRule : atx::u8 { LegacyFullSampleV1 = 1, ExpandingPastV2 = 2 };
[[nodiscard]] inline constexpr std::string_view regime_slice_rule_name(RegimeSliceRule rule) noexcept {
  switch (rule) {
  case RegimeSliceRule::LegacyFullSampleV1: return "legacy-full-sample-v1";
  case RegimeSliceRule::ExpandingPastV2: return "expanding-past-v2";
  }
  return "unknown";
}

// ===========================================================================
//  RobustnessConfig — the knobs robustness_verdict screens on.
//
//  vol_window       : trailing window length (bars) for the realized-vol that
//                     drives the tercile partition. A date with fewer than this
//                     many priors is unlabelled (kNoRegime).
//  min_regime_sharpe: the per-period OOS Sharpe FLOOR an alpha must clear in
//                     EVERY regime AND EVERY walk-forward window to be robust.
//  n_walk_forward   : number of contiguous rolling walk-forward windows.
// ===========================================================================
struct RobustnessConfig {
  atx::usize vol_window = 10;
  atx::f64 min_regime_sharpe = 0.0;
  atx::usize n_walk_forward = 4;
  RegimeSliceRule regime_rule = RegimeSliceRule::ExpandingPastV2;
  atx::usize min_regime_history = 3; // prior finite volatility dates; V2 requires >=3
};

// Explicit recipe identity for persisted robustness outputs. V1 callers retain
// their old digest path; the identity is still available as report metadata.
[[nodiscard]] inline atx::u64 regime_recipe_id(const RobustnessConfig& cfg) noexcept {
  atx::u64 hash = 14695981039346656037ULL;
  const std::array<atx::u64, 5> words{static_cast<atx::u64>(cfg.regime_rule),
      static_cast<atx::u64>(cfg.vol_window), static_cast<atx::u64>(cfg.min_regime_history),
      static_cast<atx::u64>(cfg.n_walk_forward), std::bit_cast<atx::u64>(cfg.min_regime_sharpe)};
  for (auto word : words)
    for (unsigned byte = 0; byte < 8U; ++byte) {
      hash = (hash ^ (word & 255U)) * 1099511628211ULL;
      word >>= 8U;
    }
  return hash;
}

// ===========================================================================
//  RobustnessVerdict — the plain copyable value the S4.5 gate (and S4.4b driver
//  hook) consumes. Aggregate (Rule of Zero); owns only its walk_forward vector.
//
//  regime_sharpe        : per-tercile per-period Sharpe {low, mid, high}. An
//                         empty/degenerate regime carries 0 (mean_std_pop std==0).
//  walk_forward_sharpe  : per-window per-period Sharpe, in ascending time order.
//  full_sample_sharpe   : the per-period Sharpe over the WHOLE OOS stream — the
//                         naive number robustness slicing exists to second-guess.
//  worst_regime_sharpe  : min over regime_sharpe (the binding regime constraint).
//  worst_window_sharpe  : min over walk_forward_sharpe (the binding window one).
//  recovery_corr        : OPTIONAL planted-signal recovery correlation
//                         (synthetic_alpha.hpp). NaN when not measured — a
//                         downstream gate that does not run the synthetic probe
//                         simply ignores it. Folded here so S4.5 reads ONE struct.
//  is_robust            : worst_regime_sharpe >= min_regime_sharpe AND
//                         worst_window_sharpe >= min_regime_sharpe. (recovery_corr
//                         is reported, NOT gated here — the S4.5 gate decides its
//                         own bar; keeping is_robust a pure regime/window verdict
//                         keeps this struct's contract stable.)
// ===========================================================================
struct RobustnessVerdict {
  std::array<atx::f64, kNumRegimes> regime_sharpe{};
  std::vector<atx::f64> walk_forward_sharpe;
  atx::f64 full_sample_sharpe{0.0};
  atx::f64 worst_regime_sharpe{0.0};
  atx::f64 worst_window_sharpe{0.0};
  atx::f64 recovery_corr{std::numeric_limits<atx::f64>::quiet_NaN()};
  bool is_robust{false};
  RegimeSliceRule regime_rule{RegimeSliceRule::ExpandingPastV2};
  atx::u64 regime_recipe{0};
  std::array<atx::usize, kNumRegimes> regime_observations{};
  bool regime_coverage_complete{false};
};

// ===========================================================================
//  sharpe_pp — per-period (NON-annualized) Sharpe of a slice: mean/std over the
//  span, ascending-index. A constant or empty slice (std == 0) yields 0 — the
//  degenerate convention shared with the eval spine (pbo.hpp subset_sharpe).
//  Pure; reuses eval::mean_std_pop so there is ONE moment definition.
// ===========================================================================
[[nodiscard]] inline atx::f64 sharpe_pp(std::span<const atx::f64> r) noexcept {
  const MeanStd ms = mean_std_pop(r);
  return (ms.std == 0.0) ? 0.0 : ms.mean / ms.std;
}

namespace detail {

// ---------------------------------------------------------------------------
//  market_return_at — the cross-sectional MEDIAN simple return at date t over a
//  panel's close field: median_j ( close(t,j)/close(t-1,j) - 1 ), skipping NaN /
//  non-positive-prior cells (out-of-universe). t == 0 has no prior -> 0. The
//  median (not mean) is the robust market proxy the tercile vol is measured on.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 market_return_at(const alpha::Panel &panel, alpha::FieldId close_id,
                                               atx::usize t) {
  if (t == 0U) {
    return 0.0;
  }
  const atx::usize insts = panel.instruments();
  const std::span<const atx::f64> prev = panel.field_cross_section(close_id, t - 1U);
  const std::span<const atx::f64> cur = panel.field_cross_section(close_id, t);
  std::vector<atx::f64> rets;
  rets.reserve(insts);
  for (atx::usize j = 0; j < insts; ++j) {
    const atx::f64 p = prev[j];
    const atx::f64 c = cur[j];
    if (std::isnan(p) || std::isnan(c) || p <= 0.0) {
      continue; // out-of-universe / not-yet-listed: no defined return
    }
    rets.push_back(c / p - 1.0);
  }
  if (rets.empty()) {
    return 0.0;
  }
  return median(std::span<const atx::f64>{rets});
}

// ---------------------------------------------------------------------------
//  trailing_vol_at — population std of the market (cross-sectional-median)
//  returns over the trailing window [t-window, t). PRECONDITION: t >= window
//  (the caller only labels dates with a full window). Ascending-index reduction.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 trailing_vol_at(const alpha::Panel &panel, alpha::FieldId close_id,
                                              atx::usize t, atx::usize window) {
  ATX_ASSERT(t >= window);
  std::vector<atx::f64> win;
  win.reserve(window);
  for (atx::usize s = t - window; s < t; ++s) {
    win.push_back(market_return_at(panel, close_id, s));
  }
  return mean_std_pop(std::span<const atx::f64>{win}).std;
}

} // namespace detail

namespace detail {

// One finite-only median per date, computed once. Missing market returns remain
// unavailable; t=0 is never a fabricated zero return in V2.
[[nodiscard]] inline std::vector<atx::f64>
causal_market_returns(const alpha::Panel& panel, alpha::FieldId close) {
  std::vector<atx::f64> history(panel.dates(), std::numeric_limits<atx::f64>::quiet_NaN());
  std::vector<atx::f64> values;
  values.reserve(panel.instruments());
  for (atx::usize t = 1; t < panel.dates(); ++t) {
    values.clear();
    const auto prior = panel.field_cross_section(close, t - 1);
    const auto current = panel.field_cross_section(close, t);
    for (atx::usize i = 0; i < panel.instruments(); ++i) {
      if (!std::isfinite(prior[i]) || !std::isfinite(current[i]) || prior[i] <= 0.0 || current[i] <= 0.0)
        continue;
      const auto value = current[i] / prior[i] - 1.0;
      if (std::isfinite(value)) values.push_back(value);
    }
    if (values.empty()) continue;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2U);
    std::nth_element(values.begin(), middle, values.end());
    history[t] = *middle;
    if (values.size() % 2U == 0U)
      history[t] = *std::max_element(values.begin(), middle) * 0.5 + *middle * 0.5;
  }
  return history;
}

// A chronological, two-pass population standard deviation with a window-local
// scale. No future normalization and no rolling NaN/overflow poison to recover.
[[nodiscard]] inline atx::f64 finite_window_vol(std::span<const atx::f64> values) noexcept {
  if (values.empty()) return std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 scale = 0.0;
  for (auto value : values) {
    if (!std::isfinite(value)) return std::numeric_limits<atx::f64>::quiet_NaN();
    scale = std::max(scale, std::abs(value));
  }
  if (scale == 0.0) return 0.0;
  atx::f64 mean = 0.0;
  for (auto value : values) mean += value / scale;
  mean /= static_cast<atx::f64>(values.size());
  atx::f64 variance = 0.0;
  for (auto value : values) { const auto delta = value / scale - mean; variance += delta * delta; }
  return scale * std::sqrt(variance / static_cast<atx::f64>(values.size()));
}

// Online ordered quantiles: O(log T) insertion and O(1) cut updates. The two
// iterators track floor((n-1)/3) and floor(2*(n-1)/3); no future coordinates exist.
class ExpandingVolCuts {
  using Values = std::multiset<std::pair<atx::f64, atx::usize>>;
public:
  [[nodiscard]] atx::usize size() const noexcept { return values_.size(); }
  [[nodiscard]] atx::u8 label(atx::f64 value) const noexcept {
    return value < low_->first ? atx::u8{0} : value > high_->first ? atx::u8{2} : atx::u8{1};
  }
  void add(atx::f64 value, atx::usize date) {
    const auto previous = values_.size();
    const auto inserted = values_.emplace(value, date);
    if (previous == 0U) { low_ = high_ = inserted; return; }
    auto low_rank = (previous - 1U) / 3U;
    auto high_rank = upper_rank(previous);
    if (*inserted < *low_) ++low_rank;
    if (*inserted < *high_) ++high_rank;
    const auto want_low = previous / 3U;
    const auto want_high = upper_rank(previous + 1U);
    while (low_rank < want_low) { ++low_; ++low_rank; }
    while (low_rank > want_low) { --low_; --low_rank; }
    while (high_rank < want_high) { ++high_; ++high_rank; }
    while (high_rank > want_high) { --high_; --high_rank; }
  }
private:
  [[nodiscard]] static atx::usize upper_rank(atx::usize count) noexcept {
    const auto n = count - 1U;
    return 2U * (n / 3U) + 2U * (n % 3U) / 3U;
  }
  Values values_;
  Values::const_iterator low_{values_.end()}, high_{values_.end()};
};

} // namespace detail

// ===========================================================================
//  legacy_regime_labels — frozen retrospective full-sample partition.
//
//  For each date t >= vol_window, compute the trailing realized vol of the
//  market (cross-sectional-median) return over [t-vol_window, t). Sort the
//  labelled dates by (vol, date) ASCENDING and assign the lower third -> low (0),
//  the middle third -> mid (1), the upper third -> high (kNumRegimes-1) by RANK
//  POSITION (so the partition is balanced and ties resolve to the earlier date).
//  Warm-up dates (t < vol_window) get the kNoRegime sentinel. A panel without a
//  "close" field, or with vol_window == 0 / no labelled dates, returns an all-
//  sentinel vector (no regime information). PURE; no RNG.
//
//  `n_regimes` is accepted for API symmetry with the spec but MUST equal
//  kNumRegimes (the shipped partition is terciles); other values trip the assert.
// ===========================================================================
[[nodiscard]] inline std::vector<atx::u8> legacy_regime_labels(const alpha::Panel &panel,
                                                        atx::usize vol_window,
                                                        atx::usize n_regimes = kNumRegimes) {
  ATX_ASSERT(n_regimes == kNumRegimes);
  (void)n_regimes;
  const atx::usize dates = panel.dates();
  std::vector<atx::u8> labels(dates, kNoRegime);

  const auto close = panel.field_id("close");
  if (!close.has_value() || vol_window == 0U || dates <= vol_window) {
    return labels; // no vol basis -> every date unlabelled
  }
  const alpha::FieldId close_id = *close;

  // (date, trailing-vol) for every labellable date, in ascending date order.
  std::vector<std::pair<atx::usize, atx::f64>> vols;
  vols.reserve(dates - vol_window);
  for (atx::usize t = vol_window; t < dates; ++t) {
    vols.emplace_back(t, detail::trailing_vol_at(panel, close_id, t, vol_window));
  }
  // Sort by (vol, date) ascending: ties on vol resolve to the EARLIER date.
  std::sort(vols.begin(), vols.end(),
            [](const std::pair<atx::usize, atx::f64> &a,
               const std::pair<atx::usize, atx::f64> &b) noexcept {
              return (a.second != b.second) ? (a.second < b.second) : (a.first < b.first);
            });
  // Rank-position tercile assignment: balanced thirds, lowest vol -> regime 0.
  const atx::usize m = vols.size();
  for (atx::usize rank = 0; rank < m; ++rank) {
    // regime = floor(rank * K / m), clamped to [0, K-1]; an exactly-balanced
    // split for m divisible by K, near-balanced otherwise. The multiply-then-
    // divide keeps integer thirds without float rounding.
    atx::usize g = (rank * kNumRegimes) / m;
    if (g >= kNumRegimes) {
      g = kNumRegimes - 1U;
    }
    labels[vols[rank].first] = static_cast<atx::u8>(g);
  }
  return labels;
}

// V2 trailing volatility at t uses returns [t-window,t), and cuts use only
// finite volatility observations at dates <t. No label is retrospectively revised.
// Full return windows and >=min_history prior vol observations are required.
[[nodiscard]] inline std::vector<atx::u8> regime_labels(const alpha::Panel& panel,
    atx::usize vol_window, atx::usize n_regimes = kNumRegimes,
    RegimeSliceRule rule = RegimeSliceRule::ExpandingPastV2, atx::usize min_history = 3) {
  if (rule == RegimeSliceRule::LegacyFullSampleV1)
    return legacy_regime_labels(panel, vol_window, n_regimes);
  std::vector<atx::u8> labels(panel.dates(), kNoRegime);
  const auto close = panel.field_id("close");
  if (rule != RegimeSliceRule::ExpandingPastV2 || n_regimes != kNumRegimes ||
      !close || vol_window == 0U || panel.dates() <= vol_window || min_history < 3U) return labels;
  const auto returns = detail::causal_market_returns(panel, *close);
  detail::ExpandingVolCuts cuts;
  for (atx::usize t = vol_window + 1U; t < panel.dates(); ++t) {
    const auto vol = detail::finite_window_vol(std::span<const atx::f64>{returns}.subspan(t - vol_window, vol_window));
    if (!std::isfinite(vol)) continue;
    if (cuts.size() >= min_history) labels[t] = cuts.label(vol);
    cuts.add(vol, t); // insertion AFTER classification is the fit/apply boundary
  }
  return labels;
}

[[nodiscard]] inline std::vector<atx::u8> regime_labels(const alpha::Panel& panel,
                                                       const RobustnessConfig& cfg) {
  return regime_labels(panel, cfg.vol_window, kNumRegimes, cfg.regime_rule, cfg.min_regime_history);
}

// ===========================================================================
//  per_regime_sharpe — per-period Sharpe of `alpha_pnl` WITHIN each regime.
//
//  Gathers the PnL of the dates carrying each label (ascending index) and takes
//  sharpe_pp of each gathered slice. K is the regime count (== kNumRegimes for
//  the shipped partition). A regime with no dates -> 0 (empty slice). PRECONDITION
//  (debug): alpha_pnl.size() == labels.size(). PURE; no allocation on the hot
//  path beyond the per-regime gather (this is a COLD research call).
// ===========================================================================
template <atx::usize K = kNumRegimes>
[[nodiscard]] std::array<atx::f64, K> per_regime_sharpe(std::span<const atx::f64> alpha_pnl,
                                                        std::span<const atx::u8> labels) {
  ATX_ASSERT(alpha_pnl.size() == labels.size());
  const atx::usize n = alpha_pnl.size();
  std::array<atx::f64, K> out{};
  std::array<std::vector<atx::f64>, K> buckets{};
  for (atx::usize t = 0; t < n; ++t) {
    const atx::u8 g = labels[t];
    if (g < K) {
      buckets[g].push_back(alpha_pnl[t]);
    }
  }
  for (atx::usize g = 0; g < K; ++g) {
    out[g] = sharpe_pp(std::span<const atx::f64>{buckets[g]});
  }
  return out;
}

// ===========================================================================
//  walk_forward_sharpe — per-period Sharpe over n contiguous, DISJOINT rolling
//  windows of `alpha_pnl`, in ascending time order.
//
//  The stream is split into `n_windows` near-equal contiguous blocks (the i-th
//  spans [i*n/n_windows, (i+1)*n/n_windows) — the cpcv.hpp group_start geometry).
//  Each window's Sharpe is sharpe_pp of its block. n_windows == 1 is the full-
//  sample Sharpe; n_windows is capped at the stream length (one period per window
//  max) and floored at 1, so an empty stream yields one window of Sharpe 0. NO
//  window overlaps another (disjoint partition — the firewall note). PURE.
// ===========================================================================
[[nodiscard]] inline std::vector<atx::f64> walk_forward_sharpe(std::span<const atx::f64> alpha_pnl,
                                                               atx::usize n_windows) {
  const atx::usize n = alpha_pnl.size();
  // Cap windows at the stream length (a window must hold >= 1 period); floor at 1.
  atx::usize w = n_windows;
  if (w == 0U) {
    w = 1U;
  }
  if (n != 0U && w > n) {
    w = n;
  }
  std::vector<atx::f64> out;
  out.reserve(w);
  for (atx::usize i = 0; i < w; ++i) {
    // group_start geometry (cpcv.hpp): contiguous near-equal blocks; begin..end.
    const atx::usize begin = (i * n) / w;
    const atx::usize end = ((i + 1U) * n) / w;
    out.push_back(sharpe_pp(alpha_pnl.subspan(begin, end - begin)));
  }
  return out;
}

namespace detail {

// min over a span of Sharpes; an empty span yields 0 (no binding constraint).
[[nodiscard]] inline atx::f64 min_sharpe(std::span<const atx::f64> v) noexcept {
  if (v.empty()) {
    return 0.0;
  }
  atx::f64 lo = v[0];
  for (const atx::f64 x : v) {
    if (x < lo) {
      lo = x;
    }
  }
  return lo;
}

} // namespace detail

// ===========================================================================
//  robustness_verdict — fold per-regime + walk-forward survival into the verdict.
//
//  An alpha is ROBUST iff its worst per-regime OOS Sharpe AND its worst walk-
//  forward-window Sharpe BOTH clear cfg.min_regime_sharpe — survival in EVERY
//  vol regime and across EVERY rolling window, not merely full-sample. V2 also
//  requires at least two finite observations in each regime; missing slices are
//  explicitly unqualified even though the legacy descriptive Sharpe slot is zero.
//  The
//  full_sample_sharpe is reported for contrast (the naive number). recovery_corr
//  is left NaN here (the caller folds in synthetic_alpha::recovery_correlation
//  when it runs the planted-signal probe). PRECONDITION (debug): alpha_pnl and
//  labels are the same length. PURE; reuses per_regime_sharpe / walk_forward_sharpe.
// ===========================================================================
[[nodiscard]] inline RobustnessVerdict robustness_verdict(std::span<const atx::f64> alpha_pnl,
                                                          std::span<const atx::u8> labels,
                                                          const RobustnessConfig &cfg) {
  ATX_ASSERT(alpha_pnl.size() == labels.size());
  RobustnessVerdict v;
  v.regime_rule = cfg.regime_rule;
  v.regime_recipe = regime_recipe_id(cfg);
  for (atx::usize t = 0; t < labels.size(); ++t)
    if (labels[t] < kNumRegimes && std::isfinite(alpha_pnl[t])) ++v.regime_observations[labels[t]];
  v.regime_coverage_complete = std::all_of(v.regime_observations.begin(), v.regime_observations.end(),
                                         [](auto count) { return count >= 2U; });
  v.regime_sharpe = per_regime_sharpe<kNumRegimes>(alpha_pnl, labels);
  v.walk_forward_sharpe = walk_forward_sharpe(alpha_pnl, cfg.n_walk_forward);
  v.full_sample_sharpe = sharpe_pp(alpha_pnl);
  v.worst_regime_sharpe =
      detail::min_sharpe(std::span<const atx::f64>{v.regime_sharpe.data(), v.regime_sharpe.size()});
  v.worst_window_sharpe = detail::min_sharpe(std::span<const atx::f64>{v.walk_forward_sharpe});
  v.is_robust = (v.worst_regime_sharpe >= cfg.min_regime_sharpe) &&
                (v.worst_window_sharpe >= cfg.min_regime_sharpe);
  if (cfg.regime_rule != RegimeSliceRule::LegacyFullSampleV1)
    v.is_robust = v.is_robust && cfg.regime_rule == RegimeSliceRule::ExpandingPastV2 && v.regime_coverage_complete;
  return v;
}

} // namespace atx::engine::eval

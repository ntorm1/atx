#pragma once

// Daily one-day-hold structure PnL + feature panel (SPY structure-selector ML).
//
// The structure-selector research loop needs, per trading day t:
//   labels   — the delta-neutral PnL, t -> t+1, of freshly-resolved vega-
//              normalized ATMF straddles at a short ("front") and long ("back")
//              tenor. Calendar strategies derive linearly from these two
//              (vega-matched short-front/long-back = pnl_back - pnl_front).
//   features — entry-day surface state (ATM term structure, skew, wings,
//              forward vol) and trailing history (realized vol, spot/IV
//              momentum, vol-of-vol, carry) with NO lookahead.
//
// Contract highlights:
//   * Legs are pinned to an ABSOLUTE expiry (entry now_ts + T·year); the mark
//     recomputes T from that expiry, so theta decay is inside the mark-to-mark
//     difference and a mark past expiry is an error, never a fabricated value.
//   * PnL is frictionless per-share $: sum(qty·dP) − entry_delta·dS. The delta
//     hedge is fixed at entry — "delta-neutral pnl from today to tomorrow".
//   * The builder is streaming and fail-soft: a day whose structures cannot be
//     resolved/priced still emits its row with `pnl_valid == false` (counted),
//     never a made-up mark. Configuration errors are hard failures.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/api/core/types.hpp" // Result, Side

namespace atx::vol {

class PricedSurface;

// One synthetic option leg of a resolved structure. `expiry_ts_ns` is absolute
// so a later mark re-derives its own T; qty is signed fractional contracts on
// per-share pricing (no contract multiplier anywhere in this module).
struct StructureLeg {
  double strike{0.0};
  std::int64_t expiry_ts_ns{0};
  Side side{Side::Call};
  double qty{0.0};
};

// A structure resolved against one entry surface: legs plus entry marks and
// qty-scaled book greeks (theta annualized, calendar-time convention).
struct ResolvedStructure {
  std::vector<StructureLeg> legs;
  std::int64_t entry_ts_ns{0};
  double spot{0.0};        // entry surface spot S0
  double entry_value{0.0}; // sum(qty · price)
  double entry_delta{0.0}; // sum(qty · delta) — the fixed hedge ratio
  double entry_gamma{0.0};
  double entry_vega{0.0}; // == sign · vega_target by construction
  double entry_theta{0.0};
  double entry_vanna{0.0};
  double entry_volga{0.0};
};

// Resolve an ATM-forward straddle at `target_T`, both legs at K = F(T), sized
// so the structure vega (dP/dσ units) equals `sign · vega_target`.
// @param sign  +1.0 long / -1.0 short (exactly; anything else is rejected).
// @return InvalidArgument for a non-finite/non-positive T or vega_target, a
//         degenerate forward, or a non-positive structure vega; pricing errors
//         propagate.
[[nodiscard]] Result<ResolvedStructure>
resolve_atmf_straddle(const PricedSurface &entry, double target_T, double vega_target, double sign);

// Delta-neutral mark-to-mark PnL of `s` on `mark`:
//     sum(qty · (P1 − P0)) − entry_delta · (S1 − S0)
// with each leg's T re-derived from its pinned expiry at the mark's valuation
// timestamp. @return InvalidArgument if any leg has expired (T ≤ 0) or the mark
// does not postdate the entry; leg pricing errors propagate.
[[nodiscard]] Result<double> delta_neutral_pnl(const ResolvedStructure &s,
                                               const PricedSurface &mark);

// Panel configuration. Feature tenors for the IV level columns are fixed at
// the canonical {1m, 3m, 1y} grid; `front_T`/`back_T` set the STRUCTURE tenors
// (and the forward-vol feature bridging them).
struct StructurePanelConfig {
  double front_T{30.0 / 365.25};
  double back_T{1.0};
  double vega_target{1000.0};
  double rr_delta{0.25}; // wing |delta| for the risk-reversal / butterfly columns
};

// One completed panel row: entry-day features + next-day labels. History-
// windowed columns are NaN until their window is fully observable — a NaN is
// "not yet measurable", never a placeholder for a failed computation the row
// pretends succeeded.
struct PanelRow {
  std::string key; // entry session key (ISO date)

  // entry-day surface state
  double spot{0.0};
  double r{0.0};
  double iv_1w{0.0};
  double iv_1m{0.0};
  double iv_3m{0.0};
  double iv_1y{0.0};
  double short_slope{0.0}; // iv_1m − iv_1w (short-end steepness)
  double vsw_1m{0.0};      // model-free var-swap vol at 1m (NaN on strip failure)
  double vsw_1y{0.0};
  double vsw_conv_1m{0.0}; // vsw_1m − iv_1m (smile-convexity content)
  double term_slope{0.0};         // iv_1y − iv_1m
  double fwd_vol_front_back{0.0}; // forward vol between the structure tenors
  double fwd_minus_front{0.0};    // fwd_vol_front_back − atmf_vol(front_T)
  double skew_1m{0.0};            // ∂σ/∂k at k_ref = σ_atm·√T
  double curv_1m{0.0};
  double skew_1y{0.0};
  double curv_1y{0.0};
  double rr25_1m{0.0}; // σ(Δp) − σ(Δc) at rr_delta (NaN if wing unreachable)
  double bf25_1m{0.0};
  double rr25_1y{0.0};
  double bf25_1y{0.0};

  // trailing-history features (entry day inclusive; NaN until window filled)
  double rv5{0.0};
  double rv21{0.0};
  double rv63{0.0};
  double ivrv_1m_21{0.0}; // iv_1m − rv21
  double ivrv_1y_63{0.0}; // iv_1y − rv63
  double ret_1d{0.0};
  double ret_5d{0.0};
  double ret_21d{0.0};
  double div_1m_1d{0.0}; // Δ iv_1m over the window
  double div_1m_5d{0.0};
  double div_1m_21d{0.0};
  double dslope_1d{0.0}; // Δ term_slope
  double dslope_5d{0.0};
  double vol_of_vol_21{0.0}; // stdev of daily Δ iv_1m over 21 diffs
  double vrp_mean_63{0.0};   // trailing mean of (iv_1m − rv21)

  // unit-structure entry greeks (diagnostics / features)
  double front_gamma{0.0};
  double front_theta{0.0};
  double front_delta{0.0};
  double front_vanna{0.0};
  double front_volga{0.0};
  double back_gamma{0.0};
  double back_theta{0.0};
  double back_delta{0.0};
  double back_vanna{0.0};
  double back_volga{0.0};

  // labels: 1-day delta-neutral PnL of the vega-normalized structures entered
  // on `key` and marked on the NEXT pushed session. NaN when pnl_valid is false.
  double pnl_front{0.0};
  double pnl_back{0.0};
  bool pnl_valid{false};
};

// Tab-separated header/row emitters (stable column order, `key` first;
// pnl_valid encoded 0/1). Kept in the library so downstream consumers and the
// tests agree on one schema.
[[nodiscard]] std::string panel_tsv_header();
[[nodiscard]] std::string to_tsv_line(const PanelRow &row);

// Streaming builder: push consecutive daily surfaces in strictly ascending key
// order; each push completes (and returns) the PREVIOUS day's row once its
// next-day mark is known. O(history) memory, surfaces are never retained.
class StructurePanelBuilder {
public:
  explicit StructurePanelBuilder(StructurePanelConfig cfg = {});

  // @return the completed row for the previously pushed session (nullopt on the
  //         first push). InvalidArgument on a bad config, a non-ascending key,
  //         or a non-positive valuation timestamp; per-day structure failures
  //         are SOFT (row emitted with pnl_valid == false, counted in
  //         `skipped()`), never fabricated.
  [[nodiscard]] Result<std::optional<PanelRow>> push(const std::string &key,
                                                     const PricedSurface &surf);

  // Sessions whose labels could not be computed (resolve or mark failure).
  [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }

  // End-of-stream: surrender the final pending session's row — features
  // complete, labels NaN, pnl_valid false (its next-day mark never arrived).
  // The operational predict path needs exactly this row: the latest session's
  // features ARE the live decision input. Idempotent: nullopt when nothing is
  // pending. Does not count toward `skipped()` — an unfinished label at the
  // corpus edge is not a failure.
  [[nodiscard]] std::optional<PanelRow> finish();

private:
  struct Pending {
    PanelRow row;
    std::optional<ResolvedStructure> front;
    std::optional<ResolvedStructure> back;
  };

  StructurePanelConfig cfg_;
  std::optional<Pending> prev_;
  std::string last_key_;
  std::uint64_t skipped_{0};

  // trailing per-session history, entry day inclusive (parallel vectors)
  std::vector<std::int64_t> ts_hist_;
  std::vector<double> spot_hist_;
  std::vector<double> iv1m_hist_;
  std::vector<double> slope_hist_;
  std::vector<double> ivrv_hist_; // iv_1m − rv21 (NaN until rv21 fills)
};

} // namespace atx::vol

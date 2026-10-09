#pragma once

// Cross-sectional vega panel — per-(symbol, entry date) surface/history
// features + h-day daily-rehedged ATMF-strangle hold labels.
//
// The cross-sectional vol-carry research loop needs, per (symbol, session t):
//   labels   — the h-session daily-rehedged hold PnL of a freshly-resolved,
//              vega-normalized, delta-targeted ATMF strangle entered at t
//              (plus the first day of that series as a 1-day label).
//   features — entry-day surface state (ATM term structure, skew, wings,
//              forward vol) and trailing own-history (realized vol, IV
//              momentum, vol-of-vol, IV rank) with NO lookahead.
//
// Contract highlights:
//   * Structures are ATMF strangles: call leg at American delta
//     +target_abs_delta, put leg at −target_abs_delta (strikes solved via
//     resolve_strike_by_delta, strategy.hpp), one shared qty scaled so the
//     structure vega equals sign · vega_target. Legs are pinned to an ABSOLUTE
//     expiry (entry now_ts + T·year); every later mark re-derives its own T
//     from that expiry, so a mark past expiry is an error, never a fabricated
//     value.
//   * Labels are DAILY-REHEDGED (unlike structure_panel.hpp's entry-fixed
//     hedge): each session the spot hedge resets to the structure's net delta
//     on the PREVIOUS session's surface,
//         pnl_t = Σ_legs qty·(P_t − P_{t−1}) − Δ_net,t−1·(S_t − S_{t−1}),
//     with P/Δ at t−1 = the entry marks for t = 0. FRICTIONLESS and
//     UNFINANCED: no trading costs, no carry on the hedge or the premium.
//   * The builder is streaming and fail-soft: a day whose strangle cannot be
//     resolved, or whose hold window hits an unpriceable mark, still emits its
//     row with `label_valid == 0` (counted in skipped()), never a made-up
//     label. Configuration errors are hard failures.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "backtest/structure_panel.hpp" // StructureLeg, ResolvedStructure
#include "atx/vol/api/core/types.hpp"           // Result, Side

namespace atx::vol {

class PricedSurface;

// Resolve a vega-normalized delta-targeted ATMF strangle at `target_T`:
// legs[0] = call at American delta +target_abs_delta, legs[1] = put at
// −target_abs_delta, both at one shared qty scaled so the structure vega
// (dP/dσ units) equals `sign · vega_target`. Legs pinned to the absolute
// expiry now_ts + target_T years. target_abs_delta == 0.5 is a valid target
// but is NOT required to coincide with resolve_atmf_straddle's K = F legs.
// @param sign  +1 long / -1 short (exactly; anything else is rejected).
// @return InvalidArgument for a non-finite/non-positive T or vega_target, a
//         target |delta| outside (0,1), a degenerate forward, an unreachable
//         delta target, or a non-positive structure vega; pricing errors
//         propagate.
[[nodiscard]] Result<ResolvedStructure> resolve_atmf_strangle(const PricedSurface &entry,
                                                              double target_T,
                                                              double target_abs_delta,
                                                              double vega_target, int sign);

// Daily-rehedged mark-to-mark PnL series of `s` over consecutive sessions.
// `marks[0..h-1]` are the surfaces for the sessions AFTER entry, in strictly
// ascending valuation-ts order:
//     pnl_t = Σ_legs qty·(P_t − P_{t−1}) − Δ_net,t−1·(S_t − S_{t−1})
// where P_{−1}/S_{−1}/Δ_net,−1 are the entry marks and Δ_net,t is the
// structure's net delta evaluated on session t's surface with each leg's
// tenor re-derived from its pinned expiry. Frictionless, no financing (see
// the module banner). The SUM of the series is the hold label; the per-day
// series is for diagnostics.
// @return InvalidArgument on an empty structure, an empty/null-bearing marks
//         span, a mark that does not strictly postdate its predecessor (or
//         the entry), or a leg expired at any mark (T ≤ 0); leg pricing
//         errors propagate. Any error means the whole label is unbuildable —
//         callers count and skip, never fabricate a partial series.
[[nodiscard]] Result<std::vector<double>>
hedged_daily_pnls(const ResolvedStructure &s, std::span<const PricedSurface *const> marks);

// Sum of `hedged_daily_pnls` — the h-day hold label. Same error contract.
[[nodiscard]] Result<double> hedged_hold_pnl(const ResolvedStructure &s,
                                             std::span<const PricedSurface *const> marks);

// Panel configuration. Feature tenors for the IV level columns are fixed at
// the canonical {1m, 3m, 1y} grid; `tenor_T` sets the STRUCTURE tenor.
struct VegaPanelConfig {
  double tenor_T{1.0};
  double target_abs_delta{0.30};
  double vega_target{1000.0};
  int horizon_sessions{21};
};

// One completed panel row: entry-day features + h-day hold labels. History-
// windowed columns are NaN until their window is fully observable — a NaN is
// "not yet measurable", never a placeholder for a failed computation the row
// pretends succeeded. All feature windows are trailing / entry-day inclusive:
// no lookahead.
struct VegaPanelRow {
  std::string key;    // entry session key (ISO date)
  std::string symbol; // underlying symbol this row belongs to

  // entry-day surface state
  double spot{0.0};
  double r{0.0};
  double iv_1m{0.0};
  double iv_3m{0.0};
  double iv_1y{0.0};
  double term_slope_1m_1y{0.0}; // iv_1y − iv_1m
  double fwd_vol_1m_1y{0.0};    // forward vol bridging 1m -> 1y
  double skew_1m{0.0};          // ∂σ/∂k at k_ref = σ_atm·√T
  double curv_1m{0.0};
  double skew_1y{0.0};
  double curv_1y{0.0};
  double rr25_1y{0.0}; // σ(Δp) − σ(Δc) at |Δ| = 0.25 (NaN if wing unreachable)
  double bf25_1y{0.0};

  // trailing-history features (entry day inclusive; NaN until window filled)
  double rv_21{0.0};
  double rv_63{0.0};
  double rv_252{0.0};
  double ivrv_1y_21{0.0}; // iv_1y − rv_21
  double ivrv_1y_63{0.0}; // iv_1y − rv_63
  double ret_21d{0.0};
  double div_1y_21{0.0};      // 21d change in iv_1y
  double vol_of_vol_21{0.0};  // stdev of 1d iv_1y changes over 21 diffs
  double iv_1y_rank_252{0.0}; // percentile rank of iv_1y in trailing 252 own-history

  // entry-structure greeks / strikes (diagnostics / features)
  double entry_vega{0.0}; // == +vega_target by construction (long panel entry)
  double entry_gamma{0.0};
  double entry_theta{0.0};
  double entry_delta_net{0.0};
  double strike_call{0.0};
  double strike_put{0.0};

  // labels: h-day daily-rehedged hold PnL of the strangle entered on `key`
  // (sum + first day). NaN when label_valid is false.
  double label_pnl_h{0.0};
  double label_pnl_1d{0.0};
  bool label_valid{false};
};

// Tab-separated header/row emitters (stable column order, `key` then `symbol`
// first; label_valid encoded 0/1). Kept in the library so downstream
// consumers and the tests agree on one schema.
[[nodiscard]] std::string vega_panel_tsv_header();
[[nodiscard]] std::string to_tsv_line(const VegaPanelRow &row);

// Streaming per-symbol builder: push consecutive daily surfaces in strictly
// ascending key order; a row completes (and is returned) once its entry has
// observed `horizon_sessions` later marks. The hold PnL is accumulated
// INCREMENTALLY — each push marks every pending entry against the new surface
// using the per-entry (value, net delta, spot) cached from the previous push —
// so no surface is ever retained. Memory: O(history vectors) + at most
// `horizon_sessions` pending entries.
class VegaPanelBuilder {
public:
  explicit VegaPanelBuilder(VegaPanelConfig cfg = {}, std::string symbol = {});

  // @return the completed row whose h-session label window filled on THIS push
  //         (nullopt otherwise; entries complete strictly oldest-first).
  //         InvalidArgument on a bad config, a non-ascending key, or a
  //         non-positive / non-ascending valuation timestamp; per-day resolve
  //         or mark failures are SOFT (row emitted with label_valid == 0,
  //         counted in `skipped()`), never fabricated.
  [[nodiscard]] Result<std::optional<VegaPanelRow>> push(const std::string &key,
                                                         const PricedSurface &surf);

  // Entries whose labels could not be computed (resolve or mark failure).
  [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }

  // End-of-stream: surrender every still-pending entry, oldest first —
  // features complete, labels NaN, label_valid false (their hold windows never
  // filled). Idempotent: empty when nothing is pending. Does not count toward
  // `skipped()` — an unfinished label at the corpus edge is not a failure.
  [[nodiscard]] std::vector<VegaPanelRow> finish();

private:
  struct Pending {
    VegaPanelRow row;
    std::optional<ResolvedStructure> strangle; // nullopt when entry resolve failed
    double pnl{0.0};                           // running rehedged hold PnL
    double pnl_1d{0.0};                        // captured after the first mark
    double prev_value{0.0};                    // Σ qty·P on the previous session
    double prev_delta{0.0};                    // Δ_net on the previous session
    double prev_spot{0.0};
    int marks_seen{0};
    bool failed{false};
  };

  VegaPanelConfig cfg_;
  std::string symbol_;
  std::deque<Pending> pending_; // FIFO; size ≤ horizon_sessions
  std::string last_key_;
  std::int64_t last_ts_{0};
  std::uint64_t skipped_{0};

  // trailing per-session history, entry day inclusive (parallel vectors)
  std::vector<std::int64_t> ts_hist_;
  std::vector<double> spot_hist_;
  std::vector<double> iv1y_hist_;
};

} // namespace atx::vol

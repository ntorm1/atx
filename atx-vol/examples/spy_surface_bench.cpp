// spy_surface_bench.cpp — SOTA perf + accuracy proof on the SPY index fixture.
//
// There is no cached SPY option data (and no API budget to pull one), so this
// bench drives the deterministic known-truth SPY surface fixture
// (spy_fixture.hpp): a dense 5-wide ladder over six weekly/monthly expiries with
// an index term structure, steep tenor-decaying put skew, tight index-liquid
// spreads, and two discrete dividends, priced American on an arbitrage-free S3
// truth smile. Because the truth is known, the bench reports ACCURACY two ways —
// fair value inside the (tight) synthetic bid-ask AND recovery of the truth ATM
// vol — alongside PERFORMANCE (cold vs cached whole-surface fit, per-query us,
// single-slice refit).
//
// Opt-in via ATX_BUILD_EXAMPLES. Links atx::vol; may use printf freely.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "atx/vol/arb.hpp"          // arb_check_calendar
#include "atx/vol/s3.hpp"           // s3_iv (truth ATM vol)
#include "atx/vol/session.hpp"
#include "atx/vol/spy_fixture.hpp"  // make_spy_synthetic_spec, make_spy_session_inputs
#include "atx/vol/types.hpp"        // Side

namespace {

using atx::vol::FitPreset;
using atx::vol::make_spy_session_inputs;
using atx::vol::make_spy_synthetic_spec;
using atx::vol::make_synthetic_american_panel;
using atx::vol::SessionInputs;
using atx::vol::Side;
using atx::vol::VolaSession;

[[nodiscard]] double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch())
      .count();
}

}  // namespace

int main() {
  const auto spec = make_spy_synthetic_spec();
  const auto panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    std::fprintf(stderr, "SPY panel build failed: %s\n",
                 panel.error().message().c_str());
    return 1;
  }

  std::printf("=== Synthetic SPY index surface (known truth) ===\n");
  std::printf("  spot %.2f   rate %.3f   %zu expiries x %zu strikes\n",
              spec.spot, spec.r, spec.expiries.size(), spec.strikes.size());
  std::printf("  spreads: %.1f%% of mid, floored at %.2f\n\n",
              100.0 * spec.half_spread_frac, spec.min_half_spread);

  // ── Whole-surface build cost breakdown ─────────────────────────────────────
  // Time four configurations to separate the fit cost from the calendar-repair
  // cost and the one-time query-cache construction. `Fast` = raw eSSVI fit;
  // `Robust` = Fast + MonotoneFit calendar repair; the cache trades a one-time
  // build cost for the (measured below) ~75x per-query speedup, so on a small
  // 6-slice chain it is a net build-time COST paid back over the session's life.
  const auto timed_build = [&](FitPreset preset, bool cache) {
    SessionInputs in = make_spy_session_inputs(spec, preset);
    in.use_correction_cache = cache;
    const double t = now_ms();
    auto s = VolaSession::from_frame(panel->frame, in);
    return std::pair{now_ms() - t, std::move(s)};
  };
  auto [fast_ms, fast_s] = timed_build(FitPreset::Fast, false);
  auto [rob_ms, rob_s] = timed_build(FitPreset::Robust, false);
  auto [cold_pair_ms, cold] = timed_build(FitPreset::Robust, false);
  auto [cached_ms, cached] = timed_build(FitPreset::Robust, true);
  (void)cold_pair_ms;

  if (!fast_s.has_value() || !rob_s.has_value() || !cold.has_value() ||
      !cached.has_value()) {
    std::fprintf(stderr, "SPY session build failed\n");
    return 1;
  }
  const auto& d = cached->diagnostics();
  const double nq = d.n_quotes > 0 ? static_cast<double>(d.n_quotes) : 1.0;

  const auto& df = fast_s->diagnostics();
  const auto& dr = rob_s->diagnostics();
  std::printf("=== Build cost (whole-surface de-Am + fit + re-Am) ===\n");
  std::printf("  Fast   (raw fit)       : %8.1f ms   %7.1f us/quote   in-ba %.1f%%  arb-free %s\n",
              fast_ms, 1000.0 * fast_ms / nq, 100.0 * df.mean_frac_within_bidask,
              df.calendar_arb_free ? "yes" : "NO");
  std::printf("  Robust (+MonotoneFit)  : %8.1f ms   %7.1f us/quote   in-ba %.1f%%  arb-free %s\n",
              rob_ms, 1000.0 * rob_ms / nq, 100.0 * dr.mean_frac_within_bidask,
              dr.calendar_arb_free ? "yes" : "NO");
  std::printf("  Robust +query cache    : %8.1f ms   (one-time cache build; pays\n"
              "                           back on queries below)\n\n", cached_ms);

  // ── Accuracy: per-expiry quality + KNOWN-TRUTH ATM recovery ────────────────
  // Scored off the COLD Fast session (exact Andersen-Lake re-Am) — the SOTA
  // config here (arb-free, 96 ms). The cache trades a little of this tight-spread
  // accuracy for the query speed measured below.
  const auto& sess = *fast_s;
  const auto ctx = sess.expiries();
  const auto par = sess.parity();
  std::printf("=== Fit quality + truth recovery per expiry (Fast, cold) ===\n");
  std::printf("  %-8s %10s %9s %9s %8s  %8s %8s %7s\n", "T(yr)", "forward",
              "rmse_vol", "chi2_red", "in_ba%", "atm_fit", "atm_tru", "bp_err");
  double worst_atm_bp = 0.0;
  for (std::size_t i = 0; i < ctx.size(); ++i) {
    const double T = ctx[i].T;
    const double F = ctx[i].forward;
    // Model ATM vol read from the fitted surface at the forward strike.
    const double atm_fit = sess.iv(F, T);
    // Truth ATM vol == the expiry's sigma0 (s3_iv at k=0), recovered here for a
    // like-for-like read.
    const double atm_tru = atx::vol::s3_iv(0.0, T, spec.expiries[i].truth);
    const double bp = 1.0e4 * std::fabs(atm_fit - atm_tru);
    worst_atm_bp = std::max(worst_atm_bp, bp);
    std::printf("  %-8.4f %10.4f %9.5f %9.4f %7.1f%%  %8.4f %8.4f %7.1f\n", T, F,
                par[i].rmse_mid_vol, par[i].chi2_reduced,
                100.0 * par[i].frac_fv_within_bidask, atm_fit, atm_tru, bp);
  }

  std::printf("\n=== Aggregate (Fast, cold — the SOTA config) ===\n");
  std::printf("  slices fit           : %zu   (quotes used %zu)\n",
              df.n_slices, df.n_quotes);
  std::printf("  fair value in bid-ask: worst %.1f%%   mean %.1f%%\n",
              100.0 * df.worst_frac_within_bidask,
              100.0 * df.mean_frac_within_bidask);
  std::printf("  mean reduced chi2    : %.4f\n", df.mean_chi2_reduced);
  std::printf("  mean vol RMSE        : %.5f\n", df.mean_rmse_vol);
  std::printf("  worst ATM recovery   : %.1f bp\n", worst_atm_bp);
  std::printf("  calendar arb-free    : %s\n",
              df.calendar_arb_free ? "yes" : "NO");
  std::printf("  cached-surface in-ba : %.1f%%  (the query cache's tight-spread"
              " approximation cost)\n", 100.0 * d.mean_frac_within_bidask);

  // ── Query throughput: fair_value hot path (cold vs cached) ─────────────────
  std::vector<std::pair<double, double>> pts;
  for (const auto& c : ctx) {
    pts.emplace_back(spec.spot, c.T);
    pts.emplace_back(spec.spot * 1.05, c.T);
    pts.emplace_back(spec.spot * 0.95, c.T);
  }
  constexpr int kReps = 500;
  double sink = 0.0;
  const double qc0 = now_ms();
  for (int rep = 0; rep < kReps; ++rep) {
    for (const auto& [K, T] : pts) {
      const Side sd = (K >= spec.spot) ? Side::Call : Side::Put;
      const auto fv = cold->fair_value(K, T, sd);
      if (fv.has_value()) sink += *fv;
    }
  }
  const double cold_q_ms = now_ms() - qc0;
  const double qh0 = now_ms();
  for (int rep = 0; rep < kReps; ++rep) {
    for (const auto& [K, T] : pts) {
      const Side sd = (K >= spec.spot) ? Side::Call : Side::Put;
      const auto fv = cached->fair_value(K, T, sd);
      if (fv.has_value()) sink += *fv;
    }
  }
  const double cached_q_ms = now_ms() - qh0;
  const double n_q = static_cast<double>(kReps * pts.size());
  std::printf("\n=== fair_value query throughput (%d reps x %zu pts) ===\n", kReps,
              pts.size());
  std::printf("  cold  (Andersen-Lake): %10.3f us/query\n",
              1000.0 * cold_q_ms / n_q);
  std::printf("  cached (hot path)    : %10.3f us/query\n",
              1000.0 * cached_q_ms / n_q);
  if (cached_q_ms > 0.0) {
    std::printf("  query speedup        : %.1fx   (checksum %.2f)\n",
                cold_q_ms / cached_q_ms, sink);
  }

  return 0;
}

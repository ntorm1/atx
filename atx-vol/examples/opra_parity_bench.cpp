// opra_parity_bench.cpp — real-data Vola-parity proof + throughput benchmark.
//
// Loads a REAL Databento OPRA cbbo-1m (NBBO) option-chain slice from a Parquet
// file (produced offline from the cached DBN by atx-core's opra_dbn_to_parquet —
// ZERO API spend), de-Americanizes + fits the whole surface via VolaSession, and
// reports both FIT QUALITY (per-expiry implied forward/borrow, RMSE, reduced
// chi-square, fraction of re-Americanized fair values inside the bid-ask) and
// THROUGHPUT (cold Andersen-Lake vs cached hot path, wall-clock + microseconds
// per quote).
//
// This is the "prove parity and high-quality fits on real data, and prove how
// fast" harness. It is an example (opt-in via ATX_BUILD_EXAMPLES) and links the
// atx libraries, so it may use printf / try-catch freely (examples are not held
// to the library no-exception rule).
//
// Usage: opra_parity_bench [PARQUET] [UNDERLYING] [SNAPSHOT_ISO] [RATE] [CARRY_MODE]
//   defaults: data/xom_opra_cbbo1m_2026-06-05T1955Z.parquet  XOM
//             2026-06-05T19:55:00Z  0.043  default
//
// CARRY_MODE selects whether the per-expiry CARRY CONFIDENCE GATE is armed:
//   `default` — `require_carry_confidence` stays false, so every expiry keeps its
//               OWN solved borrow whatever its confidence. Bit-identical to the
//               historical bench.
//   `risk`    — arms `require_carry_confidence`, the flag the served risk build
//               sets (pricer_fitter's risk policy). A non-confident expiry is then
//               DEFERRED to the board-level term-structure repair pass and serves a
//               borrow DERIVED from the confident expiries instead of its own. This
//               is the only mode in which the carry gate has any observable effect,
//               and therefore the only one in which a change to the gate can be
//               measured on the round trip.
// Deliberately arms ONLY that flag, not the rest of the risk policy (calendar
// Project, parity scoring, audited inversions): those move node admission too, and
// would confound a measurement whose subject is the carry gate alone.
// If the Parquet file is absent the bench prints a skip notice and exits 0
// (mirroring the C fixtures' skip-when-absent convention).

#include <chrono>
#include <cmath>    // exp, log
#include <cstdio>
#include <cstdlib>  // atof
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/fitting/arb.hpp"     // arb_check_calendar (crossing localization)
#include "atx/vol/api/fitting/calib.hpp"       // build_observations, CalibOpts, FitObs/FitDiag
#include "atx/vol/api/marketdata/data.hpp"        // data_install
#include "fitting/essvi_calib.hpp"  // essvi_fit_slice (warm vs cold refit)
#include "atx/vol/api/marketdata/opra_panel.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/core/types.hpp"  // Side
#include "atx/vol/api/marketdata/universe.hpp"     // Universe, Underlying (chains for refit)
#include "atx/vol/api/fitting/vol_surface.hpp"  // EssviParams

namespace {

using atx::vol::OpraLoadSpec;
using atx::vol::OpraPanel;
using atx::vol::SessionInputs;
using atx::vol::Side;
using atx::vol::VolaSession;

[[nodiscard]] double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch())
      .count();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path =
      argc > 1 ? argv[1] : "data/xom_opra_cbbo1m_2026-06-05T1955Z.parquet";
  const std::string underlying = argc > 2 ? argv[2] : "XOM";
  const std::string snapshot = argc > 3 ? argv[3] : "2026-06-05T19:55:00Z";
  const double rate = argc > 4 ? std::atof(argv[4]) : 0.043;
  const std::string carry_mode = argc > 5 ? argv[5] : "default";
  if (carry_mode != "default" && carry_mode != "risk") {
    std::fprintf(stderr, "unknown CARRY_MODE '%s' (expected 'default' or 'risk')\n",
                 carry_mode.c_str());
    return 2;
  }
  const bool require_carry_confidence = (carry_mode == "risk");

  if (!std::filesystem::exists(path)) {
    std::printf(
        "SKIP: %s not found.\n"
        "  Generate it (no API spend) from the cached DBN:\n"
        "    opra_dbn_to_parquet data/xom_opra_cbbo1m_2026-06-05T1955Z.dbn.zst %s "
        "2026-06-05\n",
        path.c_str(), path.c_str());
    return 0;
  }

  try {
    // ── Load the real NBBO chain slice ─────────────────────────────────────
    OpraLoadSpec spec;
    spec.path = path;
    spec.underlying = underlying;
    spec.snapshot_iso = snapshot;
    spec.r = rate;

    const double t0 = now_ms();
    auto panel_res = atx::vol::load_opra_cbbo_parquet(spec);
    const double load_ms = now_ms() - t0;
    if (!panel_res.has_value()) {
      std::fprintf(stderr, "load_opra_cbbo_parquet failed: %s\n",
                   panel_res.error().message().c_str());
      return 1;
    }
    const OpraPanel& panel = *panel_res;

    std::printf("=== Real OPRA cbbo-1m (NBBO) chain ===\n");
    std::printf("  file        : %s\n", path.c_str());
    std::printf("  underlying  : %s   snapshot %s\n", underlying.c_str(),
                panel.snapshot_iso.c_str());
    std::printf("  contracts   : %zu across %zu expiries (%zu rows dropped)\n",
                panel.n_contracts, panel.n_expiries, panel.n_dropped);
    std::printf("  implied spot: %.4f  (rate %.4f)   loaded in %.1f ms\n\n",
                panel.implied_spot, rate, load_ms);

    // Common session inputs (spot implied from the chain PCP forward).
    SessionInputs base;
    base.S = panel.implied_spot;
    base.r = rate;
    base.now_ts_ns = panel.frame.snapshot_ts_ns;
    base.deam.require_carry_confidence = require_carry_confidence;

    // ── Throughput: cold Andersen-Lake vs cached hot path ──────────────────
    SessionInputs cold_in = base;
    cold_in.use_correction_cache = false;
    const double c0 = now_ms();
    auto cold = VolaSession::from_frame(panel.frame, cold_in);
    const double cold_ms = now_ms() - c0;

    SessionInputs cached_in = base;
    cached_in.use_correction_cache = true;
    const double h0 = now_ms();
    auto cached = VolaSession::from_frame(panel.frame, cached_in);
    const double cached_ms = now_ms() - h0;

    if (!cold.has_value()) {
      std::fprintf(stderr, "cold session build failed: %s\n",
                   cold.error().message().c_str());
      return 1;
    }
    if (!cached.has_value()) {
      std::fprintf(stderr, "cached session build failed: %s\n",
                   cached.error().message().c_str());
      return 1;
    }

    const auto& d_cold = cold->diagnostics();
    const auto& d = cached->diagnostics();
    const double nq = d.n_quotes > 0 ? static_cast<double>(d.n_quotes) : 1.0;

    std::printf("=== Throughput (whole-surface de-Am + fit + re-Am) ===\n");
    std::printf("  cold  (Andersen-Lake): %8.1f ms   %7.1f us/quote\n", cold_ms,
                1000.0 * cold_ms / nq);
    std::printf("  cached (hot path)    : %8.1f ms   %7.1f us/quote\n", cached_ms,
                1000.0 * cached_ms / nq);
    if (cached_ms > 0.0) {
      std::printf("  speedup              : %.1fx\n\n", cold_ms / cached_ms);
    }

    // ── Fit quality (from the cached surface) ──────────────────────────────
    std::printf("=== Fit quality per expiry (cached surface) ===\n");
    // `rt_vol` is the ABSOLUTE round trip (T5 item 3): de-Am -> fit ->
    // re-Americanize -> compare to the ORIGINAL American mid, restated in vol
    // points. Read it next to `in_ba%`: that column is spread-normalised, so it
    // reads 100% on any board wide enough by construction, while this one does
    // not scale with the spread and is the number that says whether serving a
    // thin board is honest.
    std::printf("  %-8s %10s %10s %10s %10s %10s %8s %6s\n", "T(yr)", "forward",
                "borrow", "rmse_vol", "rt_vol", "chi2_red", "in_ba%", "n");
    const auto ctx = cached->expiries();
    const auto par = cached->parity();
    // Parallel to expiries(): carries the per-slice carry PROVENANCE, which is what
    // distinguishes a slice serving its own solved borrow from one serving a borrow
    // derived from the board term structure.
    const auto sdiag = cached->slice_diagnostics();
    std::size_t n_rt_expiries = 0;
    for (std::size_t i = 0; i < ctx.size(); ++i) {
      // An expiry whose every quote sits below the one-tick-per-vol-point vega
      // floor has NO vol-space verdict; print "n/a" rather than a 0.00000 that
      // would read as a perfect round trip.
      char rt[16];
      if (par[i].n_round_trip > 0) {
        std::snprintf(rt, sizeof(rt), "%10.5f", par[i].rmse_round_trip_vol);
        ++n_rt_expiries;
      } else {
        std::snprintf(rt, sizeof(rt), "%10s", "n/a");
      }
      std::printf("  %-8.4f %10.4f %10.5f %10.5f %s %10.4f %7.1f%% %6zu\n", ctx[i].T,
                  ctx[i].forward, ctx[i].borrow, par[i].rmse_mid_vol, rt, par[i].chi2_reduced,
                  100.0 * par[i].frac_fv_within_bidask, ctx[i].n_used);
      // Machine-readable mirror of the same row, joined to the carry provenance.
      // `src` is the CarrySource ordinal (0 Solved, 1 TermStructureInterp,
      // 2 TermStructureExtrap, 3 MoneynessBounded); `rt` is -1 where the expiry
      // carries no vol-space verdict at all.
      const bool has_carry = i < sdiag.size();
      std::printf("RTROW sym=%s mode=%s T=%.6f borrow=%.8f src=%d conf=%d avail=%d "
                  "rt=%.6f nrt=%zu rmse=%.6f n=%zu\n",
                  underlying.c_str(), carry_mode.c_str(), ctx[i].T, ctx[i].borrow,
                  has_carry ? static_cast<int>(sdiag[i].carry.source) : -1,
                  (has_carry && sdiag[i].carry.confident) ? 1 : 0,
                  (has_carry && sdiag[i].carry.available) ? 1 : 0,
                  par[i].n_round_trip > 0 ? par[i].rmse_round_trip_vol : -1.0,
                  par[i].n_round_trip, par[i].rmse_mid_vol, ctx[i].n_used);
    }

    std::printf("\n=== Aggregate parity (cached) ===\n");
    std::printf("  slices fit           : %zu   (quotes used %zu)\n", d.n_slices,
                d.n_quotes);
    std::printf("  fair value in bid-ask: worst %.1f%%   mean %.1f%%\n",
                100.0 * d.worst_frac_within_bidask,
                100.0 * d.mean_frac_within_bidask);
    std::printf("  mean reduced chi2    : %.4f\n", d.mean_chi2_reduced);
    std::printf("  mean vol RMSE        : %.5f\n", d.mean_rmse_vol);
    std::printf("  round-trip vol (abs) : mean %.5f   worst quote %.5f   (%zu of %zu expiries "
                "carry a vol-space verdict)\n",
                d.mean_round_trip_vol, d.max_round_trip_vol, n_rt_expiries, ctx.size());
    std::printf("  calendar arb-free    : %s\n",
                d.calendar_arb_free ? "yes" : "NO");

    // Cross-check: cold vs cached agreement (self-consistency of the hot path).
    std::printf("\n=== Cold vs cached agreement ===\n");
    std::printf("  mean in-bid-ask cold %.1f%%  vs cached %.1f%%\n",
                100.0 * d_cold.mean_frac_within_bidask,
                100.0 * d.mean_frac_within_bidask);
    std::printf("  mean chi2       cold %.4f  vs cached %.4f\n",
                d_cold.mean_chi2_reduced, d.mean_chi2_reduced);

    // ── Calendar-arb repair (Vola's headline: an arb-free surface) ─────────
    // The raw independent-per-slice eSSVI fit + wing extrapolation can cross in
    // total variance (calendar arbitrage). Two strategies, opposite ends of the
    // quality-vs-strictness trade-off:
    //   MonotoneFit — sequential theta-floor DURING the fit (ATM-monotone,
    //                 quality-preserving; deep-wing crossings may remain);
    //   Project     — post-hoc backbone projection to strict |k|<=3 arb-free
    //                 (quality cost when crossings sit in the data-free wings).
    // Report each: does it clear the check, and at what fit-quality cost.
    std::printf("\n=== Calendar-arb repair (raw -> repaired) ===\n");
    std::printf("  raw: calendar arb-free %s   (%zu violations of %zu-slice surface)\n",
                d.calendar_arb_free ? "yes" : "NO", d.n_calendar_viol_pre,
                d.n_slices);

    // Localize the crossings: count calendar violations over shrinking k-windows
    // on the RAW surface. If they vanish inside the data-supported / near-money
    // region and only appear in the deep wings, the surface is calendar-arb-free
    // WHERE IT IS TRADED and the wide |k|<=3 check is flagging pure extrapolation.
    std::printf("  crossing localization (raw surface, violations by |k| window):\n");
    for (const double kw : {0.3, 0.5, 1.0, 2.0, 3.0}) {
      const auto v = atx::vol::arb_check_calendar(cached->surface(), -kw, kw, 25);
      std::printf("    |k| <= %.1f : %zu\n", kw,
                  v.has_value() ? v->size() : static_cast<std::size_t>(0));
    }
    const auto report_repair = [&](const char* name,
                                   atx::vol::CalendarRepair mode) {
      SessionInputs rep_in = cached_in;
      rep_in.calendar_repair = mode;
      auto repaired = VolaSession::from_frame(panel.frame, rep_in);
      if (!repaired.has_value()) {
        std::fprintf(stderr, "  %-11s build failed: %s\n", name,
                     repaired.error().message().c_str());
        return;
      }
      const auto& dr = repaired->diagnostics();
      // Calendar violations remaining on the repaired surface, core (near-money)
      // window vs. the full |k|<=3 grid.
      const auto vc = atx::vol::arb_check_calendar(repaired->surface(), -0.6, 0.6, 25);
      const auto vf = atx::vol::arb_check_calendar(repaired->surface(), -3.0, 3.0, 25);
      std::printf(
          "  %-11s viol core|k|<=0.6 %2zu  full|k|<=3 %2zu | in-ba mean %.1f%%->%.1f%% "
          "worst %.1f%%->%.1f%% | chi2 %.3f->%.3f | RMSE %.4f->%.4f\n",
          name, vc.has_value() ? vc->size() : 0, vf.has_value() ? vf->size() : 0,
          100.0 * d.mean_frac_within_bidask, 100.0 * dr.mean_frac_within_bidask,
          100.0 * d.worst_frac_within_bidask,
          100.0 * dr.worst_frac_within_bidask, d.mean_chi2_reduced,
          dr.mean_chi2_reduced, d.mean_rmse_vol, dr.mean_rmse_vol);
    };
    report_repair("MonotoneFit", atx::vol::CalendarRepair::MonotoneFit);
    report_repair("Project", atx::vol::CalendarRepair::Project);

    // ── Query throughput: fair_value HOT PATH (post-build, cold vs cached) ──
    // The composable session's real value is answering fair_value/greeks at an
    // arbitrary (K, T) with NO refit. Time a sweep of ATM / ±5% points across
    // every fitted expiry; this isolates the per-query pricer cost (no one-time
    // cache build in the loop), which is where the cached hot path dominates.
    std::vector<std::pair<double, double>> pts;  // (K, T)
    for (const auto& c : ctx) {
      pts.emplace_back(panel.implied_spot, c.T);
      pts.emplace_back(panel.implied_spot * 1.05, c.T);
      pts.emplace_back(panel.implied_spot * 0.95, c.T);
    }
    constexpr int kReps = 500;
    double sink = 0.0;  // defeat dead-code elimination

    const double qc0 = now_ms();
    for (int rep = 0; rep < kReps; ++rep) {
      for (const auto& [K, T] : pts) {
        const Side sd = (K >= panel.implied_spot) ? Side::Call : Side::Put;
        const auto fv = cold->fair_value(K, T, sd);
        if (fv.has_value()) sink += *fv;
      }
    }
    const double cold_q_ms = now_ms() - qc0;

    const double qh0 = now_ms();
    for (int rep = 0; rep < kReps; ++rep) {
      for (const auto& [K, T] : pts) {
        const Side sd = (K >= panel.implied_spot) ? Side::Call : Side::Put;
        const auto fv = cached->fair_value(K, T, sd);
        if (fv.has_value()) sink += *fv;
      }
    }
    const double cached_q_ms = now_ms() - qh0;

    const double n_q = static_cast<double>(kReps * pts.size());
    std::printf("\n=== fair_value query throughput (%d reps x %zu points) ===\n",
                kReps, pts.size());
    std::printf("  cold  (Andersen-Lake): %10.3f us/query\n",
                1000.0 * cold_q_ms / n_q);
    std::printf("  cached (hot path)    : %10.3f us/query\n",
                1000.0 * cached_q_ms / n_q);
    if (cached_q_ms > 0.0) {
      std::printf("  query speedup        : %.1fx   (checksum %.3f)\n",
                  cold_q_ms / cached_q_ms, sink);
    }

    // ── Strike-ladder reprice latency (the HFT book path) ──────────────────
    // Reprice a whole expiry's strike ladder in ONE call (fair_value_ladder)
    // vs. a per-option fair_value loop over the same strikes. The ladder path
    // resolves the per-expiry context (T-bracket forward/carry, cache pointers)
    // once and reuses it across every strike; report ns/option for both.
    const double ladder_T = ctx.empty() ? 0.25 : ctx.back().T;
    std::vector<double> lstrikes;
    std::vector<Side> lsides;
    for (double m = 0.70; m <= 1.30 + 1e-9; m += 0.015) {  // ~40 strikes
      const double K = panel.implied_spot * m;
      lstrikes.push_back(K);
      lsides.push_back(K >= panel.implied_spot ? Side::Call : Side::Put);
    }
    std::vector<double> lout(lstrikes.size(), 0.0);
    constexpr int kLReps = 2000;

    const double ll0 = now_ms();
    for (int rep = 0; rep < kLReps; ++rep) {
      (void)cached->fair_value_ladder(ladder_T, lstrikes, lsides, lout);
      sink += lout.front();
    }
    const double ladder_ms = now_ms() - ll0;

    const double lp0 = now_ms();
    for (int rep = 0; rep < kLReps; ++rep) {
      for (std::size_t i = 0; i < lstrikes.size(); ++i) {
        const auto fv = cached->fair_value(lstrikes[i], ladder_T, lsides[i]);
        if (fv.has_value()) sink += *fv;
      }
    }
    const double loop_ms = now_ms() - lp0;

    const double n_opt = static_cast<double>(kLReps) *
                         static_cast<double>(lstrikes.size());
    std::printf("\n=== strike-ladder reprice latency (%d reps x %zu strikes @ T=%.3f) ===\n",
                kLReps, lstrikes.size(), ladder_T);
    std::printf("  ladder (1 call/expiry): %8.1f ns/option\n",
                1.0e6 * ladder_ms / n_opt);
    std::printf("  per-option loop       : %8.1f ns/option\n",
                1.0e6 * loop_ms / n_opt);
    if (ladder_ms > 0.0) {
      std::printf("  ladder speedup        : %.2fx   (checksum %.3f)\n",
                  loop_ms / ladder_ms, sink);
    }

    // ── Incremental warm-start refit latency (the tick-to-quote path) ──────
    // A market maker does not rebuild the whole surface when one chain reprints
    // — it refits THAT expiry's eSSVI slice from the fresh quotes. Measure the
    // per-slice refit COLD (neutral cube seed) vs WARM-STARTED (the whole cube
    // seeded from the pre-tick slice): iterations and wall-clock, on a real
    // middle expiry. Same landing quality; warm is the incremental hot path.
    atx::vol::Universe u_refit;
    if (const auto uid = atx::vol::data_install(u_refit, panel.frame);
        uid.has_value() && !ctx.empty()) {
      const auto under = u_refit.get_underlying(*uid);
      if (under.has_value() && !(*under)->chains.empty()) {
        const std::size_t idx = (*under)->chains.size() / 2;  // a middle expiry
        const auto& chain = (*under)->chains[idx];
        const double T = ctx[idx].T;
        const double F = ctx[idx].forward;
        const double df = std::exp(-rate * T);
        atx::vol::CalibOpts opts;  // default policy (== the session's in_.calib)
        const auto obs = atx::vol::build_observations(chain, F, T, df, opts);
        const auto slices = cached->surface().essvi_slices();
        if (obs.has_value() && obs->obs.size() >= 5 && idx < slices.size()) {
          const atx::vol::EssviParams prior = slices[idx];
          const std::span<const atx::vol::FitObs> os{obs->obs};

          atx::vol::FitDiag dc{};
          atx::vol::FitDiag dw{};
          (void)atx::vol::essvi_fit_slice(os, T, F, opts, &dc);            // cold
          (void)atx::vol::essvi_fit_slice(os, T, F, opts, &dw, 0.0, &prior);  // warm

          constexpr int kRR = 4000;
          const double rc0 = now_ms();
          for (int rep = 0; rep < kRR; ++rep) {
            const auto r = atx::vol::essvi_fit_slice(os, T, F, opts, nullptr);
            if (r.has_value()) sink += r->theta;
          }
          const double cold_refit_ms = now_ms() - rc0;

          const double rw0 = now_ms();
          for (int rep = 0; rep < kRR; ++rep) {
            const auto r =
                atx::vol::essvi_fit_slice(os, T, F, opts, nullptr, 0.0, &prior);
            if (r.has_value()) sink += r->theta;
          }
          const double warm_refit_ms = now_ms() - rw0;

          const double warm_us = 1000.0 * warm_refit_ms / kRR;
          std::printf(
              "\n=== incremental slice refit (tick-to-quote) @ T=%.3f, %zu obs ===\n",
              T, obs->obs.size());
          std::printf("  cold  (neutral seed) : %8.1f us/refit   inner %u  outer %u\n",
                      1000.0 * cold_refit_ms / kRR, dc.inner_iters_total,
                      dc.outer_iters);
          std::printf("  warm  (prior seed)   : %8.1f us/refit   inner %u  outer %u\n",
                      warm_us, dw.inner_iters_total, dw.outer_iters);
          if (warm_refit_ms > 0.0) {
            std::printf("  warm/cold seed       : %.2fx  (iteration cut shows on"
                        " far-from-neutral / thin slices; this liquid slice is\n"
                        "                         already near the cold ATM seed,"
                        " so warm's win here is small — its real value is\n"
                        "                         prior-anchored stability across"
                        " ticks, see prior_strength)\n",
                        cold_refit_ms / warm_refit_ms);
          }
          // The headline tick-to-quote win is not warm-vs-cold; it is touching
          // ONE expiry instead of rebuilding the whole surface. Compare the
          // single-slice refit to the full cached whole-surface build.
          if (warm_us > 0.0) {
            std::printf("  vs full surface build: %.0fx cheaper than the %zu-slice"
                        " whole-surface rebuild (%.1f ms) — a single-expiry\n"
                        "                         re-quote reprices its own slice,"
                        " not the %zu unchanged expiries\n",
                        (cached_ms * 1000.0) / warm_us, d.n_slices, cached_ms,
                        d.n_slices > 0 ? d.n_slices - 1 : 0);
          }
          sink += static_cast<double>(dc.inner_iters_total);
        }
      }
    }

    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "opra_parity_bench error: %s\n", e.what());
    return 1;
  }
}

// Real OPRA cold-fit breadth gate: ten SPY slices spanning date, time-of-day,
// and stress regime must each preserve at least 98% clean price-in-NBBO.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/american.hpp"
#include "atx/vol/calib.hpp"
#include "atx/vol/chain.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/universe.hpp"
#include "support/cached_artifacts.hpp"
#include "support/spy_fit_fixture.hpp"

namespace {

using namespace atx::vol;
using atx::vol::test::cached_hft_fit;
using atx::vol::testkit::kSpyFitFixtures;
using atx::vol::testkit::load_spy_fit_fixture;
using atx::vol::testkit::price_in_band;

// True when ATX_VOL_SCOREBOARDS is set non-empty and not "0" — the nightly
// full-sweep preset (Task 7). Read with _dupenv_s under MSVC/clang-cl: plain
// std::getenv trips /WX (-Wdeprecated-declarations); same pattern as
// support/bench_gate.hpp.
[[nodiscard]] bool scoreboards_enabled() noexcept {
#if defined(_MSC_VER)
  char *e = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&e, &n, "ATX_VOL_SCOREBOARDS") != 0 || e == nullptr) {
    return false;
  }
  const bool on = e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
  std::free(e);
  return on;
#else
  const char *e = std::getenv("ATX_VOL_SCOREBOARDS");
  return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
#endif
}

TEST(SpyFitCorpus, HftColdStartPreserves98PctOnEveryAvailableSlice) {
  std::size_t loaded = 0;
  for (const auto &fixture : kSpyFitFixtures) {
    auto board = load_spy_fit_fixture(fixture);
    if (!board.has_value()) {
      continue;
    }
    ++loaded;
    SCOPED_TRACE(std::string(fixture.id) + " (" + fixture.snapshot_iso + ")");

    auto chain = OptionChain::from_frame(board->panel.frame, board->env());
    ASSERT_TRUE(chain.has_value()) << chain.error().to_string();

    PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft}};
    const auto t0 = std::chrono::steady_clock::now();
    const Status fit = fitter.fit(*chain);
    const auto t1 = std::chrono::steady_clock::now();
    ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const auto score =
        price_in_band(fitter.surface()->session(), chain->underlying(), board->spot(), board->r);
    std::printf("[SPY fit corpus] %-12s %-24s fit=%7.2fms pxCLN=%6.2f%% "
                "(%zu/%zu) legs=%zu\n",
                fixture.id, fixture.regime, ms, score.px_clean, score.n_clean_in, score.n_clean,
                chain->size());
    EXPECT_GT(score.n_clean, 100u);
    EXPECT_GE(score.px_clean, 98.0);
  }

  if (loaded == 0) {
    GTEST_SKIP() << "SPY fit corpus not found under data/spy_fit_slices";
  }
  EXPECT_EQ(loaded, kSpyFitFixtures.size())
      << "partial SPY corpus: materialize all ten fixtures before benchmarking";
}

// Task 11 (P2.5) real-corpus accuracy gate: on each fitted SPY board, price every
// (expiry, side) smile ladder through the σ-boundary interpolant and confirm it
// stays within $0.001/share of the cold per-strike Andersen-Lake price. Reports
// the ColdFallback rate. Uses the fitted model IV per strike (surface.iv) as the
// per-strike σ — the exact fitted-smile board the interpolant is designed for.
//
// Reloads each fixture's Hft-preset fit from the cached archive (see
// cached_artifacts.hpp) instead of re-fitting live — HftColdStart... above
// already fits these SAME ten boards live in the same suite run, and
// spy_archive_roundtrip_test proves the reload reproduces the live session's
// iv()/context() bit-for-bit. The cold per-strike Andersen-Lake parity sweep
// below IS the assertion under test and stays fully live.
TEST(SigmaInterpCorpus, RealBoard_WithinGates) {
  std::size_t loaded = 0, n_slices = 0, n_priced = 0, n_fallback = 0;
  double max_gap = 0.0;
  // Every parity ladder is still built, interpolant-priced, and asserted; the
  // default subsamples the cold-AL reference comparison to every 2nd strike
  // (deterministic: indices 0, 2, 4, ...) for wall-time. The full-strike sweep
  // (a strict superset, same tolerances) runs under ATX_VOL_SCOREBOARDS=1
  // (nightly preset, Task 7).
  const std::size_t stride = scoreboards_enabled() ? 1 : 2;
  for (const auto &fixture : kSpyFitFixtures) {
    auto board = load_spy_fit_fixture(fixture);
    const auto archive_path = cached_hft_fit(fixture);
    if (!board.has_value() || archive_path.empty()) {
      continue;
    }
    auto arch = SurfaceArchiveV2::open_file(archive_path.string());
    ASSERT_TRUE(arch.has_value()) << arch.error().to_string();
    auto recon = arch->reconstruct_symbol("SPY");
    ASSERT_TRUE(recon.has_value()) << recon.error().to_string();
    ++loaded;
    const double S = board->spot();
    const double r = board->r;
    const Underlying &U = board->underlying();
    const CalibOpts copts;

    for (const auto &c : recon->context()) {
      const double T = c.T;
      if (T < 0.019) {
        continue;
      }
      const double F = c.forward;
      const double q_eff = c.q_eff;
      const double df = std::exp(-r * T);
      const Chain *chn = nullptr;
      for (const Chain &ch : U.chains) {
        if (std::fabs(ch.T - T) < 1e-9) {
          chn = &ch;
          break;
        }
      }
      if (chn == nullptr) {
        continue;
      }
      const auto obs = build_observations(*chn, F, T, df, copts);
      if (!obs.has_value() || obs->obs.size() < 12) {
        continue;
      }
      for (Side side : {Side::Put, Side::Call}) {
        std::vector<double> strikes, sigmas;
        for (const FitObs &o : obs->obs) {
          if (o.side != side) {
            continue;
          }
          const double miv = recon->iv(o.K, T);
          if (!std::isfinite(miv) || miv <= 0.0) {
            continue;
          }
          strikes.push_back(o.K);
          sigmas.push_back(miv);
        }
        if (strikes.size() < 12) {  // need more strikes than σ-nodes to build
          continue;
        }
        std::vector<double> px(strikes.size(), 0.0);
        SigmaInterpOptions so;
        so.use_sigma_boundary_interp = true;
        so.n_sigma = 8;
        SigmaSliceStats st;
        const auto rc =
            (side == Side::Put)
                ? andersen_lake_put_slice_sigma(S, strikes, sigmas, T, r, q_eff,
                                                std::span<double>(px), so, std::nullopt, &st)
                : andersen_lake_call_slice_sigma(S, strikes, sigmas, T, r, q_eff,
                                                 std::span<double>(px), so, std::nullopt, &st);
        if (!rc.has_value()) {
          continue;  // non-American corner for this (side, q_eff); skip
        }
        ++n_slices;
        n_fallback += st.n_cold_fallback;
        for (std::size_t i = 0; i < strikes.size(); i += stride) {
          const auto cold =
              andersen_lake(S, strikes[i], T, sigmas[i], r, q_eff, side, std::nullopt);
          if (!cold.has_value()) {
            continue;
          }
          ++n_priced;
          const double gap = std::fabs(px[i] - *cold);
          max_gap = std::max(max_gap, gap);
          EXPECT_LT(gap, 1.0e-3)
              << fixture.id << " " << (side == Side::Put ? "put" : "call") << " T=" << T
              << " K=" << strikes[i] << " sig=" << sigmas[i] << " interp=" << px[i]
              << " cold=" << *cold;
        }
      }
    }
  }
  if (loaded == 0) {
    GTEST_SKIP() << "SPY fit corpus not found under data/spy_fit_slices";
  }
  const double fb_rate =
      n_priced > 0 ? 100.0 * static_cast<double>(n_fallback) / static_cast<double>(n_priced) : 0.0;
  std::printf("[sigma-interp corpus] boards=%zu slices=%zu priced=%zu fallback=%zu (%.2f%%) "
              "max price gap vs cold=%.3e\n",
              loaded, n_slices, n_priced, n_fallback, fb_rate, max_gap);
  EXPECT_GT(n_priced, 0u);
}

// Task 11 (P2.5) board-throughput measurement (Release; run with
// --gtest_also_run_disabled_tests). Times cold per-strike vs σ-interpolant
// pricing of every (expiry, side) smile ladder across the loaded SPY corpus and
// reports the speedup — the ship-gate metric (>= 2.5x enables the flag).
TEST(SigmaInterpCorpus, DISABLED_BoardThroughput) {
  struct Ladder {
    double S, T, r, q;
    Side side;
    std::vector<double> K, sig, px;
  };
  std::vector<Ladder> ladders;
  std::size_t loaded = 0;
  for (const auto &fixture : kSpyFitFixtures) {
    auto board = load_spy_fit_fixture(fixture);
    if (!board.has_value()) {
      continue;
    }
    ++loaded;
    auto chain = OptionChain::from_frame(board->panel.frame, board->env());
    ASSERT_TRUE(chain.has_value());
    PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft}};
    ASSERT_TRUE(fitter.fit(*chain).has_value());
    const VolaSession &sess = fitter.surface()->session();
    const double S = board->spot();
    const double r = board->r;
    const Underlying &U = board->underlying();
    const CalibOpts copts;
    for (const auto &c : sess.expiries()) {
      const double T = c.T;
      if (T < 0.019) {
        continue;
      }
      const double df = std::exp(-r * T);
      const Chain *chn = nullptr;
      for (const Chain &ch : U.chains) {
        if (std::fabs(ch.T - T) < 1e-9) { chn = &ch; break; }
      }
      if (chn == nullptr) {
        continue;
      }
      const auto obs = build_observations(*chn, c.forward, T, df, copts);
      if (!obs.has_value()) {
        continue;
      }
      for (Side side : {Side::Put, Side::Call}) {
        Ladder L;
        L.S = S; L.T = T; L.r = r; L.q = c.q_eff; L.side = side;
        for (const FitObs &o : obs->obs) {
          if (o.side != side) continue;
          const double miv = sess.iv(o.K, T);
          if (!std::isfinite(miv) || miv <= 0.0) continue;
          L.K.push_back(o.K);
          L.sig.push_back(miv);
        }
        if (L.K.size() < 12) continue;
        // Only keep ladders the American arm can price (skip European/Unsupported).
        std::vector<double> probe(L.K.size(), 0.0);
        SigmaInterpOptions off;
        off.use_sigma_boundary_interp = false;
        const auto rc = (side == Side::Put)
            ? andersen_lake_put_slice_sigma(L.S, L.K, L.sig, L.T, L.r, L.q, std::span<double>(probe), off)
            : andersen_lake_call_slice_sigma(L.S, L.K, L.sig, L.T, L.r, L.q, std::span<double>(probe), off);
        if (!rc.has_value()) continue;
        L.px.assign(L.K.size(), 0.0);
        ladders.push_back(std::move(L));
      }
    }
  }
  if (loaded == 0 || ladders.empty()) {
    GTEST_SKIP() << "SPY fit corpus not found";
  }
  std::size_t total_strikes = 0;
  for (const auto &L : ladders) total_strikes += L.K.size();

  SigmaInterpOptions off, on;
  off.use_sigma_boundary_interp = false;  // explicit cold reference (default is now ON)
  on.use_sigma_boundary_interp = true;
  on.n_sigma = 8;
  auto run = [&](const SigmaInterpOptions &so) {
    double sink = 0.0;
    for (auto &L : ladders) {
      const auto rc = (L.side == Side::Put)
          ? andersen_lake_put_slice_sigma(L.S, L.K, L.sig, L.T, L.r, L.q, std::span<double>(L.px), so)
          : andersen_lake_call_slice_sigma(L.S, L.K, L.sig, L.T, L.r, L.q, std::span<double>(L.px), so);
      (void)rc;
      sink += L.px[0];
    }
    return sink;
  };
  run(off);  // warm caches
  run(on);
  const int R = 20;
  const auto c0 = std::chrono::steady_clock::now();
  double s1 = 0.0;
  for (int i = 0; i < R; ++i) s1 += run(off);
  const auto c1 = std::chrono::steady_clock::now();
  double s2 = 0.0;
  for (int i = 0; i < R; ++i) s2 += run(on);
  const auto c2 = std::chrono::steady_clock::now();
  const double cold_ms = std::chrono::duration<double, std::milli>(c1 - c0).count() / R;
  const double interp_ms = std::chrono::duration<double, std::milli>(c2 - c1).count() / R;
  std::printf("[sigma-interp throughput] ladders=%zu strikes=%zu | cold=%.3f ms  interp=%.3f ms  "
              "speedup=%.2fx  (sink %.3g/%.3g)\n",
              ladders.size(), total_strikes, cold_ms, interp_ms, cold_ms / interp_ms, s1, s2);
}

// ── G1 real-data 0DTE validation (SPY 2026-07-17 intraday session sweep) ─────

// The four real SPY slices on a genuine SPY expiration day each ingest the
// same-session (0DTE) 2026-07-17 expiry the old midnight-UTC parse hard-dropped,
// stamp it at the TRUE 16:00 ET (20:00Z) PM settle, carry a small POSITIVE
// intraday T that shrinks monotonically through the session, and fit end-to-end
// to a sane surface. Skips cleanly when the git-ignored payloads are absent
// (source-only CI). See data/fixtures/pg-sota-gdata-manifest.md (Fixture 1).
TEST(SpyFitCorpus, G1ZeroDteSessionSweepIngestsFitsAndTMonotone) {
  struct Slice {
    const char *file;
    const char *snapshot;  // load-bearing: the loader computes every T from it
    double manifest_T;     // 0DTE year-fraction to 16:00 ET, per the manifest
  };
  const std::array<Slice, 4> slices{{
      {"SPY_2026-07-17T1335Z.parquet", "2026-07-17T13:35:00Z", 0.000732},  // 09:35 ET
      {"SPY_2026-07-17T1600Z.parquet", "2026-07-17T16:00:00Z", 0.000457},  // 12:00 ET
      {"SPY_2026-07-17T1800Z.parquet", "2026-07-17T18:00:00Z", 0.000228},  // 14:00 ET
      {"SPY_2026-07-17T1955Z.parquet", "2026-07-17T19:55:00Z", 0.000010},  // 15:55 ET
  }};
  const std::int64_t settle = expiry_instant_ns("2026-07-17", SettlementSession::Pm);
  ASSERT_EQ(settle, iso_to_ns("2026-07-17T20:00:00Z"));  // 16:00 EDT == 20:00Z

  std::size_t loaded = 0;
  double prev_T = std::numeric_limits<double>::infinity();
  for (const auto &s : slices) {
    const testkit::SpyFitFixture fx{"0dte", s.file, s.snapshot, "0dte"};
    auto board = testkit::load_spy_fit_fixture(fx);
    if (!board.has_value()) {
      continue;
    }
    ++loaded;
    SCOPED_TRACE(std::string(s.file));

    // (1) The 0DTE expiry survives ingest, stamped at the TRUE 16:00 ET instant,
    //     with a small positive intraday T matching the manifest value.
    const Underlying &U = board->underlying();
    const Chain *zdte = nullptr;
    for (const Chain &ch : U.chains) {
      if (ch.expiry_ns == settle) {
        zdte = &ch;
        break;
      }
    }
    ASSERT_NE(zdte, nullptr) << "0DTE (2026-07-17) expiry not ingested";
    const auto expected_T = time_to_expiry_years(iso_to_ns(s.snapshot), settle, TimeSpec{});
    ASSERT_TRUE(expected_T.has_value()) << expected_T.error().to_string();
    EXPECT_DOUBLE_EQ(zdte->T, *expected_T);
    EXPECT_GT(zdte->T, 0.0);
    EXPECT_NEAR(zdte->T, s.manifest_T, 5.0e-6) << "0DTE T vs manifest";
    EXPECT_GT(zdte->strikes.size(), std::size_t{50}) << "front chain should be liquid";

    // (2) The board fits end-to-end with the 0DTE front expiry included.
    auto chain = OptionChain::from_frame(board->panel.frame, board->env());
    ASSERT_TRUE(chain.has_value()) << chain.error().to_string();
    PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft}};
    const Status fit = fitter.fit(*chain);
    EXPECT_TRUE(fit.has_value()) << fit.error().to_string();

    // (3) Sane surface at T ~ hours: a finite, positive, plausible ATM IV at the
    //     0DTE tenor when the fitted session serves it.
    double atm_iv = std::numeric_limits<double>::quiet_NaN();
    if (fit.has_value()) {
      const VolaSession &sess = fitter.surface()->session();
      atm_iv = sess.iv(board->spot(), zdte->T);
      if (std::isfinite(atm_iv)) {
        EXPECT_GT(atm_iv, 0.01);
        EXPECT_LT(atm_iv, 5.0);
      }
    }
    std::printf("[G1 0DTE] %-30s T=%.6f (%.2fh) strikes=%zu fit=%s atmIV=%.4f\n", s.file, zdte->T,
                zdte->T * 365.25 * 24.0, zdte->strikes.size(), fit.has_value() ? "ok" : "FAIL",
                atm_iv);

    // (4) T strictly shrinks toward the settle as the session advances.
    EXPECT_LT(zdte->T, prev_T);
    prev_T = zdte->T;
  }
  if (loaded == 0) {
    GTEST_SKIP() << "SPY 0DTE session slices not found under data/spy_fit_slices";
  }
  EXPECT_LT(prev_T, 5.0e-5) << "final (15:55 ET) slice should leave only minutes to settle";
}

} // namespace

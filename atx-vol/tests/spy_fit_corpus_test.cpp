// Real OPRA cold-fit breadth gate: ten SPY slices spanning date, time-of-day,
// and stress regime must each preserve at least 98% clean price-in-NBBO.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/american.hpp"
#include "atx/vol/calib.hpp"
#include "atx/vol/chain.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/universe.hpp"
#include "support/spy_fit_fixture.hpp"

namespace {

using namespace atx::vol;
using atx::vol::testkit::kSpyFitFixtures;
using atx::vol::testkit::load_spy_fit_fixture;
using atx::vol::testkit::price_in_band;

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
// the ColdFallback rate. Uses the fitted model IV per strike (sess.iv) as the
// per-strike σ — the exact fitted-smile board the interpolant is designed for.
TEST(SigmaInterpCorpus, RealBoard_WithinGates) {
  std::size_t loaded = 0, n_slices = 0, n_priced = 0, n_fallback = 0;
  double max_gap = 0.0;
  for (const auto &fixture : kSpyFitFixtures) {
    auto board = load_spy_fit_fixture(fixture);
    if (!board.has_value()) {
      continue;
    }
    ++loaded;
    auto chain = OptionChain::from_frame(board->panel.frame, board->env());
    ASSERT_TRUE(chain.has_value()) << chain.error().to_string();
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
          const double miv = sess.iv(o.K, T);
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
        for (std::size_t i = 0; i < strikes.size(); ++i) {
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

} // namespace

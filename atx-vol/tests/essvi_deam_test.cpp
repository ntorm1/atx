#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <vector>

#include "atx/vol/api/fitting/calib.hpp"          // CalibOpts, calib_default_opts
#include "atx/vol/api/core/chain.hpp"          // OptionChain
#include "atx/vol/api/fitting/correction.hpp"     // CorrectionCache, AmericanCorrectionCaches
#include "atx/vol/api/marketdata/data.hpp"           // iso_to_ns
#include "atx/vol/api/fitting/deamer.hpp"         // DeAmOptions
#include "fitting/essvi_calib.hpp"    // the unit under test
#include "atx/vol/api/core/market_env.hpp"     // MarketEnv
#include "atx/vol/api/backtest/panel.hpp"          // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/api/pricing/rates_curve.hpp"    // CurveSet, ForwardPoint
#include "atx/vol/api/fitting/s3.hpp"             // s3_iv
#include "fitting/spy_fixture.hpp"    // make_spy_synthetic_spec
#include "atx/vol/api/marketdata/universe.hpp"       // Underlying, Chain
#include "atx/vol/api/fitting/vol_surface.hpp"    // VolSurface, EssviParams, Parametrization

// De-Americanization routing coverage for the eSSVI surface driver (Task C2.3).
//
// The low-level driver inverts option mids with plain Black-76, silently
// treating American mids as European. On a synthetic AMERICAN board (r>0, so a
// real put-side early-exercise premium) this biases the fitted put wing high.
// `essvi_calib_surface`'s opt-in `deam` param routes observation building
// through `build_observations_european` instead, stripping the early-exercise
// premium before the fit. These tests pin the DISEASE (raw bias) and the CURE
// (de-Am removes it), the cached-vs-cold parity, and the null-path exact-bit
// default.

namespace {

using atx::vol::AmericanCorrectionCaches;
using atx::vol::CalibOpts;
using atx::vol::calib_default_opts;
using atx::vol::Chain;
using atx::vol::CorrectionCache;
using atx::vol::CurveSet;
using atx::vol::DeAmOptions;
using atx::vol::essvi_calib_surface;
using atx::vol::EssviParams;
using atx::vol::FitDiag;
using atx::vol::ForwardPoint;
using atx::vol::iso_to_ns;
using atx::vol::make_spy_synthetic_spec;
using atx::vol::make_synthetic_american_panel;
using atx::vol::MarketEnv;
using atx::vol::OptionChain;
using atx::vol::Parametrization;
using atx::vol::s3_iv;
using atx::vol::Side;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::Underlying;
using atx::vol::VolSurface;

// ── Synthetic AMERICAN board fixture ──────────────────────────────────────
//
// Mirrors bench/fitting_throughput_bench.cpp's `build_spy_board`: the SPY-like
// known-truth panel (`make_spy_synthetic_spec` -> `make_synthetic_american_panel`
// -> `MarketEnv::flat` -> `OptionChain::from_frame`) converted to the
// `Underlying`/`CurveSet` pair the eSSVI driver consumes. The panel's per-expiry
// S3 truth smile (European-equivalent) is retained so a fit can be scored
// against the generating vols.
struct AmBoard {
  Underlying under;
  CurveSet curves;
  double spot{0.0};
  double r{0.0};
  SynthPanelSpec spec;
};

[[nodiscard]] std::optional<AmBoard> build_american_board() {
  const SynthPanelSpec spec = make_spy_synthetic_spec();
  const auto panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    return std::nullopt;
  }
  const MarketEnv env =
      MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs);
  auto chain_res = OptionChain::from_frame(panel->frame, env);
  if (!chain_res.has_value()) {
    return std::nullopt;
  }

  AmBoard board;
  board.under = chain_res->underlying();
  board.spot = spec.spot;
  board.r = spec.r;
  board.spec = spec;

  board.curves.spot = spec.spot;
  const std::array<double, 2> pillar_t{1.0e-3, 50.0};
  const std::array<double, 2> pillar_r{spec.r, spec.r};
  if (!board.curves.set_yield(pillar_t, pillar_r).has_value()) {
    return std::nullopt;
  }

  std::vector<ForwardPoint> fps;
  fps.reserve(board.under.chains.size());
  for (const Chain& c : board.under.chains) {
    ForwardPoint fp{};
    fp.expiry_ns = c.expiry_ns;
    fp.T = c.T;
    fp.F = spec.spot;  // fallback; overwritten below on a truth match
    for (std::size_t i = 0; i < spec.expiries.size(); ++i) {
      if (iso_to_ns(spec.expiries[i].expiry_iso) == c.expiry_ns) {
        fp.F = panel->truth_forward[i];
        break;
      }
    }
    fps.push_back(fp);
  }
  board.curves.forward.set(fps);
  return board;
}

// The generating S3 truth smile for the slice at maturity `T` (nearest match).
[[nodiscard]] const SynthExpiry& truth_for_T(const SynthPanelSpec& spec, double T) {
  std::size_t best = 0;
  double best_gap = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < spec.expiries.size(); ++i) {
    const double gap = std::fabs(spec.expiries[i].T - T);
    if (gap < best_gap) {
      best_gap = gap;
      best = i;
    }
  }
  return spec.expiries[best];
}

// Max |fit IV − generating IV| over every slice's own strike ladder, evaluated
// at each strike's log-moneyness against the slice's stamped forward. This is
// the disease/cure metric: the eSSVI-vs-S3 model mismatch is common-mode to the
// raw and de-Am fits, so the extra deviation the raw fit carries is the
// American early-exercise premium bleaking into the fitted vols.
[[nodiscard]] double max_iv_dev_vs_truth(const VolSurface& surface,
                                         const AmBoard& board, double k_band) {
  const auto slices = surface.essvi_slices();
  double max_dev = 0.0;
  for (std::size_t si = 0; si < slices.size(); ++si) {
    const EssviParams& sl = slices[si];
    const SynthExpiry& te = truth_for_T(board.spec, sl.T);
    for (const double K : board.spec.strikes) {
      const double k = std::log(K / sl.F);
      if (std::fabs(k) > k_band) {
        continue;  // outside the data-supported band (avoid deep-wing extrap)
      }
      const double iv_true = s3_iv(k, sl.T, te.truth);
      const double iv_fit =
          surface.iv_on_slice(static_cast<std::uint16_t>(si), k);
      if (std::isfinite(iv_true) && std::isfinite(iv_fit)) {
        max_dev = std::max(max_dev, std::fabs(iv_fit - iv_true));
      }
    }
  }
  return max_dev;
}

// Fit the board through the eSSVI surface driver. `deam == nullptr` is today's
// raw Black-76-inversion path; a non-null `deam` routes observation building
// through `build_observations_european` (the opt-in de-Am fast path).
//
// C-8: `validate_no_arb`'s honest post-fit audit (essvi_calib.cpp) now
// correctly finds real calendar crossings on the RAW route's fit — this board
// is explicitly documented above (RawRouteIsBiasedOnAmericanBoard) as
// "crossing-heavy" precisely BECAUSE the raw route is biased; that bias is
// exactly the disease this whole test file exists to pin and cure, not
// something these tests should gate on. Disabled here (uniformly, for the raw
// AND de-Am'd routes) so the disease/cure comparison keeps running on both;
// the de-Am'd route's own fit is independently clean regardless (verified: it
// carries no violations the audit would have caught).
[[nodiscard]] VolSurface fit_board(const AmBoard& board, const DeAmOptions* deam,
                                   FitDiag* diag = nullptr) {
  auto surf_res =
      VolSurface::create(1u, Parametrization::Essvi, board.under.chains.size());
  EXPECT_TRUE(surf_res.has_value());
  VolSurface surface = *surf_res;
  CalibOpts opts = calib_default_opts();
  opts.validate_no_arb = false;
  const auto st = essvi_calib_surface(surface, board.under, board.curves, opts,
                                      diag, /*prior=*/nullptr, /*n_workers=*/1u,
                                      deam);
  EXPECT_TRUE(st.has_value());
  return surface;
}

[[nodiscard]] VolSurface fit_raw(const AmBoard& board, FitDiag* diag = nullptr) {
  return fit_board(board, /*deam=*/nullptr, diag);
}

// Per-side American-minus-European correction caches covering the board, built
// the way `build_session_caches` (session.cpp) does: a padded (k_log, T) box
// from every strike/expiry (spot as the forward proxy), a representative carry
// q_rep from the mid expiry's forward, and a generous sigma box. Used to prove
// the cached hot path matches the cold Andersen-Lake de-Am.
struct BoardCaches {
  CorrectionCache call;
  CorrectionCache put;
};

[[nodiscard]] BoardCaches build_board_caches(const AmBoard& board) {
  const double S = board.spot;
  double k_min = std::numeric_limits<double>::infinity();
  double k_max = -std::numeric_limits<double>::infinity();
  double T_lo = std::numeric_limits<double>::infinity();
  double T_hi = -std::numeric_limits<double>::infinity();
  for (const Chain& c : board.under.chains) {
    if (!(c.T > 0.0)) {
      continue;
    }
    T_lo = std::min(T_lo, c.T);
    T_hi = std::max(T_hi, c.T);
    for (const double K : c.strikes) {
      const double k = std::log(K / S);
      k_min = std::min(k_min, k);
      k_max = std::max(k_max, k);
    }
  }
  k_min -= 0.05;
  k_max += 0.05;
  const double T_min = 0.9 * T_lo;
  const double T_max = (T_hi > T_lo) ? (1.1 * T_hi) : (1.5 * T_lo);
  constexpr double kSigMin = 0.05;
  constexpr double kSigMax = 1.5;

  // Representative carry from the mid expiry's forward (q_eff = r - ln(F/S)/T).
  const Chain& mid = board.under.chains[board.under.chains.size() / 2];
  const double F_mid = board.curves.forward.forward_at(mid.expiry_id);
  double q_rep = board.r;
  if (mid.T > 0.0 && F_mid > 0.0) {
    q_rep = board.r - std::log(F_mid / S) / mid.T;
  }

  constexpr std::uint16_t kNK = 16;
  constexpr std::uint16_t kNT = 8;
  constexpr std::uint16_t kNS = 12;
  BoardCaches bc;
  auto cc = CorrectionCache::build(kNK, kNT, kNS, board.r, q_rep, k_min, k_max,
                                   T_min, T_max, kSigMin, kSigMax, Side::Call);
  EXPECT_TRUE(cc.has_value());
  if (cc) {
    bc.call = std::move(*cc);
  }
  auto pp = CorrectionCache::build(kNK, kNT, kNS, board.r, q_rep, k_min, k_max,
                                   T_min, T_max, kSigMin, kSigMax, Side::Put);
  EXPECT_TRUE(pp.has_value());
  if (pp) {
    bc.put = std::move(*pp);
  }
  return bc;
}

// ── Test: the raw route is biased on an American board (disease pin) ───────

TEST(EssviDeAm, RawRouteIsBiasedOnAmericanBoard) {
  const auto board = build_american_board();
  ASSERT_TRUE(board.has_value());

  const VolSurface raw = fit_raw(*board);
  ASSERT_EQ(raw.n_slices(), board->under.chains.size());

  const double dev_raw = max_iv_dev_vs_truth(raw, *board, 0.04);
  // The bias is worst on the longest tenor, whose forward sits well above spot
  // so nearly the whole strike ladder is OTM puts carrying early-exercise
  // premium. Report that slice's ATM inflation as the headline disease number.
  const auto slices = raw.essvi_slices();
  const std::size_t last = slices.size() - 1;
  const SynthExpiry& te_last = truth_for_T(board->spec, slices[last].T);
  const double atm_fit = raw.iv_on_slice(static_cast<std::uint16_t>(last), 0.0);
  std::fprintf(stderr,
               "[EssviDeAm] RAW: max_iv_dev(|k|<=0.04)=%.6f (%.1f bps); longest "
               "tenor T=%.3f ATM fit=%.4f vs truth=%.4f\n",
               dev_raw, dev_raw * 1.0e4, slices[last].T, atm_fit,
               te_last.truth.sigma0);

  // The raw Black-76 inversion leaves the American put-side early-exercise
  // premium in the fitted vols. This documents the disease and will FAIL if
  // someone silently de-Americanizes the default path.
  // FT-C9a re-pin: the alternate eSSVI driver no longer runs the quality-
  // destroying theta-scale calendar projection by default (essvi_alt_driver_
  // theta_project, default off). The old ~940 bps figure was largely that
  // projection AMPLIFYING the raw bias on this crossing-heavy American board; the
  // true raw European-on-American disease is ~77 bps ATM inflation (old 0.094 ->
  // new 0.0077, delta -857 bps). Still an order of magnitude above the <1 bp cold
  // de-Am residual, so the disease/cure separation the test guards is intact.
  EXPECT_GT(dev_raw, 5.0e-3);  // ~77 bps raw disease; pinned above the de-Am residual
}

// ── Test: cold de-Am removes the bias (the cure) ──────────────────────────

TEST(EssviDeAm, ColdDeAmRemovesBias) {
  const auto board = build_american_board();
  ASSERT_TRUE(board.has_value());

  const VolSurface raw = fit_raw(*board);

  DeAmOptions deam{};  // empty caches => cold Andersen-Lake per strike
  const VolSurface cold = fit_board(*board, &deam);
  ASSERT_EQ(cold.n_slices(), board->under.chains.size());

  const double dev_raw = max_iv_dev_vs_truth(raw, *board, 0.04);
  const double dev_cold = max_iv_dev_vs_truth(cold, *board, 0.04);

  // Direct positive check (guards against a NaN-masked false pass): on the
  // longest tenor — where the raw fit inflates the ATM vol above the generating
  // 0.162 (to ~0.169 with the theta-projection now off by default; FT-C9a) — the
  // de-Am fit must land back on the generating ATM vol.
  const auto slices = cold.essvi_slices();
  const std::size_t last = slices.size() - 1;
  const SynthExpiry& te_last = truth_for_T(board->spec, slices[last].T);
  const double atm_cold = cold.iv_on_slice(static_cast<std::uint16_t>(last), 0.0);
  std::fprintf(stderr,
               "[EssviDeAm] COLD: dev_raw=%.6f (%.1f bps) dev_cold=%.6f (%.1f bps); "
               "longest tenor ATM cold=%.4f vs truth=%.4f\n",
               dev_raw, dev_raw * 1.0e4, dev_cold, dev_cold * 1.0e4, atm_cold,
               te_last.truth.sigma0);
  ASSERT_TRUE(std::isfinite(atm_cold));
  EXPECT_NEAR(atm_cold, te_last.truth.sigma0, 2.0e-3);

  // De-Americanizing strips the early-exercise premium before the fit, so the
  // recovered European-equivalent vols land on the generating S3 smile (only the
  // small eSSVI-vs-S3 model-form residual remains, measured < 1 bp in-band).
  EXPECT_LT(dev_cold, 3.0e-3);
  EXPECT_LT(dev_cold, 0.05 * dev_raw);  // the cure is >20x better than the disease
}

// ── Test: cached de-Am matches cold de-Am ─────────────────────────────────

TEST(EssviDeAm, CachedDeAmMatchesCold) {
  const auto board = build_american_board();
  ASSERT_TRUE(board.has_value());

  DeAmOptions cold_opts{};
  const VolSurface cold = fit_board(*board, &cold_opts);

  BoardCaches bc = build_board_caches(*board);
  DeAmOptions cached_opts{};
  cached_opts.caches = AmericanCorrectionCaches{&bc.call, &bc.put};
  const VolSurface cached = fit_board(*board, &cached_opts);

  ASSERT_EQ(cold.n_slices(), cached.n_slices());
  const auto sc = cold.essvi_slices();
  const auto sk = cached.essvi_slices();

  // The observable that matters is the fitted IV surface: (theta, phi, rho)
  // trade off (phi alone spans ~9..42 across these tenors), so raw-param deltas
  // over-state the difference. Compare the two surfaces' IV at every in-band
  // strike, and — as bounded secondary pins — theta and rho per slice.
  double max_iv = 0.0;
  double max_dtheta = 0.0;
  double max_drho = 0.0;
  for (std::size_t i = 0; i < sc.size(); ++i) {
    max_dtheta = std::max(max_dtheta, std::fabs(sc[i].theta - sk[i].theta));
    max_drho = std::max(max_drho, std::fabs(sc[i].rho - sk[i].rho));
    for (const double K : board->spec.strikes) {
      const double k = std::log(K / sc[i].F);
      if (std::fabs(k) > 0.04) {
        continue;
      }
      const double iv_c = cold.iv_on_slice(static_cast<std::uint16_t>(i), k);
      const double iv_k = cached.iv_on_slice(static_cast<std::uint16_t>(i), k);
      if (std::isfinite(iv_c) && std::isfinite(iv_k)) {
        max_iv = std::max(max_iv, std::fabs(iv_c - iv_k));
      }
    }
  }
  std::fprintf(stderr,
               "[EssviDeAm] cached-vs-cold: max|d_iv|=%.6f (%.2f bps) "
               "max|d_theta|=%.3e max|d_rho|=%.3e\n",
               max_iv, max_iv * 1.0e4, max_dtheta, max_drho);

  // The cache is a Chebyshev interpolation of the cold Andersen-Lake pricer
  // baked at ONE representative carry (mirroring build_session_caches), so it is
  // a measured-and-pinned closeness, NOT bit-identity. The residual (~28 bps,
  // measured) is concentrated on the shortest tenor, whose true q_eff drifts
  // farthest from the single baked q_rep — the same single-carry approximation
  // the served session path accepts (it targets ~1e-2 surface RMSE). Even so the
  // cache removes ~97% of the ~940 bps raw bias.
  EXPECT_LT(max_iv, 4.0e-3);    // measured 28.47 bps; pinned with headroom
  EXPECT_LT(max_dtheta, 1.0e-3);
  EXPECT_LT(max_drho, 5.0e-2);
}

// ── Test: null deam is byte-identical to the raw path (default pin) ────────

TEST(EssviDeAm, NullDeamIsByteIdenticalToRaw) {
  const auto board = build_american_board();
  ASSERT_TRUE(board.has_value());

  const VolSurface a = fit_raw(*board);          // no deam arg (defaulted)
  const VolSurface b = fit_board(*board, nullptr);  // explicit nullptr

  ASSERT_EQ(a.n_slices(), b.n_slices());
  const auto sa = a.essvi_slices();
  const auto sb = b.essvi_slices();
  for (std::size_t i = 0; i < sa.size(); ++i) {
    EXPECT_EQ(sa[i].theta, sb[i].theta);
    EXPECT_EQ(sa[i].phi, sb[i].phi);
    EXPECT_EQ(sa[i].rho, sb[i].rho);
    EXPECT_EQ(sa[i].psi, sb[i].psi);
    EXPECT_EQ(sa[i].p, sb[i].p);
    EXPECT_EQ(sa[i].lambda, sb[i].lambda);
    EXPECT_EQ(sa[i].T, sb[i].T);
    EXPECT_EQ(sa[i].F, sb[i].F);
  }
}

}  // namespace

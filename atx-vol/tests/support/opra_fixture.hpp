#pragma once

// Shared OPRA-board test/bench fixture — kills the ~25-line load→install→session
// boilerplate that was copy-pasted across spy_real_test, spy_bidask_bench,
// spy_carry_diag, spy_dec_curve, spy_oos_check, chain_pricer_bench, and
// opra_parity_bench. One `load_opra_board(symbol)` call returns an installed
// board (panel + universe + resolved underlying + MarketEnv), and
// `price_in_band` is the exact pxCLN / pxALL metric spy_bidask_bench scores, so a
// regression test asserts the SAME 99.5% number the bench reports.
//
// Header-only, public-API-only. `load_opra_board` returns std::nullopt when the
// parquet fixture is absent so a CI run without data can GTEST_SKIP cleanly.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "test_paths.hpp"  // market_data — the one fixture-path resolver

#include "atx/vol/api/pricing/american.hpp"     // american_price, AmericanMethod, al_fast_opts
#include "atx/vol/api/fitting/calib.hpp"        // FitObs, build_observations, CalibOpts
#include "atx/vol/api/marketdata/data.hpp"      // data_install
#include "atx/vol/api/core/market_env.hpp"      // MarketEnv
#include "atx/vol/api/marketdata/opra_panel.hpp"  // OpraLoadSpec, load_opra_cbbo_parquet
#include "atx/vol/api/fitting/session.hpp"      // VolaSession
#include "atx/vol/api/core/types.hpp"           // Side
#include "atx/vol/api/marketdata/universe.hpp"  // Universe, Underlying, Chain

namespace atx::vol::testkit {

// Resolve one symbol's cbbo-1m parquet under the repo-root data/ tree. `symbol`
// is lowercase (matching the on-disk filename, e.g. "spy" / "xom"). Empty result
// => not found (caller should GTEST_SKIP).
[[nodiscard]] inline std::string find_opra_parquet(const std::string &symbol) {
  return market_data_if_present(symbol + "_opra_cbbo1m_2026-06-05T1955Z.parquet").string();
}

// An installed OPRA board: everything the fitter needs, resolved once.
struct OpraBoard {
  OpraPanel panel;
  Universe u;
  Uid uid{kInvalidUid};
  double r{0.043};

  [[nodiscard]] const Underlying &underlying() const { return *u.get_underlying(uid).value(); }
  [[nodiscard]] double spot() const { return panel.implied_spot; }
  [[nodiscard]] std::int64_t now_ns() const { return panel.frame.snapshot_ts_ns; }
  // The market environment (flat rate + the frame's dividend schedule).
  [[nodiscard]] MarketEnv env() const {
    return MarketEnv::flat(panel.implied_spot, r, panel.frame.snapshot_ts_ns, panel.frame.divs);
  }
};

// Load + install one symbol's OPRA board. `symbol` lowercase (filename);
// `underlying_uc` uppercase (the parquet's `underlying` column / frame uid).
// std::nullopt when the fixture is absent OR the load/install fails.
[[nodiscard]] inline std::optional<OpraBoard>
load_opra_board(const std::string &symbol, const std::string &underlying_uc, double r = 0.043);

// Load + install one explicitly named OPRA snapshot. This is the general form
// used by the multi-date/intraday SPY fit corpus; the legacy symbol-based helper
// below remains the convenient one-fixture facade used by older tests.
[[nodiscard]] inline std::optional<OpraBoard> load_opra_board_path(const std::string &path,
                                                                   const std::string &underlying_uc,
                                                                   const std::string &snapshot_iso,
                                                                   double r = 0.043) {
  if (path.empty()) {
    return std::nullopt;
  }
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = underlying_uc;
  spec.snapshot_iso = snapshot_iso;
  spec.r = r;
  auto panel = load_opra_cbbo_parquet(spec);
  if (!panel.has_value()) {
    return std::nullopt;
  }
  OpraBoard b;
  b.panel = std::move(*panel);
  b.r = r;
  auto uid = data_install(b.u, b.panel.frame);
  if (!uid.has_value()) {
    return std::nullopt;
  }
  b.uid = *uid;
  return b;
}

[[nodiscard]] inline std::optional<OpraBoard>
load_opra_board(const std::string &symbol, const std::string &underlying_uc, double r) {
  const std::string path = find_opra_parquet(symbol);
  return load_opra_board_path(path, underlying_uc, "2026-06-05T19:55:00Z", r);
}

// ── Price-in-band scorer (the spy_bidask_bench pxCLN / pxALL metric) ─────────

struct PxBandScore {
  // Headline metric: model IV re-Americanized COLD (the spy_bidask_bench pxCLN
  // definition — this is the "99.5%" number).
  double px_all{0.0};   // % of liquid quotes with cold model price in band
  double px_clean{0.0}; // % over the locally-convex (fittable) subset
  // What the library SERVES via the cached hot-path pricer (fair_value). Reported
  // for visibility; the cached correction is baked at one representative carry, so
  // it is less penny-accurate than the cold re-Am on carry-distant expiries.
  double px_clean_served{0.0};
  std::size_t n_all{0};
  std::size_t n_all_in{0};
  std::size_t n_clean{0};
  std::size_t n_clean_in{0};
  std::size_t n_clean_served_in{0};
};

// Per-quote local butterfly-convexity flag (identical to spy_bidask_bench's
// flag_fittable): an interior same-side quote whose 3-point non-uniform butterfly
// is negative is arb-inconsistent (un-fittable); endpoints are fittable.
inline void flag_fittable(const std::vector<FitObs> &obs, std::vector<char> &fit) {
  fit.assign(obs.size(), 1);
  for (int s = 0; s < 2; ++s) {
    const Side want = static_cast<Side>(static_cast<std::uint8_t>(s));
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < obs.size(); ++i) {
      if (obs[i].side == want) {
        idx.push_back(i);
      }
    }
    std::sort(idx.begin(), idx.end(),
              [&](std::size_t a, std::size_t b) { return obs[a].K < obs[b].K; });
    for (std::size_t j = 1; j + 1 < idx.size(); ++j) {
      const double K0 = obs[idx[j - 1]].K, K1 = obs[idx[j]].K, K2 = obs[idx[j + 1]].K;
      const double P0 = obs[idx[j - 1]].mid, P1 = obs[idx[j]].mid, P2 = obs[idx[j + 1]].mid;
      const double bf = P0 * (K2 - K1) - P1 * (K2 - K0) + P2 * (K1 - K0);
      if (bf < 0.0) {
        fit[idx[j]] = 0;
      }
    }
  }
}

// Score the fraction of the liquid board (T >= 1wk) whose SERVED model price
// (`sess.fair_value`, i.e. the exact price the library produces) lands inside the
// raw NBBO band, over the whole board and the locally-convex (clean) subset. This
// is the pxCLN headline the convex-QP dense fit hits 99.5% on — now scored
// through the session, whatever curve it fit.
//
// Templated on `Surface` so this scorer works identically off a live
// `VolaSession` (`expiries()`/`iv()`/`fair_value()`) or a reloaded
// `PricedSurface` (`context()`/`iv()`/`fair_value()` — the same semantics per
// priced_surface.hpp; `to_priced_surface()`/the archive round-trip proves the
// two agree bit-for-bit). `expiries()` vs `context()` is the only name that
// differs between the two types, so it is the only thing branched on here;
// existing `VolaSession` call sites are unaffected (template argument
// deduction instantiates the identical `Surface=VolaSession` specialization).
template <typename Surface>
[[nodiscard]] inline PxBandScore price_in_band(const Surface &sess, const Underlying &U,
                                               double S, double r, const CalibOpts &opts = {}) {
  PxBandScore bs;
  std::vector<char> fit;
  const auto expiries = [&sess]() -> std::span<const SliceContext> {
    if constexpr (requires { sess.expiries(); }) {
      return sess.expiries();
    } else {
      return sess.context();
    }
  }();
  for (const auto &c : expiries) {
    const double T = c.T;
    if (T < 0.019) {
      continue;
    }
    const double F = c.forward;
    const double q_eff = c.q_eff;
    const double df = std::exp(-r * T);
    const Chain *chain = nullptr;
    for (const Chain &ch : U.chains) {
      if (std::fabs(ch.T - T) < 1e-9) {
        chain = &ch;
        break;
      }
    }
    if (chain == nullptr) {
      continue;
    }
    const auto obs = build_observations(*chain, F, T, df, opts);
    if (!obs.has_value() || obs->obs.size() < 5) {
      continue;
    }
    flag_fittable(obs->obs, fit);
    for (std::size_t j = 0; j < obs->obs.size(); ++j) {
      const FitObs &o = obs->obs[j];
      const double half = 0.5 * o.spread;
      const double bid = o.mid - half, ask = o.mid + half;
      if (!(bid > 0.0) || !(ask > bid)) {
        continue;
      }
      const double miv = sess.iv(o.K, T);
      if (!std::isfinite(miv)) {
        continue;
      }
      // Headline: COLD re-Americanization of the served model IV (== the bench
      // pxCLN metric the 99.5% number is defined on).
      const auto fv_cold = american_price(S, o.K, T, miv, r, q_eff, o.side,
                                          AmericanMethod::AndersenLake, al_fast_opts());
      if (!fv_cold.has_value()) {
        continue;
      }
      const bool in_cold = (*fv_cold >= bid && *fv_cold <= ask);
      ++bs.n_all;
      if (in_cold) {
        ++bs.n_all_in;
      }
      // What the library serves via the cached hot path (fair_value).
      const auto fv_served = sess.fair_value(o.K, T, o.side);
      const bool in_served = fv_served.has_value() && (*fv_served >= bid && *fv_served <= ask);
      if (fit[j] != 0) {
        ++bs.n_clean;
        if (in_cold) {
          ++bs.n_clean_in;
        }
        if (in_served) {
          ++bs.n_clean_served_in;
        }
      }
    }
  }
  auto pct = [](std::size_t a, std::size_t b) {
    return b > 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0;
  };
  bs.px_all = pct(bs.n_all_in, bs.n_all);
  bs.px_clean = pct(bs.n_clean_in, bs.n_clean);
  bs.px_clean_served = pct(bs.n_clean_served_in, bs.n_clean);
  return bs;
}

} // namespace atx::vol::testkit

// spy_bidask_bench.cpp — the SOTA fit-quality acceptance harness on real SPY.
//
// The commercial question a market-making desk buys: what FRACTION of the
// tradeable board does the surface reproduce INSIDE the NBBO? The research
// (Vola Dynamics, Chebyshev-Hamiltonian arXiv 2512.01967, Ulrich/Zimmer) says
// to report TWO metrics, because at penny spreads they diverge sharply:
//
//   1. PRICE-in-band  : model American fair value in [bid, ask]. The literal
//      "% within bid-ask" (eSSVI-calib arXiv 2304.02106 headline F1). Brutal at
//      ATM (penny band ~= a fraction of a vol point), trivially easy in the deep
//      low-vega wings — so a raw count is gameable.
//   2. VEGA-WEIGHTED IV-in-band : model IV in [IV(bid), IV(ask)], vega^2
//      weighted. The defensible headline: it is the "fitted vol +- bid-ask error
//      bar" / minimum-edge metric Vola actually reports. SOTA calm-regime target
//      ~97-99%.
//
// This harness scores a whole SessionInputs config over the liquid board (one
// preferred OTM leg per strike, the exact build_observations survivor set),
// re-Americanizing every model vol on the fit's own carry. It sweeps a set of
// configs so we can see which fit lever actually moves the metric.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/calib.hpp"
#include "atx/vol/dense_slice.hpp"
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;

namespace {

double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch())
      .count();
}

// Cross-config band cache (deferred item): the [IV(bid), IV(ask)] band and
// IV(mid) depend only on (side, K, T, q_eff) — spot, rate, and the inversion
// pricer are config-independent, and the market bid/ask/mid are fixed per
// (K, T, side). The five scored configs share just two distinct carries (the
// three Fast-preset configs one q_eff, the two Accurate configs another), so a
// cache keyed on (side, K, T, q_eff) collapses the 5x redundant American-IV band
// inversions to 2x. Persisted across run() calls.
struct BandKey {
  double K{};
  double T{};
  double q{};
  std::uint8_t side{};
  bool operator==(const BandKey& o) const noexcept {
    return K == o.K && T == o.T && q == o.q && side == o.side;
  }
};
struct BandKeyHash {
  std::size_t operator()(const BandKey& k) const noexcept {
    const std::hash<double> hd;
    std::size_t s = hd(k.K);
    auto mix = [&](std::size_t v) {
      s ^= v + 0x9e3779b97f4a7c15ULL + (s << 6) + (s >> 2);
    };
    mix(hd(k.T));
    mix(hd(k.q));
    mix(std::hash<std::uint8_t>{}(k.side));
    return s;
  }
};
struct BandVal {
  double ivb{};
  double iva{};
  double ivm{};
  bool ok{false};
};
using BandCache = std::unordered_map<BandKey, BandVal, BandKeyHash>;

struct BoardScore {
  double px_all{};       // % model fair value in [bid,ask], whole liquid board
  double px_clean{};     // ... over the locally-convex (fittable) subset
  double vw1_all{};      // vega²-wtd % model IV in [IV(bid),IV(ask)] (band_k=1), all
  double vw1_clean{};    // ... clean subset (the fair SOTA-comparable metric)
  double vw2_clean{};    // clean, band widened x2 (Vola-style error bars)
  double vw3_clean{};    // clean, band widened x3
  double mean_dvol{};    // signed mean (model IV - market American IV), vol pts
  double build_ms{};
  double fit_ms{};       // total per-slice convex-fit time (convex path only)
  std::size_t n_fit{};   // slices fit (convex path)
  std::size_t n_all{};
  std::size_t n_clean{};
};

// Per-quote local butterfly convexity flag within a slice: option price is convex
// in strike, so a same-side interior quote whose 3-point non-uniform butterfly is
// negative is arb-inconsistent (un-fittable). Endpoints are marked fittable (a
// violation cannot be proven). Fills `fittable` (‖ obs) with the flags.
void flag_fittable(const std::vector<FitObs>& obs, std::vector<char>& fittable) {
  fittable.assign(obs.size(), 1);
  for (int s = 0; s < 2; ++s) {
    const Side want = static_cast<Side>(static_cast<std::uint8_t>(s));
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < obs.size(); ++i) {
      if (obs[i].side == want) idx.push_back(i);
    }
    std::sort(idx.begin(), idx.end(),
              [&](std::size_t a, std::size_t b) { return obs[a].K < obs[b].K; });
    for (std::size_t j = 1; j + 1 < idx.size(); ++j) {
      const double K0 = obs[idx[j - 1]].K, K1 = obs[idx[j]].K, K2 = obs[idx[j + 1]].K;
      const double P0 = obs[idx[j - 1]].mid, P1 = obs[idx[j]].mid, P2 = obs[idx[j + 1]].mid;
      const double bf = P0 * (K2 - K1) - P1 * (K2 - K0) + P2 * (K1 - K0);
      if (bf < 0.0) fittable[idx[j]] = 0;
    }
  }
}

// Score one built session over the liquid board (T >= 1wk). When `convex` is
// non-null, the per-slice model IV comes from the arbitrage-constrained dense
// convex-QP fit (Phase 1) instead of the session's eSSVI surface — everything
// else (population, re-Americanization, metrics) is identical, an apples-to-apples
// A/B of the representation.
BoardScore score_board(const QuoteFrame& frame, const Underlying& U,
                       double r, const SessionInputs& in,
                       BandCache& band_cache, std::size_t& n_band_calc,
                       std::size_t& n_band_hit,
                       const ConvexFitOpts* convex = nullptr) {
  BoardScore bs;
  const double t0 = now_ms();
  auto sess = VolaSession::from_frame(frame, in);
  bs.build_ms = now_ms() - t0;
  if (!sess.has_value()) {
    return bs;
  }
  const auto ctx = sess->expiries();
  const CalibOpts opts{};

  std::size_t px_all_n = 0, px_all_in = 0, px_cl_n = 0, px_cl_in = 0;
  double w_all = 0.0, w_all_in = 0.0;
  double w_cl = 0.0, w_cl_in1 = 0.0, w_cl_in2 = 0.0, w_cl_in3 = 0.0;
  double dvol_sum = 0.0;
  std::size_t dvol_n = 0;
  std::vector<char> fittable;

  for (std::size_t i = 0; i < ctx.size(); ++i) {
    const double T = ctx[i].T;
    if (T < 0.019) continue;  // ultra-short (<1wk) regime excluded
    const double q_eff = ctx[i].q_eff;
    const double F = ctx[i].forward;
    const double df = std::exp(-r * T);
    const Chain* chain = nullptr;
    for (const Chain& c : U.chains) {
      if (std::fabs(c.T - T) < 1e-9) { chain = &c; break; }
    }
    if (chain == nullptr) continue;
    const auto obs = build_observations(*chain, F, T, df, opts);
    if (!obs.has_value() || obs->obs.size() < 5) continue;
    flag_fittable(obs->obs, fittable);

    // Optional per-slice arb-constrained dense convex fit (Phase 1). The fit is
    // built on DE-AMERICANIZED (European-equivalent) observations: the convex
    // fold uses European put-call parity and the model IV is re-Americanized when
    // scored, so raw American mids would leave the put early-exercise premium in
    // and bias the near-money put wing. Scoring below still uses the raw-American
    // `obs` band.
    std::optional<ConvexSliceFit> cfit;
    if (convex != nullptr) {
      const auto obs_eu =
          build_observations_european(*chain, in.S, r, F, T, df, opts);
      if (!obs_eu.has_value() || obs_eu->obs.size() < 5) continue;
      const double tf = now_ms();
      auto cf = fit_convex_slice(obs_eu->obs, F, T, df, *convex);
      bs.fit_ms += now_ms() - tf;
      ++bs.n_fit;
      if (cf.has_value()) cfit = std::move(*cf);
      else continue;
    }

    for (std::size_t j = 0; j < obs->obs.size(); ++j) {
      const FitObs& o = obs->obs[j];
      const bool clean = fittable[j] != 0;
      const double half = 0.5 * o.spread;
      const double bid = o.mid - half;
      const double ask = o.mid + half;
      if (!(bid > 0.0) || !(ask > bid)) continue;
      const double miv = cfit ? cfit->iv(o.k) : sess->iv(o.K, T);
      if (!std::isfinite(miv)) continue;

      // PRICE-in-band.
      const auto fv = american_price(in.S, o.K, T, miv, r, q_eff, o.side,
                                     in.deam.method, in.deam.al_opts);
      if (!fv.has_value()) continue;
      const bool in_px = (*fv >= bid && *fv <= ask);
      ++px_all_n; if (in_px) ++px_all_in;
      if (clean) { ++px_cl_n; if (in_px) ++px_cl_in; }

      // VEGA-WEIGHTED IV-in-band at band_k in {1,2,3}. Invert bid/ask/mid to
      // American IV on the fit's own carry; widen the [IV(bid),IV(ask)] band about
      // IV(mid) by band_k (the Vola "minimum edge" error-bar family). The three
      // inversions are config-independent given (side, K, T, q_eff), so they are
      // memoized across the whole config sweep (see BandCache).
      const BandKey bk{o.K, T, q_eff, static_cast<std::uint8_t>(o.side)};
      BandVal bv;
      if (const auto it = band_cache.find(bk); it != band_cache.end()) {
        bv = it->second;
        ++n_band_hit;
      } else {
        const auto ivb = american_implied_vol(bid, in.S, o.K, T, r, q_eff, o.side, in.deam.method);
        const auto iva = american_implied_vol(ask, in.S, o.K, T, r, q_eff, o.side, in.deam.method);
        const auto ivm = american_implied_vol(o.mid, in.S, o.K, T, r, q_eff, o.side, in.deam.method);
        bv.ok = ivb.has_value() && iva.has_value() && ivm.has_value();
        if (bv.ok) {
          bv.ivb = *ivb;
          bv.iva = *iva;
          bv.ivm = *ivm;
        }
        band_cache.emplace(bk, bv);
        ++n_band_calc;
      }
      const double w = (o.vega > 0.0) ? o.vega * o.vega : 0.0;
      if (bv.ok) {
        const double dn = bv.ivm - std::min(bv.ivb, bv.iva);  // half-band below mid
        const double up = std::max(bv.ivb, bv.iva) - bv.ivm;   // half-band above mid
        auto inband = [&](double kk) {
          return miv >= (bv.ivm - kk * dn) && miv <= (bv.ivm + kk * up);
        };
        w_all += w; if (inband(1.0)) w_all_in += w;
        if (clean) {
          w_cl += w;
          if (inband(1.0)) w_cl_in1 += w;
          if (inband(2.0)) w_cl_in2 += w;
          if (inband(3.0)) w_cl_in3 += w;
        }
        dvol_sum += miv - bv.ivm; ++dvol_n;
      }
    }
  }

  auto pct = [](double a, double b) { return b > 0.0 ? 100.0 * a / b : 0.0; };
  bs.n_all = px_all_n;
  bs.n_clean = px_cl_n;
  bs.px_all = pct(static_cast<double>(px_all_in), static_cast<double>(px_all_n));
  bs.px_clean = pct(static_cast<double>(px_cl_in), static_cast<double>(px_cl_n));
  bs.vw1_all = pct(w_all_in, w_all);
  bs.vw1_clean = pct(w_cl_in1, w_cl);
  bs.vw2_clean = pct(w_cl_in2, w_cl);
  bs.vw3_clean = pct(w_cl_in3, w_cl);
  bs.mean_dvol = dvol_n ? dvol_sum / static_cast<double>(dvol_n) : 0.0;
  return bs;
}

// Raw-board static-arb floor (research protocol): the fraction of interior
// liquid quotes whose MARKET mid price violates butterfly convexity in strike.
// Option price is convex in K (call and put alike), so for same-side sorted
// strikes K0<K1<K2 the non-uniform butterfly must be >= 0:
//   P0·(K2−K1) − P1·(K2−K0) + P2·(K1−K0) >= 0.
// A quote failing this is un-fittable by ANY arbitrage-free surface — it caps the
// attainable % within bid-ask. Reported once (config-independent).
void market_arb_floor(const Underlying& U, double r) {
  const CalibOpts opts{};
  std::size_t n_interior = 0, n_viol = 0;
  double worst_pennies = 0.0;
  for (const Chain& c : U.chains) {
    const double T = c.T;
    if (T < 0.019 || c.strikes.empty()) continue;
    const double df = std::exp(-r * T);
    // Convexity in K is forward-independent; a mid strike is a fine forward proxy
    // just to drive build_observations' preferred-leg selection.
    const double fwd = c.strikes[c.strikes.size() / 2];
    const auto obs = build_observations(c, fwd, T, df, opts);
    if (!obs.has_value() || obs->obs.size() < 7) continue;
    // Split by side, sort by strike, check convexity on interior points.
    for (int s = 0; s < 2; ++s) {
      const Side want = static_cast<Side>(static_cast<std::uint8_t>(s));
      std::vector<std::pair<double, double>> kp;  // (strike, mid)
      for (const FitObs& o : obs->obs) {
        if (o.side == want) kp.emplace_back(o.K, o.mid);
      }
      if (kp.size() < 3) continue;
      std::sort(kp.begin(), kp.end());
      for (std::size_t i = 1; i + 1 < kp.size(); ++i) {
        const double K0 = kp[i - 1].first, K1 = kp[i].first, K2 = kp[i + 1].first;
        const double P0 = kp[i - 1].second, P1 = kp[i].second, P2 = kp[i + 1].second;
        const double bf = P0 * (K2 - K1) - P1 * (K2 - K0) + P2 * (K1 - K0);
        ++n_interior;
        if (bf < 0.0) {
          ++n_viol;
          worst_pennies = std::max(worst_pennies, -100.0 * bf / (K2 - K0));
        }
      }
    }
  }
  std::printf("RAW-BOARD ARB FLOOR: %zu/%zu interior liquid quotes butterfly-violating "
              "(%.1f%%); an arb-free surface cannot reprice these.\n\n",
              n_viol, n_interior,
              n_interior ? 100.0 * static_cast<double>(n_viol) /
                               static_cast<double>(n_interior)
                         : 0.0);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1
      ? argv[1]
      : "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet";
  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  auto panel = load_opra_cbbo_parquet(spec);
  if (!panel.has_value()) {
    std::fprintf(stderr, "load failed: %s\n", panel.error().message().c_str());
    return 1;
  }
  Universe u;
  const auto uid = data_install(u, panel->frame);
  const auto under = uid.has_value() ? u.get_underlying(*uid)
                                     : decltype(u.get_underlying(0)){};
  if (!uid.has_value() || !under.has_value()) {
    std::fprintf(stderr, "install failed\n");
    return 1;
  }
  const Underlying& U = **under;
  const double S = panel->implied_spot;
  const double r = spec.r;
  const auto now_ns = panel->frame.snapshot_ts_ns;

  std::printf("SPY: spot %.2f, snapshot %s\n\n", S, spec.snapshot_iso.c_str());
  market_arb_floor(U, r);
  std::printf("Metrics over the liquid board (T>=1wk). px = model-price-in-[bid,ask];\n"
              "vwIV = vega^2-weighted model-IV-in-band. ALL = whole board; CLEAN = the\n"
              "locally-convex (fittable) subset. band_k widens the error bar (Vola\n"
              "minimum-edge family); SOTA calm ~97-99%% at vwIV clean.\n\n");
  std::printf("%-24s %7s %7s | %8s %8s %8s %8s | %8s\n", "config", "pxALL",
              "pxCLN", "vwIV_k1A", "vwIVk1C", "vwIVk2C", "vwIVk3C", "dVOL");

  BandCache band_cache;
  std::size_t n_band_calc = 0, n_band_hit = 0;
  auto run = [&](const char* name, SessionInputs in,
                 const ConvexFitOpts* convex = nullptr) {
    const BoardScore bs = score_board(panel->frame, U, r, in, band_cache,
                                      n_band_calc, n_band_hit, convex);
    const double ms_slice = bs.n_fit ? bs.fit_ms / static_cast<double>(bs.n_fit) : 0.0;
    std::printf("%-24s %6.1f%% %6.1f%% | %7.1f%% %7.1f%% %7.1f%% %7.1f%% | %+7.4f | %5.1f ms/sl\n",
                name, bs.px_all, bs.px_clean, bs.vw1_all, bs.vw1_clean,
                bs.vw2_clean, bs.vw3_clean, bs.mean_dvol, ms_slice);
  };

  // Baseline eSSVI (3 DoF/slice).
  run("Fast baseline (eSSVI)", make_session_inputs(FitPreset::Fast, S, r, now_ns));

  // Dense C2 full-smile residual (robust IRLS + roughness + local butterfly
  // projection): the arb-safe dense DoF lever, at a couple of roughness settings.
  for (const double lamf : {2.0e-3, 8.0e-3}) {
    SessionInputs in = make_session_inputs(FitPreset::Accurate, S, r, now_ns);
    in.calib.residual_disable = false;
    in.calib.residual_basis_kind = ResidualBasisKind::C2Bspline;
    in.calib.residual_n_basis_terms = 14;
    in.calib.residual_ridge_factor = lamf;
    char name[48];
    std::snprintf(name, sizeof(name), "dense C2 (lam=%.0e)", lamf);
    run(name, in);
  }

  // Phase 1: arbitrage-constrained DENSE convex-QP fit (butterfly no-arb a hard
  // constraint). The session is built only for the per-slice forward/carry; the
  // model IV comes from the convex fit. Probe the speed / accuracy trade of the
  // node count and active-set iteration cap.
  {
    ConvexFitOpts cvx;  // ATM-clustered grid, defaults
    run("convex-QP (40n)", make_session_inputs(FitPreset::Fast, S, r, now_ns), &cvx);
  }
  {
    ConvexFitOpts cvx;
    cvx.node_cap = 56;
    run("convex-QP (56n)", make_session_inputs(FitPreset::Fast, S, r, now_ns), &cvx);
  }

  std::printf(
      "\nNOTE: liquid board T>=1wk, one preferred OTM leg per strike. px_band =\n"
      "raw model-price-in-[bid,ask] (gameable, wing-weighted); vwIVband = vega^2-\n"
      "weighted model-IV-in-[IV(bid),IV(ask)] (defensible SOTA headline, ~97-99%%\n"
      "calm). mean_dVOL<0 => model vol biased below market.\n");
  std::printf(
      "BAND-CACHE: %zu American-IV band inversions computed, %zu served from cache "
      "(%.1f%% reuse across the %d-config sweep).\n",
      n_band_calc, n_band_hit,
      (n_band_calc + n_band_hit) ? 100.0 * static_cast<double>(n_band_hit) /
                                       static_cast<double>(n_band_calc + n_band_hit)
                                 : 0.0,
      5);
  return 0;
}

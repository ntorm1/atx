// spy_dec_curve.cpp — dump the SPY December-2026 expiry vol smile + bid/ask IV
// bands over the normalized-strike range z in [-2, 2], for plotting.
//
// z = ln(K/F) / (sigma_atm * sqrt(T)) is the repo's normalized strike (s3.hpp).
// The primary model curve is the ARBITRAGE-CONSTRAINED CONVEX-QP dense fit
// (fit_convex_slice, node_cap 40) — the same per-slice model that achieves the
// 65.8% price-in-band headline in spy_bidask_bench. The eSSVI 3-parameter
// backbone (FitPreset::Fast) is dumped alongside as a comparison (its ~11% score
// is exactly the smooth-backbone miss this plot makes visible). Per strike we take
// the preferred OTM leg (build_observations survivor), invert its bid/ask/mid to
// American IV on the fit's carry (cold ACCURATE Andersen-Lake, matching the
// bench's defensible band metric), and sample both model curves densely.
//
// CSV on stdout: a #META line, a #CURVE block (z, essvi_iv, convex_iv), and a
// #QUOTES block (z, K, side, bid_iv, ask_iv, mid_iv, essvi_iv, convex_iv).

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "atx/vol/american_iv.hpp"
#include "atx/vol/calib.hpp"        // build_observations, CalibOpts, FitObs
#include "atx/vol/chain.hpp"
#include "atx/vol/data.hpp"         // ns_to_iso_date
#include "atx/vol/dense_slice.hpp"  // fit_convex_slice, ConvexFitOpts, ConvexSliceFit
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/universe.hpp"     // Chain

using namespace atx::vol;

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

  auto chain_r = OptionChain::from_frame(panel->frame, spec.r, panel->implied_spot);
  if (!chain_r.has_value()) {
    std::fprintf(stderr, "chain build failed: %s\n", chain_r.error().message().c_str());
    return 1;
  }
  const OptionChain chain = std::move(*chain_r);

  // Fit the eSSVI session (for the per-slice forward/carry + the comparison curve).
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast}};
  if (const Status st = fitter.fit(chain); !st.has_value()) {
    std::fprintf(stderr, "fit failed: %s\n", st.error().message().c_str());
    return 1;
  }
  const VolaSession& sess = fitter.surface()->session();
  const double S = chain.spot();
  const double r = chain.rate();

  // Locate the December-2026 expiry chain.
  const Underlying& U = chain.underlying();
  const Chain* dec = nullptr;
  for (const Chain& c : U.chains) {
    if (ns_to_iso_date(c.expiry_ns).rfind("2026-12", 0) == 0) {
      dec = &c;
      break;
    }
  }
  if (dec == nullptr) {
    std::fprintf(stderr, "no 2026-12 expiry found\n");
    return 1;
  }

  const double T = dec->T;
  const double F = sess.forward_at(T);
  const double q = sess.q_eff_at(T);
  const double df = std::exp(-r * T);
  if (!(F > 0.0)) {
    std::fprintf(stderr, "degenerate forward\n");
    return 1;
  }

  // The arbitrage-constrained convex-QP dense fit — the 65.8%-in-band model.
  const CalibOpts copts{};
  auto obs = build_observations(*dec, F, T, df, copts);
  if (!obs.has_value() || obs->obs.size() < 5) {
    std::fprintf(stderr, "too few observations for the Dec slice\n");
    return 1;
  }
  // Fit the convex curve on DE-AMERICANIZED (European-equivalent) observations —
  // the fold uses European put-call parity and the model IV is re-Americanized to
  // score, so raw American mids would leave the put early-exercise premium in and
  // lift the near-money put wing (the systematic miss this plot exposed). The
  // market points below still use the raw American `obs`.
  auto obs_eu = build_observations_european(*dec, S, r, F, T, df, copts);
  if (!obs_eu.has_value() || obs_eu->obs.size() < 5) {
    std::fprintf(stderr, "too few European observations for the Dec slice\n");
    return 1;
  }
  ConvexFitOpts cvx;  // node_cap 40 — the spy_bidask_bench convex-QP (40n) config
  auto cfit_r = fit_convex_slice(obs_eu->obs, F, T, df, cvx);
  if (!cfit_r.has_value()) {
    std::fprintf(stderr, "convex fit failed: %s\n", cfit_r.error().message().c_str());
    return 1;
  }
  const ConvexSliceFit& cfit = *cfit_r;

  // Normalize z by the CONVEX model's ATM vol (its iv at k = 0).
  const double atm_vol = cfit.iv(0.0);
  const double sigmahat = atm_vol * std::sqrt(T);
  if (!(sigmahat > 0.0)) {
    std::fprintf(stderr, "degenerate normalization (atm=%.4f)\n", atm_vol);
    return 1;
  }
  const std::string iso = ns_to_iso_date(dec->expiry_ns);

  std::printf("#META spot=%.4f F=%.4f T=%.6f atm_vol=%.6f expiry=%s zmax=2\n",
              S, F, T, atm_vol, iso.c_str());

  // Dense curves over z in [-2, 2]: eSSVI backbone vs convex-QP dense.
  std::printf("#CURVE z,essvi_iv,convex_iv\n");
  for (double z = -2.0; z <= 2.0 + 1e-9; z += 0.04) {
    const double k = z * sigmahat;
    const double K = F * std::exp(k);
    const double essvi = sess.iv(K, T);
    const double convex = cfit.iv(k);
    std::printf("%.4f,%.6f,%.6f\n", z, std::isfinite(essvi) ? essvi : 0.0,
                std::isfinite(convex) ? convex : 0.0);
  }

  // Per-observation OTM leg: market bid/ask/mid inverted to American IV (cold
  // ACCURATE, matching the bench band) + both model IVs.
  std::printf("#QUOTES z,K,side,bid_iv,ask_iv,mid_iv,essvi_iv,convex_iv\n");
  for (const FitObs& o : obs->obs) {
    const double z = o.k / sigmahat;
    if (!(std::fabs(z) <= 2.0)) {
      continue;
    }
    const double half = 0.5 * o.spread;
    const double bid = o.mid - half;
    const double ask = o.mid + half;
    if (!(bid > 0.0) || !(ask > bid)) {
      continue;
    }
    auto inv = [&](double px) {
      const auto v = american_implied_vol(px, S, o.K, T, r, q, o.side,
                                          AmericanMethod::AndersenLake);
      return v.has_value() ? *v : std::nan("");
    };
    const double essvi = sess.iv(o.K, T);
    const double convex = cfit.iv(o.k);
    std::printf("%.4f,%.4f,%c,%.6f,%.6f,%.6f,%.6f,%.6f\n", z, o.K,
                o.side == Side::Call ? 'C' : 'P', inv(bid), inv(ask), inv(o.mid),
                std::isfinite(essvi) ? essvi : 0.0,
                std::isfinite(convex) ? convex : 0.0);
  }
  return 0;
}

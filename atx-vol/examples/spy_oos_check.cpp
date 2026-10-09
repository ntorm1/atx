// spy_oos_check.cpp — OUT-OF-SAMPLE honesty check for the convex-QP dense fit.
//
// The in-sample "% price-in-band" can be inflated by a near-interpolating fit
// that round-trips through the same American pricer used to de-Americanize. This
// harness removes that circularity: per expiry it splits the strikes into a FIT
// set (even index) and a HELD-OUT set (odd index), fits the convex curve on the
// de-Americanized FIT set ONLY, then scores the HELD-OUT strikes — strikes the
// fit never saw — by re-Americanizing the model IV and testing it against the raw
// market NBBO. A high held-out in-band rate means the arb-free surface genuinely
// generalizes between strikes, not that it memorized the training quotes.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/fitting/calib.hpp"
#include "atx/vol/api/fitting/dense_slice.hpp"
#include "atx/vol/api/marketdata/opra_panel.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/marketdata/universe.hpp"

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

  auto in = make_session_inputs(FitPreset::Fast, S, r, panel->frame.snapshot_ts_ns);
  auto sess = VolaSession::from_frame(panel->frame, in);
  if (!sess.has_value()) {
    std::fprintf(stderr, "session build failed\n");
    return 1;
  }
  const auto ctx = sess->expiries();
  const CalibOpts opts{};

  std::size_t is_n = 0, is_in = 0;   // in-sample (fit strikes)
  std::size_t oos_n = 0, oos_in = 0; // out-of-sample (held-out strikes)
  double oos_wsum = 0.0, oos_win = 0.0;  // vega²-weighted OOS

  // Regional OOS buckets (held-out strikes only). Moneyness by k=ln(K/F): the
  // near-money put wing (the reported defect) is k<0 & |k|<0.05; deep wings |k|>=
  // 0.15. Maturity: short <1m, mid 1-6m, long >6m.
  struct Bucket { std::size_t n = 0, in = 0; };
  auto add = [](Bucket& b, bool inb) { ++b.n; if (inb) ++b.in; };
  Bucket put_near, put_mid, put_deep, call_near, call_mid, call_deep;
  Bucket mat_short, mat_midT, mat_long;

  for (const auto& c : ctx) {
    const double T = c.T;
    if (T < 0.019) continue;
    const double F = c.forward;
    const double q_eff = c.q_eff;
    const double df = std::exp(-r * T);
    const Chain* chain = nullptr;
    for (const Chain& ch : U.chains) {
      if (std::fabs(ch.T - T) < 1e-9) { chain = &ch; break; }
    }
    if (chain == nullptr) continue;

    // American obs (raw NBBO bands) and European obs (fit input), same cascade.
    const auto am = build_observations(*chain, F, T, df, opts);
    const auto eu = build_observations_european(*chain, S, r, F, T, df, opts);
    if (!am.has_value() || !eu.has_value()) continue;
    if (am->obs.size() != eu->obs.size() || eu->obs.size() < 8) continue;

    // Sort a shared index by strike so even/odd interleaves fit and holdout.
    const std::size_t m = eu->obs.size();
    std::vector<std::size_t> ord(m);
    for (std::size_t i = 0; i < m; ++i) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) {
      return eu->obs[a].K < eu->obs[b].K;
    });

    // FIT set: even positions in strike order. Need >= 3 for the convex QP.
    std::vector<FitObs> fit_obs;
    fit_obs.reserve(m / 2 + 1);
    for (std::size_t p = 0; p < m; p += 2) fit_obs.push_back(eu->obs[ord[p]]);
    if (fit_obs.size() < 3) continue;

    ConvexFitOpts cvx;  // node_cap 40, same as the headline bench config
    auto cf = fit_convex_slice(fit_obs, F, T, df, cvx);
    if (!cf.has_value()) continue;
    const ConvexSliceFit& cfit = *cf;

    // Score every strike; even = in-sample, odd = held-out. Band = raw American
    // NBBO from the American obs; model = re-Americanized convex model IV.
    for (std::size_t p = 0; p < m; ++p) {
      const FitObs& oe = eu->obs[ord[p]];
      const FitObs& oa = am->obs[ord[p]];
      const double half = 0.5 * oa.spread;
      const double bid = oa.mid - half, ask = oa.mid + half;
      if (!(bid > 0.0) || !(ask > bid)) continue;
      const double miv = cfit.iv(oe.k);
      if (!std::isfinite(miv)) continue;
      const auto fv = american_price(S, oa.K, T, miv, r, q_eff, oa.side,
                                     in.deam.method, in.deam.al_opts);
      if (!fv.has_value()) continue;
      const bool inb = (*fv >= bid && *fv <= ask);
      if (p % 2 == 0) {
        ++is_n; if (inb) ++is_in;
      } else {
        ++oos_n; if (inb) ++oos_in;
        const double w = (oe.vega > 0.0) ? oe.vega * oe.vega : 0.0;
        oos_wsum += w; if (inb) oos_win += w;
        // Regional buckets.
        const double ak = std::fabs(oe.k);
        const bool put = (oa.side == Side::Put);
        Bucket& mb = (ak < 0.05) ? (put ? put_near : call_near)
                   : (ak < 0.15) ? (put ? put_mid : call_mid)
                                 : (put ? put_deep : call_deep);
        add(mb, inb);
        add(T < 0.085 ? mat_short : (T < 0.5 ? mat_midT : mat_long), inb);
      }
    }
  }

  auto pct = [](double a, double b) { return b > 0.0 ? 100.0 * a / b : 0.0; };
  std::printf("SPY convex-QP (40n) leave-every-other-strike-out (T>=1wk):\n");
  std::printf("  IN-SAMPLE  price-in-band: %6.2f%%  (%zu/%zu fit strikes)\n",
              pct(static_cast<double>(is_in), static_cast<double>(is_n)), is_in, is_n);
  std::printf("  OUT-OF-SAMPLE price-in-band: %6.2f%%  (%zu/%zu HELD-OUT strikes)\n",
              pct(static_cast<double>(oos_in), static_cast<double>(oos_n)), oos_in, oos_n);
  std::printf("  OUT-OF-SAMPLE vega^2-weighted: %6.2f%%\n",
              pct(oos_win, oos_wsum));

  auto row = [&](const char* name, const Bucket& b) {
    std::printf("    %-22s %6.2f%%  (%zu/%zu)\n", name,
                pct(static_cast<double>(b.in), static_cast<double>(b.n)), b.in, b.n);
  };
  std::printf("  OOS by region (held-out strikes):\n");
  row("PUT near-money", put_near);
  row("PUT mid |k|.05-.15", put_mid);
  row("PUT deep wing", put_deep);
  row("CALL near-money", call_near);
  row("CALL mid |k|.05-.15", call_mid);
  row("CALL deep wing", call_deep);
  std::printf("  OOS by maturity (held-out strikes):\n");
  row("short  (<1m)", mat_short);
  row("mid    (1-6m)", mat_midT);
  row("long   (>6m)", mat_long);
  return 0;
}

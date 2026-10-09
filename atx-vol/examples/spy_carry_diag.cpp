// spy_carry_diag.cpp — locate the put-call-CONSISTENT forward per expiry using
// the library's OWN Andersen-Lake de-Americanization, and compare it to the
// forward the pipeline (VolaSession, Fast preset) actually produces.
//
// Rationale (goal: 95%+ price-in-band): the forward is correct iff, after a
// consistent American strip, the put-implied and call-implied IV COINCIDE at
// every strike (European parity is exact). We sweep the effective yield q_eff,
// de-Americanize both legs of each near-ATM co-terminal pair via
// american_implied_vol (cold Andersen-Lake), and find q* where the mean signed
// (IV_put - IV_call) crosses zero. F* = S·e^{(r-q*)T} is the consistent forward.
//
// If F* disagrees with the pipeline forward, the borrow/forward solve is biased
// (the reported ~0.9 vol-pt near-ATM put bias). Printed per expiry.

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/marketdata/data.hpp"       // ns_to_iso_date
#include "atx/vol/api/marketdata/opra_panel.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/marketdata/universe.hpp"

using namespace atx::vol;

namespace {

struct Pair {
  double K;
  double call_mid;
  double put_mid;
};

// Mean signed (IV_put - IV_call) over the near-ATM pairs at effective yield q.
// Cold Andersen-Lake inversion of each leg on carry (r, q). Pairs that fail to
// invert are skipped; returns {gap, n_used}.
std::pair<double, int> pc_gap(const std::vector<Pair>& pairs, double S, double T,
                              double r, double q) {
  double sum = 0.0;
  int n = 0;
  for (const Pair& p : pairs) {
    const auto ivc = american_implied_vol(p.call_mid, S, p.K, T, r, q, Side::Call);
    const auto ivp = american_implied_vol(p.put_mid, S, p.K, T, r, q, Side::Put);
    if (!ivc.has_value() || !ivp.has_value()) continue;
    sum += (*ivp - *ivc);
    ++n;
  }
  return {n > 0 ? sum / static_cast<double>(n) : std::nan(""), n};
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

  auto in = make_session_inputs(FitPreset::Fast, S, r, panel->frame.snapshot_ts_ns);
  auto sess = VolaSession::from_frame(panel->frame, in);
  if (!sess.has_value()) {
    std::fprintf(stderr, "session build failed\n");
    return 1;
  }
  const auto ctx = sess->expiries();

  std::printf("SPY carry consistency: spot %.3f, r %.3f%%\n", S, r * 100.0);
  std::printf("F* = forward where American put-IV == call-IV (consistent strip).\n\n");
  std::printf("%-11s %6s %8s %7s | %8s %7s | %7s %8s\n", "expiry", "T",
              "F_pipe", "q_pipe", "F_star", "q_star", "dF", "gap@pipe");

  for (const auto& c : ctx) {
    const double T = c.T;
    if (T < 0.02) continue;
    const double F_pipe = c.forward;
    const double q_pipe = c.q_eff;

    const Chain* chain = nullptr;
    for (const Chain& ch : U.chains) {
      if (std::fabs(ch.T - T) < 1e-9) { chain = &ch; break; }
    }
    if (chain == nullptr) continue;

    // Near-ATM (|K/S-1| < 5%) co-terminal pairs, both legs two-sided.
    std::vector<Pair> pairs;
    const std::size_t ns = chain->n_strikes();
    for (std::size_t i = 0; i < ns; ++i) {
      const double K = chain->strikes[i];
      if (!(K > 0.0) || std::fabs(K / S - 1.0) > 0.05) continue;
      const std::size_t ci = chain_index(static_cast<std::uint16_t>(i), Side::Call);
      const std::size_t pi = chain_index(static_cast<std::uint16_t>(i), Side::Put);
      const double cb = chain->bids[ci], ca = chain->asks[ci], cm = chain->mids[ci];
      const double pb = chain->bids[pi], pa = chain->asks[pi], pm = chain->mids[pi];
      if (!(cb > 0.0 && ca > cb && cm > 0.0)) continue;
      if (!(pb > 0.0 && pa > pb && pm > 0.0)) continue;
      pairs.push_back(Pair{K, cm, pm});
    }
    if (pairs.size() < 5) continue;

    // gap(q) is decreasing in q (higher q -> lower F -> put IV down, call IV up).
    // Bisect for the zero crossing on a wide yield bracket around the pipeline q.
    double qlo = q_pipe - 0.03, qhi = q_pipe + 0.04;
    auto glo = pc_gap(pairs, S, T, r, qlo).first;
    auto ghi = pc_gap(pairs, S, T, r, qhi).first;
    double q_star = std::nan("");
    if (std::isfinite(glo) && std::isfinite(ghi) && glo > 0.0 && ghi < 0.0) {
      for (int it = 0; it < 40; ++it) {
        const double qm = 0.5 * (qlo + qhi);
        const double gm = pc_gap(pairs, S, T, r, qm).first;
        if (!std::isfinite(gm)) break;
        if (gm > 0.0) qlo = qm; else qhi = qm;
      }
      q_star = 0.5 * (qlo + qhi);
    }
    const double F_star = S * std::exp((r - q_star) * T);
    const auto [gap_pipe, npairs] = pc_gap(pairs, S, T, r, q_pipe);
    const std::string iso = ns_to_iso_date(chain->expiry_ns);
    std::printf("%-11s %6.3f %8.3f %6.3f%% | %8.3f %6.3f%% | %+6.3f %+8.4f  (n=%d)\n",
                iso.c_str(), T, F_pipe, q_pipe * 100.0, F_star, q_star * 100.0,
                F_star - F_pipe, gap_pipe, npairs);
  }
  return 0;
}

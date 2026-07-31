// AL preset ladder — accuracy x cost frontier for the Andersen-Lake solver, one
// row per QuantLib-QdFpAmericanEngine-mapped preset RUNG, scored on a real (or
// representative) SPY-OPRA strike/T/vol grid. WS-K task K1.
//
// ── Why this bench exists (the K1 finding it makes measurable) ───────────────
// QuantLib's QdFpAmericanEngine schemes DECOUPLE the two Gauss-Legendre orders:
//   QdFpLegendreScheme(l, m, n, p) — l = fixed-point quadrature order (locate the
//   early-exercise boundary), p = final PRICING quadrature order (the premium
//   integral along the converged boundary). Its fast scheme is (l=7, m=2, n=7,
//   p=27): a CHEAP boundary-locating quadrature paired with an ACCURATE pricing
//   quadrature. Source: QuantLib ql/pricingengines/vanilla/qdfpamericanengine.cpp
//   (lballabio/QuantLib); Andersen, Lake, Offengenden, "High-Performance American
//   Option Pricing", SSRN 2547027 (2015).
//
// Our public AlOpts exposes a SINGLE quadrature knob (n_quadrature) and the
// production AlOpts->AlScheme map (american.cpp scheme_from_opts) sets
// n_quad_price = n_quad_fp — it CANNOT express l != p. `al_fast_opts()` is
// {7,16,4,1e-8} -> {n_boundary=7, n_quad_fp=16, n_quad_price=16, ...}: it OVERpays
// the fixed-point quadrature (16 vs QuantLib's 7) while UNDERpaying the premium
// quadrature (16 vs QuantLib's 27). The fixed-point block is the dominant cold
// cost (it runs n_quad_fp * n_boundary * n_sweeps times per solve — american.cpp
// eqn_b_ND), so cutting n_quad_fp is where the time is, and the premium integral
// runs ONCE per solve so raising it is cheap accuracy. This bench measures that
// decoupling headroom directly by driving the premium order INDEPENDENTLY through
// the existing `detail::andersen_lake_seeded(..., n_quad_price)` measurement seam.
//
// ── Rungs (each an `american/ladder/<name>` row) ─────────────────────────────
//   reference     — richest in-repo scheme; the accuracy DENOMINATOR (err == 0)
//   accurate      — the production ACCURATE preset (nullopt): {12,24,48,2,4,1e-10}
//   fast          — al_fast_opts(): {7,16,16,2,2,1e-8} (n_quad_price tied to fp)
//   ql_fast       — QuantLib fast (l=7,m=2,n=7,p=27) mapped: fp=8, price=32, nb=7
//   ql_accurate   — QuantLib accurate (l=25,m=5,n=13,eps1e-8) mapped: fp=24,price=48,nb=13
//   fast_p32      — interpolated: current fast boundary/iters, premium DECOUPLED
//                   16->32 (isolates "raise premium only" on the current fast tier)
//   mid           — interpolated balance point: nb=9, fp=16, price=24, 6 sweeps
//
// Each row carries: real_time = per-op wall; counters us_per_op, max_abs_err,
// med_abs_err, p99_abs_err (all vs the reference), the scheme knobs
// (nb/fp/price/iter), grid size, and grid_real (1 = harvested from a real fitted
// SPY OPRA board; 0 = the self-contained representative SPY-shaped grid).
//
// PROVISIONAL: this host is shared. Per §0/§3 of the solve-wall sprint, the
// RELATIVE A/B between rungs in ONE run is citable; the ABSOLUTE us_per_op is
// provisional until the WS-V quiet-window re-capture (V3). Emit with:
//   atx-vol-al-preset-ladder-bench \
//     --benchmark_out=bench/baselines/i7-1260p-clang18-avx2-al-preset-ladder.json \
//     --benchmark_out_format=json
//
// Class: test/infra (measurement only — no production code path changes; every
// rung routes through the pre-existing A6 `andersen_lake_seeded` bench seam).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"
#include "atx/vol/types.hpp"

// Real-OPRA harvest (optional; falls back to a representative grid when the
// parquet fixture is absent). Same testkit fixtures every tests/*.cpp and the
// fitting bench use; ../tests is on this target's PRIVATE include path (CMake).
#include "atx/vol/session.hpp"   // VolaSession, make_session_inputs, FitPreset
#include "atx/vol/universe.hpp"  // Underlying
#include "support/spy_fit_fixture.hpp" // load_spy_fit_fixture, kSpyFitFixtures

#include "bench_util.hpp"

namespace {

using atx::vol::AlOpts;
using atx::vol::Side;

// ── The (S,K,T,sigma,r,q,side) grid ──────────────────────────────────────────
struct GridPt {
  double S, K, T, sigma, r, q;
  Side side;
};

// SPY-OPRA moneyness ladder (K/F), dense near-the-money, thinner wings — the
// shape a liquid SPY board actually quotes. Shared by the real harvest (strikes
// sampled on the real forward) and the representative fallback.
constexpr double kMoneyness[] = {0.80,  0.85, 0.90,  0.925, 0.95, 0.975, 1.00,
                                 1.025, 1.05, 1.075, 1.10,  1.15, 1.20};

// Representative SPY term grid (year-fractions ~1wk .. 1y), used only when no
// real board is available.
constexpr double kMaturities[] = {0.019, 0.038, 0.083, 0.167, 0.25, 0.5, 0.75, 1.0};

// A representative SPY skew: ATM term structure + a put-heavy smile in
// log-moneyness. Not a fit to any one board — a stand-in whose (moneyness, T,
// vol) DISTRIBUTION matches a liquid SPY surface so the ladder's accuracy/cost
// numbers weight the regions the engine actually prices. Clamped to a sane band.
[[nodiscard]] double representative_sigma(double k_log, double T) noexcept {
  const double atm = 0.115 + 0.035 * std::sqrt(T);
  double s = atm * (1.0 - 0.90 * k_log + 1.5 * k_log * k_log);
  return std::clamp(s, 0.06, 0.90);
}

// Build the representative SPY-shaped grid (self-contained, no fixture needed).
[[nodiscard]] std::vector<GridPt> representative_grid() {
  constexpr double S = 600.0;   // SPY ~600 (2026)
  constexpr double r = 0.043;   // matches the OPRA fixtures' flat rate
  constexpr double q = 0.013;   // SPY continuous div yield ~1.3%
  std::vector<GridPt> g;
  for (const double T : kMaturities) {
    const double F = S * std::exp((r - q) * T);
    for (const double m : kMoneyness) {
      const double K = F * m;
      const double k_log = std::log(K / F);
      const double sigma = representative_sigma(k_log, T);
      // OTM side: puts below spot, calls at/above — exercises both boundary
      // code paths (calls route via the McDonald-Schroder internal put).
      const Side side = (K < S) ? Side::Put : Side::Call;
      g.push_back(GridPt{S, K, T, sigma, r, q, side});
    }
  }
  return g;
}

// Try to harvest a REAL grid from a fitted SPY OPRA board (the 2026-06-05
// stress-close slice — the same board the pxCLN 99.5% headline is defined on).
// Fits the production Fast session ONCE (outside any timed loop), then samples
// the fitted smile on the real forward at every real expiry. Returns empty when
// the parquet fixture is not present (source-only CI) -> caller uses the
// representative grid.
[[nodiscard]] std::vector<GridPt> real_spy_grid() {
  std::vector<GridPt> g;
  // stress-close (2026-06-05T1955Z) — index 9 in kSpyFitFixtures.
  const auto board = atx::vol::testkit::load_spy_fit_fixture(
      atx::vol::testkit::kSpyFitFixtures[9]);
  if (!board.has_value()) {
    return g;
  }
  const double S = board->spot();
  const double r = board->r;
  auto sess = atx::vol::VolaSession::build(
      board->underlying(),
      atx::vol::make_session_inputs(atx::vol::FitPreset::Fast, S, r, board->now_ns()));
  if (!sess.has_value()) {
    return g;
  }
  for (const auto &c : sess->expiries()) {
    if (c.T < 0.02) {
      continue; // drop sub-1wk / expiring
    }
    const double F = c.forward;
    const double q = c.q_eff;
    for (const double m : kMoneyness) {
      const double K = F * m;
      const double sigma = sess->iv(K, c.T);
      if (!std::isfinite(sigma) || sigma <= 0.0) {
        continue;
      }
      const Side side = (K < S) ? Side::Put : Side::Call;
      g.push_back(GridPt{S, K, c.T, sigma, r, q, side});
    }
  }
  return g;
}

// The grid + whether it came from a real board — built once, shared by every row.
struct Grid {
  std::vector<GridPt> pts;
  bool real = false;
};
[[nodiscard]] const Grid &grid() {
  static const Grid g = [] {
    Grid out;
    out.pts = real_spy_grid();
    out.real = !out.pts.empty();
    if (!out.real) {
      out.pts = representative_grid();
    }
    return out;
  }();
  return g;
}

// ── Preset rungs ─────────────────────────────────────────────────────────────
//
// Every rung is expressed through the ONE measurement seam
// `detail::andersen_lake_seeded(opts, seed=Baw, n_quad_price)`:
//   * opts == nullopt        -> the internal ACCURATE preset {12,24,48,2,4,1e-10}
//   * opts engaged           -> scheme_from_opts maps the knobs; premium tied to fp
//   * n_quad_price != 0       -> DECOUPLE the premium GL order from fp (the QuantLib
//                               l!=p axis our public AlOpts cannot express)
// seed is pinned to Baw (the production default; QD+ regressed the fast tier per
// american_boundary.hpp), so seeded(opts,Baw,0) reproduces the production path
// bit-for-bit.
struct Rung {
  const char *name;
  std::optional<AlOpts> opts;
  std::uint16_t price; // premium GL override (0 = keep the mapped n_quad_fp)
};

// AlOpts is designated-init only (american.hpp construction contract). Each
// rung's premium GL order rides `Rung::price` (the seeded-call override) rather
// than AlOpts::n_quad_price, so the ladder sweeps p independently of the opts.
// Quadrature is quantized by scheme_from_opts to {8,16,24,32,48,64};
// n_collocation -> n_boundary (clamped [6,32]); max_newton_iter ->
// (jn=min(2,tot), fp=tot-jn).
[[nodiscard]] const std::vector<Rung> &rungs() {
  static const std::vector<Rung> r = {
      // richest in-repo scheme; accuracy denominator.
      {"reference",
       AlOpts{.n_collocation = 16, .n_quadrature = 64, .max_newton_iter = 12, .tol = 1.0e-12}, 64},
      // production ACCURATE (nullopt): {12,24,48,2,4,1e-10}.
      {"accurate", std::nullopt, 0},
      // production fast al_fast_opts(): {7,16,16,2,2,1e-8} (price tied to fp=16).
      {"fast",
       AlOpts{.n_collocation = 7, .n_quadrature = 16, .max_newton_iter = 4, .tol = 1.0e-8}, 0},
      // QuantLib fast (l=7,m=2,n=7,p=27): cheap fp=8, DECOUPLED rich premium=32,
      // nb=7, 2 sweeps. The headline "decoupled" rung.
      {"ql_fast",
       AlOpts{.n_collocation = 7, .n_quadrature = 8, .max_newton_iter = 2, .tol = 1.0e-8}, 32},
      // QuantLib accurate (l=25,m=5,n=13,eps1e-8): fp=24, premium=48, nb=13, 5 sweeps.
      {"ql_accurate",
       AlOpts{.n_collocation = 13, .n_quadrature = 24, .max_newton_iter = 5, .tol = 1.0e-8}, 48},
      // interpolated: current fast boundary/iters, premium DECOUPLED 16->32.
      {"fast_p32",
       AlOpts{.n_collocation = 7, .n_quadrature = 16, .max_newton_iter = 4, .tol = 1.0e-8}, 32},
      // interpolated balance: nb=9, fp=16, premium=24, 6 sweeps.
      {"mid",
       AlOpts{.n_collocation = 9, .n_quadrature = 16, .max_newton_iter = 6, .tol = 1.0e-9}, 24},
  };
  return r;
}

[[nodiscard]] double price_rung(const GridPt &p, const Rung &rung) noexcept {
  const atx::vol::Result<double> r = atx::vol::detail::andersen_lake_seeded(
      p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, rung.opts, atx::vol::detail::AlSeedMode::Baw,
      rung.price);
  return r.has_value() ? *r : std::numeric_limits<double>::quiet_NaN();
}

// Reference prices for the shared grid (computed once — the accuracy denominator).
[[nodiscard]] const std::vector<double> &reference_prices() {
  static const std::vector<double> refs = [] {
    const Grid &g = grid();
    const Rung &ref = rungs()[0];
    std::vector<double> v(g.pts.size());
    for (std::size_t i = 0; i < g.pts.size(); ++i) {
      v[i] = price_rung(g.pts[i], ref);
    }
    return v;
  }();
  return refs;
}

struct Accuracy {
  double max_abs = 0.0;
  double med_abs = 0.0;
  double p99_abs = 0.0;
};
[[nodiscard]] Accuracy accuracy_vs_reference(const Rung &rung) {
  const Grid &g = grid();
  const std::vector<double> &refs = reference_prices();
  std::vector<double> errs;
  errs.reserve(g.pts.size());
  double max_abs = 0.0;
  for (std::size_t i = 0; i < g.pts.size(); ++i) {
    const double p = price_rung(g.pts[i], rung);
    if (!std::isfinite(p) || !std::isfinite(refs[i])) {
      continue;
    }
    const double e = std::abs(p - refs[i]);
    errs.push_back(e);
    max_abs = std::max(max_abs, e);
  }
  Accuracy a;
  a.max_abs = max_abs;
  if (!errs.empty()) {
    std::sort(errs.begin(), errs.end());
    a.med_abs = errs[errs.size() / 2];
    a.p99_abs = errs[static_cast<std::size_t>(0.99 * static_cast<double>(errs.size() - 1))];
  }
  return a;
}

// Decode a rung's resolved scheme knobs for the JSON (so a reader sees exactly
// what (nb, fp, price, iter) each row priced). Mirrors scheme_from_opts.
struct SchemeKnobs {
  double nb, fp, price, iter;
};
[[nodiscard]] SchemeKnobs decode(const Rung &rung) {
  if (!rung.opts) {
    return {12, 24, 48, 6}; // nullopt ACCURATE: 2 JN + 4 FP = 6 sweeps
  }
  const AlOpts &o = *rung.opts;
  double fp = 8;
  for (const double q : {64.0, 48.0, 32.0, 24.0, 16.0, 8.0}) {
    if (o.n_quadrature >= q) {
      fp = q;
      break;
    }
  }
  const double nb = std::clamp<double>(o.n_collocation, 6, 32);
  const double price = rung.price != 0 ? rung.price : fp;
  const double iter = o.max_newton_iter;
  return {nb, fp, price, iter};
}

void run_rung(benchmark::State &state, const Rung &rung) {
  const Grid &g = grid();
  double sink = 0.0;
  for (auto _ : state) {
    for (const GridPt &p : g.pts) {
      sink += price_rung(p, rung);
    }
    benchmark::DoNotOptimize(sink);
  }
  benchmark::ClobberMemory();
  const double ops = static_cast<double>(state.iterations()) * static_cast<double>(g.pts.size());
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  const Accuracy a = accuracy_vs_reference(rung);
  const SchemeKnobs k = decode(rung);
  state.counters["grid"] = static_cast<double>(g.pts.size());
  state.counters["grid_real"] = g.real ? 1.0 : 0.0;
  state.counters["us_per_op"] =
      benchmark::Counter(ops * 1e-6, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["max_abs_err"] = a.max_abs;
  state.counters["med_abs_err"] = a.med_abs;
  state.counters["p99_abs_err"] = a.p99_abs;
  state.counters["n_boundary"] = k.nb;
  state.counters["n_quad_fp"] = k.fp;
  state.counters["n_quad_price"] = k.price;
  state.counters["n_sweeps"] = k.iter;
}

const bool kRegistered = [] {
  using atx::vol::bench::apply_common;
  for (const Rung &rung : rungs()) {
    const std::string name = std::string("american/ladder/") + rung.name;
    apply_common(benchmark::RegisterBenchmark(
                     name, [&rung](benchmark::State &state) { run_rung(state, rung); }))
        ->Unit(benchmark::kMicrosecond);
  }
  return true;
}();

} // namespace

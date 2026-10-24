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
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_batch.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "pricing/adjoint_greeks.hpp"        // L3g: the adjoint bundle's µs on the same contracts
#include "pricing/al_probe.hpp"
#include "pricing/american_boundary.hpp"      // L2h-c: amer:: seam for the sqbuild parts row
#include "pricing/american_detail.hpp"
#include "pricing/american_sigma_family.hpp" // L3g: sigma_family_detail::family_rho_variants
#include "pricing/american_sigma_q_family.hpp" // K1: the fused price + vega rows (sqfused)
#include "pricing/scalar_erfc.hpp"
#include "simd/american_boundary_nodes_avx2.hpp" // L2h-c: the 4-lane node solve (parts row)
#include "atx/vol/api/simd/cpu.hpp"              // L2h-c: SimdIsa override for sqbuild rows
#include "atx/core/math.hpp"
#include "atx/vol/api/core/types.hpp"

// Real-OPRA harvest (optional; falls back to a representative grid when the
// parquet fixture is absent). Same testkit fixtures every tests/*.cpp and the
// fitting bench use; ../tests is on this target's PRIVATE include path (CMake).
#include "atx/vol/api/fitting/session.hpp"   // VolaSession, make_session_inputs, FitPreset
#include "atx/vol/api/marketdata/universe.hpp"  // Underlying
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

// ── al-speed lane (2026-10-17): single-op latency, kernel fingerprint, profile ──
//
// Three additions, all measurement-only:
//   * american/ladder_latency/<preset> — times EVERY single cold price on the grid
//     (steady_clock, i.e. QueryPerformanceCounter on Windows, 100 ns resolution
//     against a ~20 us op) and reports p50/p90/p99/mean per op, which the
//     whole-grid ladder rows above cannot (they time 104 ops per iteration).
//     Presets go through the PUBLIC andersen_lake entry with the shipped opts, so
//     the row is the production path, not the seeded measurement seam.
//   * ATX_VOL_AL_FINGERPRINT=<path> registers american/ladder_fingerprint, which
//     writes every double the AL kernel's entry points return over a fixed input
//     set (the ladder grid on every rung, a deterministic 6,000-contract random
//     set on the four presets, Greeks, IVs, slices, batch, boundary, BAW) to
//     <path> as raw little-endian doubles and prints one FNV-1a hash per section.
//     Two builds are bit-identical on the kernel exactly when the files' sha256
//     agree; the section hashes say where they first differ.
//   * ATX_VOL_AL_PROFILE=1 registers american/ladder_profile/<preset> (the bare
//     grid loop, for alprobe zone attribution under ATX_VOL_AL_PROBE=z, dumped at
//     exit), the BAW seed Newton iteration census, and libm unit-cost rows.
//   * ATX_VOL_AL_DEV200=<csv> (a kernel-input file, see load_kernel_inputs) also
//     registers american/ladder_latency_dev200/<preset>: the ladder_latency timing
//     loop over that file's contracts instead of the 104-point grid (al-speed fix
//     round 1). The grid stays the declared latency set; this row is supplementary.

[[nodiscard]] std::string env_text(const char *name) {
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&raw, &n, name) != 0 || raw == nullptr) {
    return {};
  }
  std::string s(raw);
  std::free(raw);
  return s;
#else
  const char *raw = std::getenv(name);
  return raw != nullptr ? std::string(raw) : std::string();
#endif
}

struct Preset {
  const char *name;
  std::optional<AlOpts> opts;
};

// The shipped presets, priced through the public entry point, then the same three
// schemes on the opt-in AlProfile::Fast kernel arithmetic (al-speed tier 2).
[[nodiscard]] const std::vector<Preset> &presets() {
  static const std::vector<Preset> p = {
      {"accurate", std::nullopt},
      {"fast", atx::vol::al_fast_opts()},
      {"bulk", atx::vol::al_bulk_opts()},
      {"accurate_fastprofile", atx::vol::al_fast_profile(std::nullopt)},
      {"fast_fastprofile", atx::vol::al_fast_profile(atx::vol::al_fast_opts())},
      {"bulk_fastprofile", atx::vol::al_fast_profile(atx::vol::al_bulk_opts())},
      // L2f al-fast-tier (2026-10-19): the opt-in Screen tier.
      {"screen", atx::vol::al_screen_opts()},
  };
  return p;
}

// The Reference presets only (the first three rows above): the error rows below
// pair each with its al_fast_profile twin.
[[nodiscard]] std::size_t reference_preset_count() noexcept { return 3; }

[[nodiscard]] double sorted_quantile(const std::vector<double> &s, double q) {
  if (s.empty()) {
    return 0.0;
  }
  const auto idx = static_cast<std::size_t>(q * static_cast<double>(s.size() - 1));
  return s[idx];
}

// Times every single cold price over `pts`. An empty set is skipped, never spun: a
// zero manual time per iteration would never reach the minimum run time.
void run_latency_on(benchmark::State &state, const std::vector<GridPt> &pts,
                    const Preset &preset) {
  if (pts.empty()) {
    state.SkipWithError("no contracts to time (unreadable or empty kernel-input file)");
    return;
  }
  constexpr std::size_t kCap = std::size_t{1} << 18;
  std::vector<double> samples_us;
  samples_us.reserve(kCap);
  double sink = 0.0;
  for (auto _ : state) {
    double total_us = 0.0;
    for (const GridPt &p : pts) {
      const auto t0 = std::chrono::steady_clock::now();
      const atx::vol::Result<double> r =
          atx::vol::andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, preset.opts);
      const auto t1 = std::chrono::steady_clock::now();
      sink += r.has_value() ? *r : 0.0;
      const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      total_us += us;
      if (samples_us.size() < kCap) {
        samples_us.push_back(us);
      }
    }
    benchmark::DoNotOptimize(sink);
    state.SetIterationTime(total_us * 1.0e-6);
  }
  std::sort(samples_us.begin(), samples_us.end());
  double mean = 0.0;
  for (const double v : samples_us) {
    mean += v;
  }
  mean = samples_us.empty() ? 0.0 : mean / static_cast<double>(samples_us.size());
  const double ops = static_cast<double>(state.iterations()) * static_cast<double>(pts.size());
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  state.counters["grid"] = static_cast<double>(pts.size()); // contracts per iteration
  state.counters["samples"] = static_cast<double>(samples_us.size());
  state.counters["p50_us"] = sorted_quantile(samples_us, 0.50);
  state.counters["p90_us"] = sorted_quantile(samples_us, 0.90);
  state.counters["p99_us"] = sorted_quantile(samples_us, 0.99);
  state.counters["mean_us"] = mean;
}

void run_latency(benchmark::State &state, const Preset &preset) {
  run_latency_on(state, grid().pts, preset);
}

// ── Fingerprint ───────────────────────────────────────────────────────────────

// splitmix64: a fixed, library-independent stream, so the input set is identical
// in every build regardless of the standard library's distributions.
class Stream {
public:
  explicit Stream(std::uint64_t seed) noexcept : s_(seed) {}
  [[nodiscard]] double uniform() noexcept {
    s_ += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = s_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * 0x1.0p-53;
  }
  [[nodiscard]] double range(double lo, double hi) noexcept { return lo + (hi - lo) * uniform(); }
  [[nodiscard]] double log_range(double lo, double hi) noexcept {
    return std::exp(range(std::log(lo), std::log(hi)));
  }

private:
  std::uint64_t s_;
};

// A deterministic contract set wide enough to reach every kernel branch: both
// sides, deep ITM/OTM, 1 day to 5 years, near-zero to very high vol, zero and
// negative rates and yields (the European / Unsupported / benign-flat corners).
[[nodiscard]] std::vector<GridPt> fingerprint_contracts(std::size_t n) {
  Stream rng(0x5eed2026'10170001ULL);
  std::vector<GridPt> v;
  v.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double S = 100.0;
    const double K = S * std::exp(rng.range(-0.8, 0.8));
    const double T = rng.log_range(1.0 / 365.0, 5.0);
    const double sigma = rng.log_range(0.04, 1.5);
    const double ur = rng.uniform();
    const double r = ur < 0.08 ? 0.0 : (ur < 0.12 ? -0.004 : rng.range(0.0005, 0.10));
    const double uq = rng.uniform();
    const double q = uq < 0.10 ? 0.0 : (uq < 0.15 ? -0.01 : rng.range(0.0005, 0.25));
    const Side side = rng.uniform() < 0.5 ? Side::Put : Side::Call;
    v.push_back(GridPt{S, K, T, sigma, r, q, side});
  }
  return v;
}

class Fingerprint {
public:
  void section(const char *name) {
    close_section();
    name_ = name;
    hash_ = 0xcbf29ce484222325ULL;
    count_ = 0;
  }
  void add(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    for (int b = 0; b < 8; ++b) {
      hash_ ^= (bits >> (8 * b)) & 0xffULL;
      hash_ *= 0x100000001b3ULL;
    }
    values_.push_back(v);
    ++count_;
  }
  // Errors are part of the contract too: encode the code as a distinct value.
  template <class T> void add_result(const atx::vol::Result<T> &r, double value) {
    if (r.has_value()) {
      add(value);
    } else {
      add(-1.0e6 - static_cast<double>(static_cast<int>(r.error().code())));
    }
  }
  void add_status(const atx::vol::Status &s) {
    add(s.has_value() ? 0.0 : -1.0e6 - static_cast<double>(static_cast<int>(s.error().code())));
  }
  void finish(const std::string &path) {
    close_section();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(values_.data()),
              static_cast<std::streamsize>(values_.size() * sizeof(double)));
    std::fprintf(stderr, "[al-fingerprint] total doubles=%zu -> %s (%s)\n", values_.size(),
                 path.c_str(), out.good() ? "written" : "WRITE FAILED");
  }
  [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

private:
  void close_section() {
    if (name_ != nullptr) {
      std::fprintf(stderr, "[al-fingerprint] %-24s n=%-7zu fnv1a=%016llx\n", name_, count_,
                   static_cast<unsigned long long>(hash_));
    }
    name_ = nullptr;
  }
  const char *name_ = nullptr;
  std::uint64_t hash_ = 0;
  std::size_t count_ = 0;
  std::vector<double> values_;
};

void add_greeks(Fingerprint &fp, const atx::vol::Result<atx::vol::AmericanGreeks> &g) {
  if (!g.has_value()) {
    fp.add_result(g, 0.0);
    return;
  }
  for (const double v : {g->price, g->delta, g->gamma, g->vega, g->theta, g->rho, g->vanna,
                         g->volga, g->charm}) {
    fp.add(v);
  }
}

void fingerprint_sections(Fingerprint &fp) {
  using namespace atx::vol;
  const Grid &g = grid();
  fp.section("ladder_grid_rungs");
  for (const Rung &rung : rungs()) {
    for (const GridPt &p : g.pts) {
      fp.add(price_rung(p, rung));
    }
  }
  const std::vector<GridPt> c = fingerprint_contracts(6000);
  const std::optional<AlOpts> opts4[] = {std::nullopt, al_fast_opts(), al_bulk_opts(),
                                         al_default_opts()};
  fp.section("random_presets");
  for (const auto &o : opts4) {
    for (const GridPt &p : c) {
      const Result<double> r = andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, o);
      fp.add_result(r, r.has_value() ? *r : 0.0);
    }
  }
  fp.section("convergence");
  for (std::size_t i = 0; i < 600; ++i) {
    const GridPt &p = c[i];
    const auto rep = andersen_lake_convergence(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side,
                                               i % 2 == 0 ? opts4[0] : opts4[2]);
    fp.add_result(rep, rep.has_value() ? rep->resid : 0.0);
  }
  fp.section("greeks_al");
  for (std::size_t i = 0; i < 600; ++i) {
    const GridPt &p = c[i];
    add_greeks(fp, american_greeks_al(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, opts4[0]));
    add_greeks(fp, american_greeks_al(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, opts4[1]));
  }
  fp.section("greeks_fd");
  for (std::size_t i = 600; i < 900; ++i) {
    const GridPt &p = c[i];
    add_greeks(fp, american_greeks_fd(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side,
                                      AmericanMethod::AndersenLake, opts4[0], false));
    add_greeks(fp, american_greeks_fd(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side,
                                      AmericanMethod::AndersenLake, opts4[1], true));
  }
  fp.section("delta_vega");
  for (std::size_t i = 900; i < 1500; ++i) {
    const GridPt &p = c[i];
    const Result<double> d = american_delta(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side,
                                            AmericanMethod::AndersenLake, opts4[2]);
    fp.add_result(d, d.has_value() ? *d : 0.0);
    const Result<double> v = american_vega_al(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side,
                                              opts4[0]);
    fp.add_result(v, v.has_value() ? *v : 0.0);
  }
  fp.section("implied_vol");
  for (std::size_t i = 1500; i < 2100; ++i) {
    const GridPt &p = c[i];
    for (std::size_t k = 0; k < 3; ++k) {
      const Result<double> px = andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, opts4[k]);
      if (!px.has_value()) {
        fp.add_result(px, 0.0);
        continue;
      }
      const Result<double> iv = american_implied_vol(*px, p.S, p.K, p.T, p.r, p.q, p.side,
                                                     AmericanMethod::AndersenLake, 1.0e-7, 64,
                                                     opts4[k]);
      fp.add_result(iv, iv.has_value() ? *iv : 0.0);
    }
  }
  fp.section("exercise_boundary_baw");
  for (std::size_t i = 2100; i < 2700; ++i) {
    const GridPt &p = c[i];
    const Result<double> b =
        exercise_boundary(p.K, p.T, p.sigma, p.r, p.q, p.side, i % 2 == 0 ? opts4[0] : opts4[1]);
    fp.add_result(b, b.has_value() ? *b : 0.0);
    const Result<double> w = baw_american(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side);
    fp.add_result(w, w.has_value() ? *w : 0.0);
  }
  fp.section("slices");
  {
    Stream rng(0x51ce2026'10170002ULL);
    std::vector<double> strikes(25);
    std::vector<double> out(25);
    for (int row = 0; row < 40; ++row) {
      const double S = 100.0;
      const double T = rng.log_range(0.02, 3.0);
      const double sigma = rng.log_range(0.08, 0.9);
      const double r = rng.range(0.001, 0.08);
      const double q = rng.range(0.0, 0.12);
      for (std::size_t k = 0; k < strikes.size(); ++k) {
        strikes[k] = S * std::exp(-0.5 + static_cast<double>(k) * (1.0 / 24.0));
      }
      const std::optional<AlOpts> &o = opts4[static_cast<std::size_t>(row % 3)];
      fp.add_status(andersen_lake_put_slice(S, strikes, T, sigma, r, q, out, o));
      for (const double v : out) {
        fp.add(v);
      }
      fp.add_status(andersen_lake_call_slice(S, strikes, T, sigma, r, q, out, o));
      for (const double v : out) {
        fp.add(v);
      }
    }
  }
  fp.section("batch_resolved");
  {
    Stream rng(0xba7c2026'10170003ULL);
    constexpr std::size_t kN = 37;
    std::vector<double> K(kN), sig(kN), price(kN);
    std::vector<Side> side(kN);
    std::vector<Status> status(kN);
    for (int chain = 0; chain < 12; ++chain) {
      ResolvedAmericanPriceBatchRequest req;
      req.S = 100.0;
      req.T = rng.log_range(0.02, 2.0);
      req.r = rng.range(0.001, 0.07);
      req.q = rng.range(0.0, 0.08);
      for (std::size_t k = 0; k < kN; ++k) {
        K[k] = req.S * std::exp(rng.range(-0.4, 0.4));
        sig[k] = rng.log_range(0.1, 0.8);
        side[k] = rng.uniform() < 0.5 ? Side::Put : Side::Call;
      }
      req.K = K;
      req.sigma = sig;
      req.side = side;
      req.al_opts = chain % 2 == 0 ? opts4[0] : opts4[2];
      req.price = price;
      req.status = status;
      const Result<std::size_t> rc = american_price_batch_resolved(req);
      fp.add_result(rc, rc.has_value() ? static_cast<double>(*rc) : 0.0);
      for (std::size_t k = 0; k < kN; ++k) {
        fp.add_status(status[k]);
        fp.add(price[k]);
      }
    }
  }
}

void run_fingerprint(benchmark::State &state, const std::string &path) {
  std::size_t n = 0;
  for (auto _ : state) {
    Fingerprint fp;
    fingerprint_sections(fp);
    fp.finish(path);
    n = fp.size();
  }
  state.counters["doubles"] = static_cast<double>(n);
}

// ── Profile (ATX_VOL_AL_PROFILE=1) ────────────────────────────────────────────

// The bare preset loop, no accuracy pass, so alprobe zones (ATX_VOL_AL_PROBE=z)
// accumulate only the priced grid. Run one preset per process with
// --benchmark_filter to keep the zone tables separate.
void run_profile(benchmark::State &state, const Preset &preset) {
  const Grid &g = grid();
  double sink = 0.0;
  for (auto _ : state) {
    for (const GridPt &p : g.pts) {
      const atx::vol::Result<double> r =
          atx::vol::andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, preset.opts);
      sink += r.has_value() ? *r : 0.0;
    }
    benchmark::DoNotOptimize(sink);
  }
  const double ops = static_cast<double>(state.iterations()) * static_cast<double>(g.pts.size());
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  state.counters["us_per_op"] =
      benchmark::Counter(ops * 1e-6, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// BAW critical-price Newton census over the grid's cold seeds: the seed lays down
// one Newton per collocation node tau_i > 0 (al_seed_boundary), so this reproduces
// that node grid (al_init_nodes: x_i = sqrt(T)/2 (1 + z_i), tau_i = x_i^2 with
// Chebyshev-Lobatto z_i) on the McDonald-Schroder internal put and counts iterations
// through the baw_critical_solve seam (same Newton, same 16 / 1e-10 budget).
void run_seed_census(benchmark::State &state, unsigned nb) {
  const Grid &g = grid();
  double iters = 0.0;
  double nodes = 0.0;
  double retries = 0.0;
  double capped = 0.0;
  for (auto _ : state) {
    iters = nodes = retries = capped = 0.0;
    for (const GridPt &p : g.pts) {
      const bool put = p.side == Side::Put;
      const double Kp = put ? p.K : p.S;
      const double rp = put ? p.r : p.q;
      const double qp = put ? p.q : p.r;
      for (unsigned i = 1; i < nb; ++i) {
        const double z = -std::cos(3.14159265358979323846 * static_cast<double>(i) /
                                   static_cast<double>(nb - 1));
        const double zi = (i + 1 == nb) ? 1.0 : z;
        const double x = 0.5 * std::sqrt(p.T) * (1.0 + zi);
        const double tau = x * x;
        const atx::vol::detail::BawCriticalSolve s =
            atx::vol::detail::baw_critical_solve(Kp, tau, p.sigma, rp, qp, Side::Put, 16, 1.0e-10);
        iters += static_cast<double>(s.iters);
        retries += s.attempts > 1 ? 1.0 : 0.0;
        capped += s.converged ? 0.0 : 1.0;
        nodes += 1.0;
      }
    }
  }
  state.counters["seed_nodes"] = nodes;
  state.counters["newton_iters_per_node"] = nodes > 0.0 ? iters / nodes : 0.0;
  state.counters["newton_iters_per_solve"] =
      g.pts.empty() ? 0.0 : iters / static_cast<double>(g.pts.size());
  state.counters["bracket_retry_nodes"] = retries;
  state.counters["unconverged_nodes"] = capped;
}

// libm unit costs on this host (independent calls, throughput-bound like the
// quadrature loops). ns_per_call = real_time / 4096.
enum class Fn { Exp, Log, Sqrt, Erfc, Div, NormCdf, CodyNormCdf, CodyExp };
void run_libm(benchmark::State &state, Fn fn) {
  std::vector<double> x(4096);
  Stream rng(0x11b02026'10170004ULL);
  const bool cdf_arg = fn == Fn::Erfc || fn == Fn::NormCdf || fn == Fn::CodyNormCdf;
  const bool exp_arg = fn == Fn::Exp || fn == Fn::CodyExp;
  for (double &v : x) {
    v = cdf_arg ? rng.range(-6.0, 6.0)
                : (exp_arg ? rng.range(-8.0, 2.0) : rng.log_range(1.0e-4, 1.0e3));
  }
  double sink = 0.0;
  for (auto _ : state) {
    for (const double v : x) {
      switch (fn) {
      case Fn::Exp:
        sink += std::exp(v);
        break;
      case Fn::Log:
        sink += std::log(v);
        break;
      case Fn::Sqrt:
        sink += std::sqrt(v);
        break;
      case Fn::Erfc:
        sink += std::erfc(v);
        break;
      case Fn::Div:
        sink += 1.0 / v;
        break;
      case Fn::NormCdf:
        sink += atx::core::norm_cdf(v);
        break;
      case Fn::CodyNormCdf:
        sink += atx::vol::detail::norm_cdf_erfc(v);
        break;
      case Fn::CodyExp:
        sink += atx::vol::detail::exp_cody(v);
        break;
      }
    }
    benchmark::DoNotOptimize(sink);
  }
  const double ops = static_cast<double>(state.iterations()) * static_cast<double>(x.size());
  state.counters["ns_per_call"] =
      benchmark::Counter(ops * 1e-9, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// ── Greeks latency (american/greeks_latency/<preset>) ─────────────────────────
// Single-op american_greeks_al (the full bundle: 5 boundary solves) on the grid,
// timed exactly like run_latency.
void run_greeks_latency(benchmark::State &state, const Preset &preset) {
  const Grid &g = grid();
  constexpr std::size_t kCap = std::size_t{1} << 16;
  std::vector<double> samples_us;
  samples_us.reserve(kCap);
  double sink = 0.0;
  for (auto _ : state) {
    double total_us = 0.0;
    for (const GridPt &p : g.pts) {
      const auto t0 = std::chrono::steady_clock::now();
      const atx::vol::Result<atx::vol::AmericanGreeks> r =
          atx::vol::american_greeks_al(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, preset.opts);
      const auto t1 = std::chrono::steady_clock::now();
      sink += r.has_value() ? r->delta : 0.0;
      const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      total_us += us;
      if (samples_us.size() < kCap) {
        samples_us.push_back(us);
      }
    }
    benchmark::DoNotOptimize(sink);
    state.SetIterationTime(total_us * 1.0e-6);
  }
  std::sort(samples_us.begin(), samples_us.end());
  double mean = 0.0;
  for (const double v : samples_us) {
    mean += v;
  }
  mean = samples_us.empty() ? 0.0 : mean / static_cast<double>(samples_us.size());
  state.counters["samples"] = static_cast<double>(samples_us.size());
  state.counters["p50_us"] = sorted_quantile(samples_us, 0.50);
  state.counters["p90_us"] = sorted_quantile(samples_us, 0.90);
  state.counters["p99_us"] = sorted_quantile(samples_us, 0.99);
  state.counters["mean_us"] = mean;
}

// ── Fast-profile error (ATX_VOL_AL_FAST_ERR=1) ────────────────────────────────
//
// american/fast_profile_error/<set>/<preset> prices every point of <set> on the
// Reference preset and on al_fast_profile(preset) — the SAME scheme, only the
// kernel arithmetic differs — and reports the distribution of the difference:
//   price_*      |P_fast - P_ref| (currency units of the set's spot) and, on prices
//                >= 0.01, |P_fast - P_ref| / P_ref;
//   iv_*_bp      |IV_fast - IV_ref| * 1e4 where both invert the SAME target (the
//                Reference price) with american_implied_vol (tol 1e-10, 100 iter);
//   *_mismatch   points where exactly one profile returned an error.
// american/fast_profile_error/greeks/<set>/<preset> does the same for every field
// of american_greeks_al (p99 is the comparison statistic; max is reported but a
// finite-difference greek's max is dominated by its worst stencil).
// Sets: grid (the ladder grid), random (the 6,000-contract fingerprint set) and,
// when ATX_VOL_AL_DEV200=<csv> names a kernel-input file (header line, then
// S,K,T,sigma,r,q,is_call per row), dev200.
class ErrDist {
public:
  void add(double e) { v_.push_back(e); }
  [[nodiscard]] double max() const {
    return v_.empty() ? 0.0 : *std::max_element(v_.begin(), v_.end());
  }
  [[nodiscard]] double quantile(double q) const {
    std::vector<double> s = v_;
    std::sort(s.begin(), s.end());
    return sorted_quantile(s, q);
  }
  [[nodiscard]] std::size_t size() const noexcept { return v_.size(); }

private:
  std::vector<double> v_;
};

[[nodiscard]] std::vector<GridPt> load_kernel_inputs(const std::string &path) {
  std::vector<GridPt> v;
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) {
    return v; // empty or missing file
  }
  while (std::getline(in, line)) {
    double f[7] = {};
    const char *c = line.c_str();
    bool ok = true;
    for (int k = 0; k < 7 && ok; ++k) {
      char *end = nullptr;
      f[k] = std::strtod(c, &end);
      ok = end != c;
      c = (*end == ',') ? end + 1 : end;
    }
    if (ok) {
      v.push_back(GridPt{f[0], f[1], f[2], f[3], f[4], f[5], f[6] != 0.0 ? Side::Call : Side::Put});
    }
  }
  return v;
}

// L2f al-fast-tier (2026-10-19): the SPY 23 d chain (cohort5 SPY 2026-11-06, the L1
// attribution's target chain). The file holds one quote per line after a header:
// S,K,T,r,q,is_call,bid,ask. Each mid is inverted ONCE here, outside any timing, on the
// accurate Reference kernel (tol 1e-10, 100 iterations) to give the contract a sigma;
// a quote whose mid does not invert is dropped.
[[nodiscard]] std::vector<GridPt> load_quote_inputs(const std::string &path) {
  std::vector<GridPt> v;
  std::ifstream in(path);
  std::string line;
  if (!std::getline(in, line)) {
    return v; // empty or missing file
  }
  while (std::getline(in, line)) {
    double f[8] = {};
    const char *c = line.c_str();
    bool ok = true;
    for (int k = 0; k < 8 && ok; ++k) {
      char *end = nullptr;
      f[k] = std::strtod(c, &end);
      ok = end != c;
      c = (*end == ',') ? end + 1 : end;
    }
    if (!ok) {
      continue;
    }
    const Side side = f[5] != 0.0 ? Side::Call : Side::Put;
    const double mid = 0.5 * (f[6] + f[7]);
    const atx::vol::Result<double> iv = atx::vol::american_implied_vol(
        mid, f[0], f[1], f[2], f[3], f[4], side, atx::vol::AmericanMethod::AndersenLake, 1.0e-10,
        100, std::nullopt);
    if (iv.has_value() && std::isfinite(*iv) && *iv > 0.0) {
      v.push_back(GridPt{f[0], f[1], f[2], *iv, f[3], f[4], side});
    }
  }
  return v;
}

enum class ErrSet { Grid, Random, Dev200, Spy23 };

[[nodiscard]] const std::vector<GridPt> &err_points(ErrSet s) {
  static const std::vector<GridPt> random = fingerprint_contracts(6000);
  static const std::vector<GridPt> dev200 = load_kernel_inputs(env_text("ATX_VOL_AL_DEV200"));
  static const std::vector<GridPt> spy23 = load_quote_inputs(env_text("ATX_VOL_AL_SPY23"));
  switch (s) {
  case ErrSet::Grid:
    return grid().pts;
  case ErrSet::Random:
    return random;
  case ErrSet::Spy23:
    return spy23;
  case ErrSet::Dev200:
    break;
  }
  return dev200;
}

void run_fast_error(benchmark::State &state, ErrSet set, const Preset &preset) {
  using namespace atx::vol;
  const std::vector<GridPt> &pts = err_points(set);
  const std::optional<AlOpts> ref = preset.opts;
  const std::optional<AlOpts> fast = al_fast_profile(preset.opts);
  ErrDist p_abs, p_rel, iv_bp;
  double mismatch = 0.0;
  double iv_mismatch = 0.0;
  for (auto _ : state) {
    for (const GridPt &p : pts) {
      const Result<double> a = andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, ref);
      const Result<double> b = andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, fast);
      if (a.has_value() != b.has_value()) {
        mismatch += 1.0;
        continue;
      }
      if (!a.has_value()) {
        continue;
      }
      const double d = std::abs(*b - *a);
      p_abs.add(d);
      if (*a >= 0.01) {
        p_rel.add(d / *a);
      }
      const Result<double> iva = american_implied_vol(*a, p.S, p.K, p.T, p.r, p.q, p.side,
                                                      AmericanMethod::AndersenLake, 1.0e-10, 100,
                                                      ref);
      const Result<double> ivb = american_implied_vol(*a, p.S, p.K, p.T, p.r, p.q, p.side,
                                                      AmericanMethod::AndersenLake, 1.0e-10, 100,
                                                      fast);
      if (iva.has_value() != ivb.has_value()) {
        iv_mismatch += 1.0;
      } else if (iva.has_value()) {
        iv_bp.add(std::abs(*ivb - *iva) * 1.0e4);
      }
    }
  }
  state.counters["points"] = static_cast<double>(pts.size());
  state.counters["priced"] = static_cast<double>(p_abs.size());
  state.counters["status_mismatch"] = mismatch;
  state.counters["price_max_abs"] = p_abs.max();
  state.counters["price_p99_abs"] = p_abs.quantile(0.99);
  state.counters["price_p50_abs"] = p_abs.quantile(0.50);
  state.counters["price_max_rel"] = p_rel.max();
  state.counters["iv_solved"] = static_cast<double>(iv_bp.size());
  state.counters["iv_mismatch"] = iv_mismatch;
  state.counters["iv_max_bp"] = iv_bp.max();
  state.counters["iv_p99_bp"] = iv_bp.quantile(0.99);
}

void run_fast_greeks_error(benchmark::State &state, ErrSet set, const Preset &preset) {
  using namespace atx::vol;
  const std::vector<GridPt> &pts = err_points(set);
  const std::optional<AlOpts> ref = preset.opts;
  const std::optional<AlOpts> fast = al_fast_profile(preset.opts);
  constexpr const char *kNames[] = {"price", "delta", "gamma", "vega", "theta",
                                    "rho",   "vanna", "volga", "charm"};
  ErrDist dist[9];
  double mismatch = 0.0;
  for (auto _ : state) {
    for (const GridPt &p : pts) {
      const Result<AmericanGreeks> a =
          american_greeks_al(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, ref);
      const Result<AmericanGreeks> b =
          american_greeks_al(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, fast);
      if (a.has_value() != b.has_value()) {
        mismatch += 1.0;
        continue;
      }
      if (!a.has_value()) {
        continue;
      }
      const double va[9] = {a->price, a->delta, a->gamma, a->vega, a->theta,
                            a->rho,   a->vanna, a->volga, a->charm};
      const double vb[9] = {b->price, b->delta, b->gamma, b->vega, b->theta,
                            b->rho,   b->vanna, b->volga, b->charm};
      for (int k = 0; k < 9; ++k) {
        dist[k].add(std::abs(vb[k] - va[k]));
      }
    }
  }
  state.counters["points"] = static_cast<double>(pts.size());
  state.counters["status_mismatch"] = mismatch;
  for (int k = 0; k < 9; ++k) {
    state.counters[std::string(kNames[k]) + "_p99"] = dist[k].quantile(0.99);
    state.counters[std::string(kNames[k]) + "_max"] = dist[k].max();
  }
}

// ── L2f al-fast-tier (2026-10-19): SPY 23 d rows and the Screen scan ──────────
//
//   * ATX_VOL_AL_SPY23=<csv> (load_quote_inputs) registers, per preset,
//     american/spy23_latency/<preset> (run_latency_on over the chain),
//     american/spy23_profile/<preset> (the bare loop, for alprobe zones under
//     ATX_VOL_AL_PROBE=z) and american/spy23_iv/<preset> (one american_implied_vol of
//     the contract's Reference price per op on the preset's own kernel, tol 1e-10,
//     100 iterations: the unit the fit's de-Am inversions pay).
//   * ATX_VOL_AL_SCREEN_SCAN=1 registers american/screen_scan/<set>/<candidate> over
//     grid, random, and spy23 / dev200 when their files are named: |P - P_ref| against
//     the accurate Reference (nullopt) — max abs, p99 abs, max relative on P_ref >=
//     0.01 — the worst contract's inputs (stderr), the IV error of the candidate's
//     inversion of P_ref, and the candidate's single-op p50 / mean latency.

void run_profile_on(benchmark::State &state, const std::vector<GridPt> &pts,
                    const Preset &preset) {
  if (pts.empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  double sink = 0.0;
  for (auto _ : state) {
    for (const GridPt &p : pts) {
      const atx::vol::Result<double> r =
          atx::vol::andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, preset.opts);
      sink += r.has_value() ? *r : 0.0;
    }
    benchmark::DoNotOptimize(sink);
  }
  const double ops = static_cast<double>(state.iterations()) * static_cast<double>(pts.size());
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  state.counters["us_per_op"] =
      benchmark::Counter(ops * 1e-6, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// One inversion per op: the Reference price of each contract, inverted on the preset.
void run_iv_on(benchmark::State &state, const std::vector<GridPt> &pts, const Preset &preset) {
  if (pts.empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  std::vector<double> target(pts.size());
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const GridPt &p = pts[i];
    const atx::vol::Result<double> r =
        atx::vol::andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, std::nullopt);
    target[i] = r.has_value() ? *r : std::numeric_limits<double>::quiet_NaN();
  }
  std::vector<double> samples_us;
  double sink = 0.0;
  double failed = 0.0;
  for (auto _ : state) {
    double total_us = 0.0;
    failed = 0.0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
      const GridPt &p = pts[i];
      const auto t0 = std::chrono::steady_clock::now();
      const atx::vol::Result<double> iv = atx::vol::american_implied_vol(
          target[i], p.S, p.K, p.T, p.r, p.q, p.side, atx::vol::AmericanMethod::AndersenLake,
          1.0e-10, 100, preset.opts);
      const auto t1 = std::chrono::steady_clock::now();
      sink += iv.has_value() ? *iv : 0.0;
      failed += iv.has_value() ? 0.0 : 1.0;
      const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      total_us += us;
      samples_us.push_back(us);
    }
    benchmark::DoNotOptimize(sink);
    state.SetIterationTime(total_us * 1.0e-6);
  }
  std::sort(samples_us.begin(), samples_us.end());
  double mean = 0.0;
  for (const double v : samples_us) {
    mean += v;
  }
  mean = samples_us.empty() ? 0.0 : mean / static_cast<double>(samples_us.size());
  state.counters["grid"] = static_cast<double>(pts.size());
  state.counters["failed"] = failed;
  state.counters["p50_us"] = sorted_quantile(samples_us, 0.50);
  state.counters["p99_us"] = sorted_quantile(samples_us, 0.99);
  state.counters["mean_us"] = mean;
}

struct ScreenCandidate {
  const char *name;
  AlOpts opts;
};

// (n_boundary, fixed-point order l, sweeps m, premium order p) on the Screen profile.
// m maps to Jacobi-Newton first (min(2, m)), then fixed-point sweeps.
[[nodiscard]] const std::vector<ScreenCandidate> &screen_candidates() {
  using atx::vol::AlProfile;
  static const std::vector<ScreenCandidate> c = [] {
    std::vector<ScreenCandidate> v;
    struct K {
      const char *name;
      std::uint16_t nb, l, m, p;
      double tol;
    };
    // Round 1 (scan-wip1/2 in the lane folder) ranged nb 4-9, l 8/16, m 1-5, p 16-32;
    // round 2 (scan-wip3) the nb 6-9 / m 4-6 / p 32-48 neighbourhood; round 3 (this
    // list) the chosen (7, 16, 5, 48) point, its neighbours and its early-exit tol.
    constexpr K kCands[] = {
        {"nb5_l8_m1_p16", 5, 8, 1, 16, 1e-8},          {"nb7_l8_m5_p48", 7, 8, 5, 48, 1e-8},
        {"nb7_l16_m4_p48", 7, 16, 4, 48, 1e-8},        {"nb7_l16_m5_p48", 7, 16, 5, 48, 1e-8},
        {"nb7_l16_m5_p48_tol1e-7", 7, 16, 5, 48, 1e-7}, {"nb7_l16_m5_p48_tol1e-6", 7, 16, 5, 48, 1e-6},
        {"nb7_l16_m6_p48", 7, 16, 6, 48, 1e-8},        {"nb6_l16_m5_p48", 6, 16, 5, 48, 1e-8},
    };
    for (const K &k : kCands) {
      v.push_back(ScreenCandidate{k.name, AlOpts{.n_collocation = k.nb,
                                                 .n_quadrature = k.l,
                                                 .n_quad_price = k.p,
                                                 .max_newton_iter = k.m,
                                                 .tol = k.tol,
                                                 .profile = AlProfile::Screen}});
    }
    v.push_back(ScreenCandidate{"al_screen_opts", atx::vol::al_screen_opts()});
    return v;
  }();
  return c;
}

// Per set, computed once and shared by every candidate row: the accurate Reference
// price, the Reference inversion of that price (tol 1e-10, 100 iterations), and whether
// the contract is vega-bearing on the American map: a central sigma bump of +/- 1e-4
// moves the Reference price by at least 1e-2 * max(P, 1) per unit vol. A deep-ITM price
// pinned at intrinsic (the exercise region) inverts to a band of vols under ANY kernel,
// so its IV difference measures the target, not the kernel.
struct ScanRef {
  std::vector<double> price;
  std::vector<double> iv;
  std::vector<bool> vega_bearing;
};

[[nodiscard]] const ScanRef &scan_ref(ErrSet set) {
  using namespace atx::vol;
  static ScanRef cache[4];
  static bool ready[4] = {false, false, false, false};
  const auto k = static_cast<std::size_t>(set);
  if (!ready[k]) {
    const std::vector<GridPt> &pts = err_points(set);
    ScanRef &r = cache[k];
    const double nan = std::numeric_limits<double>::quiet_NaN();
    r.price.assign(pts.size(), nan);
    r.iv.assign(pts.size(), nan);
    r.vega_bearing.assign(pts.size(), false);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      const GridPt &p = pts[i];
      const Result<double> a = andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, std::nullopt);
      if (!a.has_value()) {
        continue;
      }
      r.price[i] = *a;
      const Result<double> up =
          andersen_lake(p.S, p.K, p.T, p.sigma + 1.0e-4, p.r, p.q, p.side, std::nullopt);
      const Result<double> dn =
          andersen_lake(p.S, p.K, p.T, p.sigma - 1.0e-4, p.r, p.q, p.side, std::nullopt);
      if (up.has_value() && dn.has_value()) {
        r.vega_bearing[i] = (*up - *dn) / 2.0e-4 >= 1.0e-2 * std::max(*a, 1.0);
      }
      const Result<double> iv = american_implied_vol(*a, p.S, p.K, p.T, p.r, p.q, p.side,
                                                     AmericanMethod::AndersenLake, 1.0e-10, 100,
                                                     std::nullopt);
      r.iv[i] = iv.has_value() ? *iv : nan;
    }
    ready[k] = true;
  }
  return cache[k];
}

void run_screen_scan(benchmark::State &state, ErrSet set, const char *set_name,
                     const ScreenCandidate &cand) {
  using namespace atx::vol;
  const std::vector<GridPt> &pts = err_points(set);
  if (pts.empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  const ScanRef &sr = scan_ref(set);
  ErrDist p_abs, p_rel, iv_bp;
  double mismatch = 0.0;
  double iv_mismatch = 0.0;
  double worst_abs = -1.0;
  double worst_rel = -1.0;
  std::size_t worst_abs_i = 0;
  std::size_t worst_rel_i = 0;
  std::vector<double> ref(pts.size());
  std::vector<double> got(pts.size());
  std::vector<double> samples_us;
  samples_us.reserve(3 * pts.size());
  for (auto _ : state) {
    for (int pass = 0; pass < 3; ++pass) {
      for (std::size_t i = 0; i < pts.size(); ++i) {
        const GridPt &p = pts[i];
        const auto t0 = std::chrono::steady_clock::now();
        const Result<double> b = andersen_lake(p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, cand.opts);
        const auto t1 = std::chrono::steady_clock::now();
        got[i] = b.has_value() ? *b : std::numeric_limits<double>::quiet_NaN();
        samples_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
      }
    }
    for (std::size_t i = 0; i < pts.size(); ++i) {
      const GridPt &p = pts[i];
      ref[i] = sr.price[i];
      if (std::isfinite(ref[i]) != std::isfinite(got[i])) {
        mismatch += 1.0;
        continue;
      }
      if (!std::isfinite(ref[i])) {
        continue;
      }
      const double d = std::abs(got[i] - ref[i]);
      p_abs.add(d);
      if (d > worst_abs) {
        worst_abs = d;
        worst_abs_i = i;
      }
      if (ref[i] >= 0.01) {
        p_rel.add(d / ref[i]);
        if (d / ref[i] > worst_rel) {
          worst_rel = d / ref[i];
          worst_rel_i = i;
        }
      }
      // IV: the candidate's and the Reference's inversions of the SAME target, on
      // vega-bearing contracts (ScanRef).
      if (!sr.vega_bearing[i] || !std::isfinite(sr.iv[i])) {
        continue;
      }
      const Result<double> iva = american_implied_vol(ref[i], p.S, p.K, p.T, p.r, p.q, p.side,
                                                      AmericanMethod::AndersenLake, 1.0e-10, 100,
                                                      cand.opts);
      if (!iva.has_value()) {
        iv_mismatch += 1.0;
      } else {
        iv_bp.add(std::abs(*iva - sr.iv[i]) * 1.0e4);
      }
    }
  }
  std::sort(samples_us.begin(), samples_us.end());
  double mean = 0.0;
  for (const double v : samples_us) {
    mean += v;
  }
  mean = samples_us.empty() ? 0.0 : mean / static_cast<double>(samples_us.size());
  state.counters["points"] = static_cast<double>(pts.size());
  state.counters["status_mismatch"] = mismatch;
  state.counters["price_max_abs"] = p_abs.max();
  state.counters["price_p99_abs"] = p_abs.quantile(0.99);
  state.counters["price_max_rel"] = p_rel.max();
  state.counters["price_p99_rel"] = p_rel.quantile(0.99);
  state.counters["iv_failed"] = iv_mismatch;
  state.counters["iv_p50_bp"] = iv_bp.quantile(0.50);
  state.counters["iv_p99_bp"] = iv_bp.quantile(0.99);
  state.counters["iv_max_bp"] = iv_bp.max();
  state.counters["p50_us"] = sorted_quantile(samples_us, 0.50);
  state.counters["mean_us"] = mean;
  const auto describe = [&](const char *what, std::size_t i, double e) {
    const GridPt &p = pts[i];
    std::fprintf(stderr,
                 "[screen-scan] %s/%s %s=%.6g S=%.17g K=%.17g T=%.17g sigma=%.17g r=%.17g "
                 "q=%.17g side=%s ref=%.17g got=%.17g\n",
                 set_name, cand.name, what, e, p.S, p.K, p.T, p.sigma, p.r, p.q,
                 p.side == Side::Call ? "C" : "P", ref[i], got[i]);
  };
  if (worst_abs >= 0.0) {
    describe("worst_abs", worst_abs_i, worst_abs);
  }
  if (worst_rel >= 0.0) {
    describe("worst_rel", worst_rel_i, worst_rel);
  }
}

// ── L2h sigma-family (2026-10-19): AlSigmaFamily build / price / chord / error rows ──
//
//   * ATX_VOL_AL_FAMILY=1 registers, for n_sigma in {8, 12, 16} and both axes
//     (sigma, logsigma):
//       american/spy23_family/<axis>/n<N>            (needs ATX_VOL_AL_SPY23)
//       american/family_error/<set>/<axis>/n<N>      sets: grid, fastgrid, deepcarry,
//                                                    dev200 (needs ATX_VOL_AL_DEV200),
//                                                    spy23offnode (ATX_VOL_AL_SPY23)
//     Each row groups the set's contracts by (S, T, r, q, side) — one expiry side —
//     builds ONE family per group on a Reference geometry over the group's sigma box
//     (0.95 * min .. 1.05 * max, widened to >= 0.02 and kAlPackSigmaBump of chord
//     headroom; L2i's recommended rule), times the build (median of 5 repetitions per
//     group, reported per family), then prices every contract through the family and
//     reports: build_us (per family), price p50 / p99 us and chord p50 us per contract
//     (3 timed passes), |P_family - P_ref| vs the accurate cold Reference (max abs,
//     p99 abs, max rel and p99 rel on P_ref >= 0.01), the IV error of a safeguarded
//     Newton on the family's own chord vs the Reference inversion of the same target
//     (bp, vega-bearing contracts only), refusals (contracts the family would not
//     price: below the guards / outside the box / a non-American side) and the worst
//     contracts' full inputs on stderr. The spy23offnode set replaces each SPY
//     contract's sigma by the midpoint of the two family nodes around it (the P2.5
//     off-node pattern on the real chain's strikes).

struct FamilyKey {
  double S, T, r, q;
  Side side;
  [[nodiscard]] bool operator<(const FamilyKey &o) const noexcept {
    if (S != o.S) {
      return S < o.S;
    }
    if (T != o.T) {
      return T < o.T;
    }
    if (r != o.r) {
      return r < o.r;
    }
    if (q != o.q) {
      return q < o.q;
    }
    return static_cast<int>(side) < static_cast<int>(o.side);
  }
};

enum class FamilySet { Grid, FastGrid, DeepCarry, Dev200, Spy23, Spy23OffNode };

// The AlFastProfile 1,350-contract grid (tests/american_test.cpp fast_profile_grid).
[[nodiscard]] std::vector<GridPt> fast_profile_grid_pts() {
  std::vector<GridPt> v;
  for (const Side side : {Side::Put, Side::Call}) {
    for (const double T : {0.02, 0.1, 0.5, 1.5, 4.0}) {
      for (const double sigma : {0.08, 0.25, 0.7}) {
        for (const double r : {0.005, 0.045, 0.1}) {
          for (const double q : {0.0, 0.02, 0.09}) {
            for (const double m : {0.7, 0.9, 1.0, 1.1, 1.4}) {
              v.push_back(GridPt{100.0, 100.0 * m, T, sigma, r, q, side});
            }
          }
        }
      }
    }
  }
  return v;
}

// Screen's worst corner: long-dated, low-vol, deep-carry at-the-money puts.
[[nodiscard]] std::vector<GridPt> deep_carry_pts() {
  std::vector<GridPt> v;
  for (const double T : {2.0, 3.0, 4.0, 5.0}) {
    for (const double sigma : {0.05, 0.06, 0.07, 0.08, 0.09, 0.1, 0.11, 0.12}) {
      for (const double r : {0.05, 0.06, 0.075, 0.09, 0.1}) {
        for (const double q : {0.0, 0.01, 0.02, 0.03}) {
          v.push_back(GridPt{100.0, 100.0, T, sigma, r, q, Side::Put});
        }
      }
    }
  }
  return v;
}

[[nodiscard]] const std::vector<GridPt> &family_points(FamilySet s) {
  static const std::vector<GridPt> fastgrid = fast_profile_grid_pts();
  static const std::vector<GridPt> deep = deep_carry_pts();
  switch (s) {
  case FamilySet::Grid:
    return grid().pts;
  case FamilySet::FastGrid:
    return fastgrid;
  case FamilySet::DeepCarry:
    return deep;
  case FamilySet::Dev200:
    return err_points(ErrSet::Dev200);
  case FamilySet::Spy23:
  case FamilySet::Spy23OffNode:
    break;
  }
  return err_points(ErrSet::Spy23);
}

// The family's sigma nodes on [lo, hi] (Chebyshev-Lobatto; the family's own form).
[[nodiscard]] std::vector<double> family_nodes(double lo, double hi, unsigned n,
                                               atx::vol::AlSigmaFamilyAxis axis) {
  const bool logax = axis == atx::vol::AlSigmaFamilyAxis::LogSigma;
  const double ulo = logax ? std::log(lo) : lo;
  const double uhi = logax ? std::log(hi) : hi;
  std::vector<double> v;
  for (unsigned s = 0; s < n; ++s) {
    double z = -std::cos(3.14159265358979323846 * static_cast<double>(s) /
                         static_cast<double>(n - 1));
    if (s == 0) {
      z = -1.0;
    }
    if (s == n - 1) {
      z = 1.0;
    }
    const double u = ulo + 0.5 * (uhi - ulo) * (z + 1.0);
    v.push_back(logax ? std::exp(u) : u);
  }
  return v;
}

// L2i's recommended box rule over a group's admitted sigmas.
struct FamilyBox {
  double lo;
  double hi;
};
[[nodiscard]] FamilyBox family_box(double smin, double smax) noexcept {
  double lo = std::max(atx::vol::kAlSigmaFamilyMinSigma, 0.95 * smin);
  double hi = 1.05 * smax + 2.0 * atx::vol::kAlPackSigmaBump;
  if (hi - lo < 0.02) {
    const double mid = 0.5 * (lo + hi);
    lo = std::max(atx::vol::kAlSigmaFamilyMinSigma, mid - 0.01);
    hi = mid + 0.01;
  }
  return FamilyBox{lo, hi};
}

// Safeguarded Newton on the family's chord (the inversion an L2i caller iterates).
[[nodiscard]] double family_invert(const atx::vol::AlSigmaFamily &f, double S, double K,
                                   double target, double lo, double hi) {
  using atx::vol::AlSigmaFamilyChord;
  using atx::vol::AlSigmaFamilyStatus;
  double a = lo;
  double b = hi - 2.0 * atx::vol::kAlPackSigmaBump;
  double x = 0.5 * (a + b);
  for (int it = 0; it < 60; ++it) {
    const AlSigmaFamilyChord c = f.price_and_chord(S, K, x);
    if (c.status != AlSigmaFamilyStatus::Ok) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    const double fx = c.price - target;
    if (std::abs(fx) <= 1.0e-13 * std::max(target, 1.0)) {
      return x;
    }
    if (fx > 0.0) {
      b = x;
    } else {
      a = x;
    }
    const double slope = (c.price_sigma_bumped - c.price) / atx::vol::kAlPackSigmaBump;
    double nx = (slope > 0.0) ? x - fx / slope : 0.5 * (a + b);
    if (!(nx > a && nx < b)) {
      nx = 0.5 * (a + b);
    }
    if (std::abs(nx - x) <= 1.0e-12) {
      return nx;
    }
    x = nx;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

void run_family(benchmark::State &state, FamilySet set, const char *set_name, unsigned n_sigma,
                atx::vol::AlSigmaFamilyAxis axis) {
  using namespace atx::vol;
  const std::vector<GridPt> &pts = family_points(set);
  if (pts.empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  // Group by (S, T, r, q, side) — one geometry each — but take the sigma BOX from the
  // whole (S, T, side) chain: the quote files carry a per-row sdiv (the SPY 23 d
  // file has 74 distinct q over 322 rows), which would otherwise split a chain into
  // narrow-box families and flatter the interpolation. Every family of a chain
  // therefore spans the chain's full admitted sigma range, as L2i's one family per
  // (expiry, side) would.
  std::map<FamilyKey, std::vector<std::size_t>> groups;
  std::map<FamilyKey, std::pair<double, double>> chain_range; // (S, T, side) -> (min, max)
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const GridPt &p = pts[i];
    groups[FamilyKey{p.S, p.T, p.r, p.q, p.side}].push_back(i);
    if (p.sigma >= atx::vol::kAlSigmaFamilyMinSigma) {
      auto [it, fresh] = chain_range.try_emplace(FamilyKey{p.S, p.T, 0.0, 0.0, p.side},
                                                 std::pair<double, double>{p.sigma, p.sigma});
      if (!fresh) {
        it->second.first = std::min(it->second.first, p.sigma);
        it->second.second = std::max(it->second.second, p.sigma);
      }
    }
  }
  const auto group_box = [&](const FamilyKey &key) -> std::optional<FamilyBox> {
    const auto it = chain_range.find(FamilyKey{key.S, key.T, 0.0, 0.0, key.side});
    if (it == chain_range.end()) {
      return std::nullopt;
    }
    return family_box(it->second.first, it->second.second);
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> query_sigma(pts.size(), nan); // the sigma actually queried
  std::vector<double> ref_price(pts.size(), nan);
  std::vector<double> got(pts.size(), nan);
  std::vector<double> build_us;
  std::vector<double> price_us;
  std::vector<double> chord_us;
  double refused = 0.0;
  double families = 0.0;
  double cold_groups = 0.0;
  for (auto _ : state) {
    build_us.clear();
    price_us.clear();
    chord_us.clear();
    refused = 0.0;
    families = 0.0;
    cold_groups = 0.0;
    for (const auto &[key, idx] : groups) {
      const std::optional<FamilyBox> box_opt = group_box(key);
      if (!box_opt) {
        refused += static_cast<double>(idx.size());
        continue;
      }
      const FamilyBox box = *box_opt;
      const AlExpiryGeometry g = prepare_al_geometry(key.T, key.r, key.q);
      const double anchor = key.S;
      AlSigmaFamily fam(g, key.side, anchor);
      // Build: median of 5 repetitions.
      std::vector<double> reps;
      bool built = false;
      for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = fam.prepare(box.lo, box.hi, static_cast<std::uint16_t>(n_sigma), axis);
        const auto t1 = std::chrono::steady_clock::now();
        built = st.has_value();
        if (!built) {
          break;
        }
        reps.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
      }
      if (!built) {
        cold_groups += 1.0; // the whole expiry side prices cold (guards / regime)
        refused += static_cast<double>(idx.size());
        continue;
      }
      std::sort(reps.begin(), reps.end());
      build_us.push_back(reps[reps.size() / 2]);
      families += 1.0;
      const std::vector<double> nodes = family_nodes(box.lo, box.hi, n_sigma, axis);
      for (const std::size_t i : idx) {
        const GridPt &p = pts[i];
        double sigma = p.sigma;
        if (set == FamilySet::Spy23OffNode) {
          // The midpoint of the two nodes around the contract's own sigma.
          for (std::size_t k = 1; k < nodes.size(); ++k) {
            if (sigma <= nodes[k] || k + 1 == nodes.size()) {
              sigma = 0.5 * (nodes[k - 1] + nodes[k]);
              break;
            }
          }
        }
        query_sigma[i] = sigma;
        const Result<double> a = andersen_lake(g, p.S, p.K, sigma, p.side);
        ref_price[i] = a.has_value() ? *a : nan;
        AlSigmaFamilyPrice fp{};
        for (int pass = 0; pass < 3; ++pass) {
          const auto t0 = std::chrono::steady_clock::now();
          fp = fam.price(p.S, p.K, sigma);
          const auto t1 = std::chrono::steady_clock::now();
          price_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        for (int pass = 0; pass < 3; ++pass) {
          const auto t0 = std::chrono::steady_clock::now();
          AlSigmaFamilyChord c = fam.price_and_chord(p.S, p.K, sigma);
          const auto t1 = std::chrono::steady_clock::now();
          benchmark::DoNotOptimize(c);
          chord_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        if (fp.status != AlSigmaFamilyStatus::Ok) {
          refused += 1.0;
          got[i] = nan;
          continue;
        }
        got[i] = fp.price;
      }
    }
  }
  // Error distributions vs the cold Reference at the queried sigma.
  ErrDist p_abs, p_rel, iv_bp;
  double worst_abs = -1.0;
  double worst_rel = -1.0;
  double worst_iv = -1.0;
  std::size_t worst_abs_i = 0;
  std::size_t worst_rel_i = 0;
  std::size_t worst_iv_i = 0;
  double iv_failed = 0.0;
  double priced = 0.0;
  std::vector<double> iv_family(pts.size(), nan);
  std::vector<double> iv_ref(pts.size(), nan);
  for (const auto &[key, idx] : groups) {
    // Rebuild the group's family once more (untimed) for the IV pass.
    const std::optional<FamilyBox> box_opt = group_box(key);
    if (!box_opt) {
      continue;
    }
    const FamilyBox box = *box_opt;
    const AlExpiryGeometry g = prepare_al_geometry(key.T, key.r, key.q);
    AlSigmaFamily fam(g, key.side, key.S);
    if (!fam.prepare(box.lo, box.hi, static_cast<std::uint16_t>(n_sigma), axis)) {
      continue;
    }
    for (const std::size_t i : idx) {
      const GridPt &p = pts[i];
      if (!std::isfinite(ref_price[i]) || !std::isfinite(got[i])) {
        continue;
      }
      priced += 1.0;
      const double d = std::abs(got[i] - ref_price[i]);
      p_abs.add(d);
      if (d > worst_abs) {
        worst_abs = d;
        worst_abs_i = i;
      }
      if (ref_price[i] >= 0.01) {
        p_rel.add(d / ref_price[i]);
        if (d / ref_price[i] > worst_rel) {
          worst_rel = d / ref_price[i];
          worst_rel_i = i;
        }
      }
      // IV on vega-bearing contracts: Newton on the family vs the Reference inversion
      // of the same Reference target.
      const double sigma = query_sigma[i];
      const Result<double> up = andersen_lake(g, p.S, p.K, sigma + 1.0e-4, p.side);
      const Result<double> dn = andersen_lake(g, p.S, p.K, sigma - 1.0e-4, p.side);
      if (!up.has_value() || !dn.has_value() ||
          !((*up - *dn) / 2.0e-4 >= 1.0e-2 * std::max(ref_price[i], 1.0))) {
        continue;
      }
      const Result<double> ivr =
          american_implied_vol(ref_price[i], p.S, p.K, p.T, p.r, p.q, p.side,
                               AmericanMethod::AndersenLake, 1.0e-10, 100, std::nullopt);
      if (!ivr.has_value()) {
        continue;
      }
      const double ivf = family_invert(fam, p.S, p.K, ref_price[i], box.lo, box.hi);
      if (!std::isfinite(ivf)) {
        iv_failed += 1.0;
        continue;
      }
      iv_ref[i] = *ivr;
      iv_family[i] = ivf;
      const double bp = std::abs(ivf - *ivr) * 1.0e4;
      iv_bp.add(bp);
      if (bp > worst_iv) {
        worst_iv = bp;
        worst_iv_i = i;
      }
    }
  }
  std::sort(price_us.begin(), price_us.end());
  std::sort(chord_us.begin(), chord_us.end());
  std::sort(build_us.begin(), build_us.end());
  state.counters["points"] = static_cast<double>(pts.size());
  state.counters["families"] = families;
  state.counters["cold_groups"] = cold_groups;
  state.counters["refused"] = refused;
  state.counters["priced"] = priced;
  state.counters["build_us_p50"] = sorted_quantile(build_us, 0.50);
  state.counters["build_us_max"] = build_us.empty() ? 0.0 : build_us.back();
  state.counters["price_us_p50"] = sorted_quantile(price_us, 0.50);
  state.counters["price_us_p99"] = sorted_quantile(price_us, 0.99);
  state.counters["chord_us_p50"] = sorted_quantile(chord_us, 0.50);
  state.counters["price_max_abs"] = p_abs.max();
  state.counters["price_p99_abs"] = p_abs.quantile(0.99);
  state.counters["price_max_rel"] = p_rel.max();
  state.counters["price_p99_rel"] = p_rel.quantile(0.99);
  state.counters["iv_solved"] = static_cast<double>(iv_bp.size());
  state.counters["iv_failed"] = iv_failed;
  state.counters["iv_p99_bp"] = iv_bp.quantile(0.99);
  state.counters["iv_max_bp"] = iv_bp.max();
  state.counters["state_bytes"] = static_cast<double>(AlSigmaFamily::state_bytes());
  const char *axis_name = axis == AlSigmaFamilyAxis::LogSigma ? "logsigma" : "sigma";
  const auto describe = [&](const char *what, std::size_t i, double e) {
    const GridPt &p = pts[i];
    std::fprintf(stderr,
                 "[family] %s/%s/n%u %s=%.6g S=%.17g K=%.17g T=%.17g sigma=%.17g r=%.17g "
                 "q=%.17g side=%s ref=%.17g got=%.17g iv_ref=%.17g iv_family=%.17g\n",
                 set_name, axis_name, n_sigma, what, e, p.S, p.K, p.T, query_sigma[i], p.r, p.q,
                 p.side == Side::Call ? "C" : "P", ref_price[i], got[i], iv_ref[i], iv_family[i]);
  };
  if (worst_abs >= 0.0) {
    describe("worst_abs", worst_abs_i, worst_abs);
  }
  if (worst_rel >= 0.0) {
    describe("worst_rel", worst_rel_i, worst_rel);
  }
  if (worst_iv >= 0.0) {
    describe("worst_iv_bp", worst_iv_i, worst_iv);
  }
}

// ── L3g family-greeks (2026-10-20): analytic Greeks on the family — µs + error rows ──
//
//   * ATX_VOL_AL_FAMILY_GREEKS=1 registers:
//       american/spy23_family_greeks/<mask>      (needs ATX_VOL_AL_SPY23) mask in
//           price | hedge (+Δ Γ vega θ) | eight | all (+charm) | price_and_vega | chord:
//           per-contract µs of family_greeks (p50 / p99 / mean, one timed pass per
//           contract) on the SPY 23 d chain, one family per (S, T, r, q, side) group as
//           the L2h rows build them; the `price` row's µs must equal the L2h price row
//           within noise (it IS price()).
//       american/spy23_family_greeks/reference_bundles (needs ATX_VOL_AL_SPY23):
//           american_greeks_al(g, …) accurate and al_fast_profile, and the adjoint
//           bundle (detail::american_greeks_adjoint, puts only), per contract.
//       american/family_greeks_error/<set>/n<N>  sets: spy23, spy23offnode (ATX_VOL_AL_SPY23),
//           grid, fastgrid, deepcarry; N = 12 (and 16 for fastgrid): every Greek of
//           family_greeks(all) against TWO references on the SAME Reference geometry —
//           (i) american_greeks_al(g, …), the cold bundle's bits, and (ii) a Richardson
//           reference: Δ / Γ from h = 1e-3 S and h/2 central stencils of the cold price
//           (one-sided on the continuation side when the central stencil would
//           straddle the exercise edge — LEDGER 2026-10-16), vega / volga from σ ± 2e-3
//           and ± 1e-3 cold re-solves, vanna from the mixed stencil at both steps, θ
//           from the PDE identity on the Richardson Δ / Γ, and rho / phi from a SECOND
//           FAMILY built at r ± 1e-4 / q ± 1e-4 (two builds each, central difference of
//           its prices: the total derivative of the budget-limited map). Per Greek:
//           max abs, p99 abs, max rel and p99 rel (on |ref| ≥ the stated floor), the
//           worst contract's inputs on stderr. rho / phi are reported in both frozen
//           conventions (frozen y = served; frozen B = no xmax term).
//       american/family_chain/<chain>/<mask>  chain in spy23 (317 rows, one put + one
//           call family at the chain's median q; ATX_VOL_AL_SPY23) | synthetic300 (S 773,
//           23 d, 300 strikes 540..1090 on the representative skew; puts below spot,
//           calls above): one single-thread family_chain_greeks call, ms per chain.
//       american/family_shock/<grid>  11x11 (±10 % spot, ±10 vol points) per contract
//           (the SPY ATM put) and per synthetic 300-chain: ms.
//   Every timing is one pass (order of magnitude first); the load line is the lane's.

struct GreekErr {
  ErrDist abs_, rel_;
  double worst_abs = -1.0, worst_rel = -1.0;
  std::size_t worst_abs_i = 0, worst_rel_i = 0;
  double worst_abs_fam = 0.0, worst_abs_ref = 0.0, worst_rel_fam = 0.0, worst_rel_ref = 0.0;
  void add(std::size_t i, double fam, double ref, double floor) {
    if (!std::isfinite(fam) || !std::isfinite(ref)) {
      return;
    }
    const double d = std::abs(fam - ref);
    abs_.add(d);
    if (d > worst_abs) {
      worst_abs = d;
      worst_abs_i = i;
      worst_abs_fam = fam;
      worst_abs_ref = ref;
    }
    if (std::abs(ref) >= floor) {
      const double r = d / std::abs(ref);
      rel_.add(r);
      if (r > worst_rel) {
        worst_rel = r;
        worst_rel_i = i;
        worst_rel_fam = fam;
        worst_rel_ref = ref;
      }
    }
  }
  void publish(benchmark::State &state, const std::string &name) const {
    state.counters[name + "_max_abs"] = abs_.max();
    state.counters[name + "_p99_abs"] = abs_.quantile(0.99);
    state.counters[name + "_max_rel"] = rel_.max();
    state.counters[name + "_p99_rel"] = rel_.quantile(0.99);
    state.counters[name + "_n"] = static_cast<double>(abs_.size());
  }
};

// Richardson reference on the COLD kernel (every price a fresh andersen_lake(g, …):
// the boundary re-solved at the same σ is bit-identical, so the spot stencils are the
// exact spot derivatives of the cold map; the σ stencils carry the re-solved boundary).
struct ColdRef {
  double delta = std::numeric_limits<double>::quiet_NaN();
  double gamma = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();
  double volga = std::numeric_limits<double>::quiet_NaN();
  double vanna = std::numeric_limits<double>::quiet_NaN();
  double theta = std::numeric_limits<double>::quiet_NaN();
  bool one_sided = false;
  bool exercised = false;
};

[[nodiscard]] ColdRef cold_richardson(const atx::vol::AlExpiryGeometry &g, const GridPt &p,
                                      double sigma) {
  using namespace atx::vol;
  ColdRef out;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const auto P = [&](double s, double sig) {
    const Result<double> r = andersen_lake(g, s, p.K, sig, p.side);
    return r.has_value() ? *r : nan;
  };
  const double P0 = P(p.S, sigma);
  const double intr = p.side == Side::Call ? p.S - p.K : p.K - p.S;
  if (intr > 0.0 && P0 <= intr + 1.0e-9 * p.K) {
    out.exercised = true;
    return out;
  }
  const auto clamped = [&](double s) {
    const double v = P(s, sigma);
    const double in = p.side == Side::Call ? s - p.K : p.K - s;
    return in > 0.0 && v <= in + 1.0e-9 * p.K;
  };
  const double h1 = 1.0e-3 * p.S;
  const bool low = clamped(p.S - 2.0 * h1);
  const bool high = clamped(p.S + 2.0 * h1);
  if (low && high) {
    return out;
  }
  // Δ / Γ at step h with a central, forward or backward stencil (all second order).
  const auto dg = [&](double h, double &d, double &gm) {
    if (!low && !high) {
      const double pp = P(p.S + h, sigma), pm = P(p.S - h, sigma);
      d = (pp - pm) / (2.0 * h);
      gm = (pp - 2.0 * P0 + pm) / (h * h);
      return;
    }
    const double dir = low ? 1.0 : -1.0; // step away from the exercise region
    const double p1 = P(p.S + dir * h, sigma), p2 = P(p.S + 2.0 * dir * h, sigma),
                 p3 = P(p.S + 3.0 * dir * h, sigma);
    d = dir * (-3.0 * P0 + 4.0 * p1 - p2) / (2.0 * h);
    gm = (2.0 * P0 - 5.0 * p1 + 4.0 * p2 - p3) / (h * h);
  };
  double d1, g1, d2, g2;
  dg(h1, d1, g1);
  dg(0.5 * h1, d2, g2);
  out.one_sided = low || high;
  out.delta = (4.0 * d2 - d1) / 3.0;
  out.gamma = (4.0 * g2 - g1) / 3.0;
  out.theta = p.r * P0 - (p.r - p.q) * p.S * out.delta -
              0.5 * sigma * sigma * p.S * p.S * out.gamma;
  const double hv1 = 2.0e-3;
  if (sigma - hv1 > 1.0e-4) {
    const auto vv = [&](double h, double &ve, double &vo) {
      const double pp = P(p.S, sigma + h), pm = P(p.S, sigma - h);
      ve = (pp - pm) / (2.0 * h);
      vo = (pp - 2.0 * P0 + pm) / (h * h);
    };
    double ve1, vo1, ve2, vo2;
    vv(hv1, ve1, vo1);
    vv(0.5 * hv1, ve2, vo2);
    out.vega = (4.0 * ve2 - ve1) / 3.0;
    out.volga = (4.0 * vo2 - vo1) / 3.0;
    if (!out.one_sided) {
      const auto vn = [&](double hs, double hsig) {
        return (P(p.S + hs, sigma + hsig) - P(p.S + hs, sigma - hsig) -
                P(p.S - hs, sigma + hsig) + P(p.S - hs, sigma - hsig)) /
               (4.0 * hs * hsig);
      };
      out.vanna = (4.0 * vn(0.5 * h1, 0.5 * hv1) - vn(h1, hv1)) / 3.0;
    }
  }
  return out;
}

void run_family_greeks_error(benchmark::State &state, FamilySet set, const char *set_name,
                             unsigned n_sigma) {
  using namespace atx::vol;
  using Mask = AlSigmaFamilyGreeksMask;
  const std::vector<GridPt> &pts = family_points(set);
  if (pts.empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  const AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma;
  std::map<FamilyKey, std::vector<std::size_t>> groups;
  std::map<FamilyKey, std::pair<double, double>> chain_range;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const GridPt &p = pts[i];
    groups[FamilyKey{p.S, p.T, p.r, p.q, p.side}].push_back(i);
    if (p.sigma >= kAlSigmaFamilyMinSigma) {
      auto [it, fresh] = chain_range.try_emplace(FamilyKey{p.S, p.T, 0.0, 0.0, p.side},
                                                 std::pair<double, double>{p.sigma, p.sigma});
      if (!fresh) {
        it->second.first = std::min(it->second.first, p.sigma);
        it->second.second = std::max(it->second.second, p.sigma);
      }
    }
  }
  GreekErr e_delta_b, e_gamma_b, e_vega_b, e_theta_b, e_vanna_b, e_volga_b, e_rho_b;
  GreekErr e_delta_r, e_gamma_r, e_vega_r, e_theta_r, e_vanna_r, e_volga_r;
  GreekErr e_rho_y, e_rho_bb, e_phi_y, e_phi_bb, e_rhoB_vs_fam2, e_phiB_vs_fam2;
  // The served (carry set) rho / phi vs the re-solved bundle, the cold q ± h central
  // difference and the second-family price difference; the frozen rho vs the bundle.
  GreekErr e_rhoC_b, e_rhoC_fam2, e_phiC_cold, e_phiC_fam2, e_rhoY_b;
  double refused = 0.0, priced = 0.0, exercised = 0.0, one_sided = 0.0, fam2_failed = 0.0;
  double carry_failed = 0.0;
  std::vector<double> query_sigma(pts.size(), std::numeric_limits<double>::quiet_NaN());
  for (auto _ : state) {
    for (const auto &[key, idx] : groups) {
      const auto it = chain_range.find(FamilyKey{key.S, key.T, 0.0, 0.0, key.side});
      if (it == chain_range.end()) {
        refused += static_cast<double>(idx.size());
        continue;
      }
      const FamilyBox box = family_box(it->second.first, it->second.second);
      const AlExpiryGeometry g = prepare_al_geometry(key.T, key.r, key.q);
      AlSigmaFamily fam(g, key.side, key.S);
      if (!fam.prepare(box.lo, box.hi, static_cast<std::uint16_t>(n_sigma), axis)) {
        refused += static_cast<double>(idx.size());
        continue;
      }
      // Second families at r ± h and q ± h (the total derivative of the served map).
      const double hr = 1.0e-4;
      const AlExpiryGeometry grp = prepare_al_geometry(key.T, key.r + hr, key.q);
      const AlExpiryGeometry grm = prepare_al_geometry(key.T, key.r - hr, key.q);
      const AlExpiryGeometry gqp = prepare_al_geometry(key.T, key.r, key.q + hr);
      const AlExpiryGeometry gqm = prepare_al_geometry(key.T, key.r, key.q - hr);
      AlSigmaFamily frp(grp, key.side, key.S), frm(grm, key.side, key.S);
      AlSigmaFamily fqp(gqp, key.side, key.S), fqm(gqm, key.side, key.S);
      const bool fam_r = frp.prepare(box.lo, box.hi, static_cast<std::uint16_t>(n_sigma), axis) &&
                         frm.prepare(box.lo, box.hi, static_cast<std::uint16_t>(n_sigma), axis);
      const bool fam_q = fqp.prepare(box.lo, box.hi, static_cast<std::uint16_t>(n_sigma), axis) &&
                         fqm.prepare(box.lo, box.hi, static_cast<std::uint16_t>(n_sigma), axis);
      // The served rho / phi (L3g resume r4): the carry set of the same family.
      AlSigmaFamilyCarryBumps carry;
      const bool have_carry = carry.prepare(fam, Mask{Mask::kRho | Mask::kPhi}).has_value();
      carry_failed += have_carry ? 0.0 : 1.0;
      const std::vector<double> nodes = family_nodes(box.lo, box.hi, n_sigma, axis);
      for (const std::size_t i : idx) {
        const GridPt &p = pts[i];
        double sigma = p.sigma;
        if (set == FamilySet::Spy23OffNode) {
          for (std::size_t k = 1; k < nodes.size(); ++k) {
            if (sigma <= nodes[k] || k + 1 == nodes.size()) {
              sigma = 0.5 * (nodes[k - 1] + nodes[k]);
              break;
            }
          }
        }
        query_sigma[i] = sigma;
        const AlSigmaFamilyGreeks a = have_carry
                                          ? family_greeks(fam, carry, p.S, p.K, sigma, Mask::all())
                                          : family_greeks(fam, p.S, p.K, sigma, Mask::all());
        if (a.status != AlSigmaFamilyStatus::Ok) {
          refused += 1.0;
          continue;
        }
        priced += 1.0;
        if (a.exercised) {
          exercised += 1.0;
          continue;
        }
        const double fp = 1.0e-3 * std::max(a.price, 1.0);
        const double fg = 1.0e-7 * p.S;
        // (i) the Reference bundle on the same geometry.
        const Result<AmericanGreeks> b = american_greeks_al(g, p.S, p.K, sigma, p.side);
        if (b.has_value()) {
          e_delta_b.add(i, a.delta, b->delta, 1.0e-3);
          e_gamma_b.add(i, a.gamma, b->gamma, fg);
          e_vega_b.add(i, a.vega, b->vega, fp);
          e_theta_b.add(i, a.theta, b->theta, fp);
          e_vanna_b.add(i, a.vanna, b->vanna, 1.0e-3);
          e_volga_b.add(i, a.volga, b->volga, fp);
          e_rhoC_b.add(i, a.rho, b->rho, fp); // NaN (skipped) when the carry set refused
        }
        // (ii) Richardson on the cold kernel.
        const ColdRef c = cold_richardson(g, p, sigma);
        if (!c.exercised) {
          one_sided += c.one_sided ? 1.0 : 0.0;
          e_delta_r.add(i, a.delta, c.delta, 1.0e-3);
          e_gamma_r.add(i, a.gamma, c.gamma, fg);
          e_theta_r.add(i, a.theta, c.theta, fp);
          e_vega_r.add(i, a.vega, c.vega, fp);
          e_volga_r.add(i, a.volga, c.volga, fp);
          e_vanna_r.add(i, a.vanna, c.vanna, 1.0e-3);
        }
        // rho / phi: both frozen conventions vs the r ± h / q ± h families.
        const sigma_family_detail::FamilyRhoVariants rv =
            sigma_family_detail::family_rho_variants(fam, p.S, p.K, sigma);
        if (fam_r) {
          const AlSigmaFamilyPrice up = frp.price(p.S, p.K, sigma);
          const AlSigmaFamilyPrice dn = frm.price(p.S, p.K, sigma);
          if (up.status == AlSigmaFamilyStatus::Ok && dn.status == AlSigmaFamilyStatus::Ok) {
            const double rho2 = (up.price - dn.price) / (2.0 * hr);
            e_rho_y.add(i, rv.rho_frozen_y, rho2, fp);
            e_rho_bb.add(i, rv.rho_frozen_b, rho2, fp);
            e_rhoC_fam2.add(i, a.rho, rho2, fp);
            if (b.has_value()) {
              e_rhoB_vs_fam2.add(i, b->rho, rho2, fp);
              e_rhoY_b.add(i, rv.rho_frozen_y, b->rho, fp);
            }
          } else {
            fam2_failed += 1.0;
          }
        } else {
          fam2_failed += 1.0;
        }
        if (fam_q) {
          const AlSigmaFamilyPrice up = fqp.price(p.S, p.K, sigma);
          const AlSigmaFamilyPrice dn = fqm.price(p.S, p.K, sigma);
          if (up.status == AlSigmaFamilyStatus::Ok && dn.status == AlSigmaFamilyStatus::Ok) {
            const double phi2 = (up.price - dn.price) / (2.0 * hr);
            e_phi_y.add(i, rv.phi_frozen_y, phi2, fp);
            e_phi_bb.add(i, rv.phi_frozen_b, phi2, fp);
            e_phiC_fam2.add(i, a.phi, phi2, fp);
            // The cold q ± h central difference (what a carry bundle would re-solve).
            const Result<double> cu = andersen_lake(gqp, p.S, p.K, sigma, p.side);
            const Result<double> cd = andersen_lake(gqm, p.S, p.K, sigma, p.side);
            if (cu.has_value() && cd.has_value()) {
              e_phiB_vs_fam2.add(i, (*cu - *cd) / (2.0 * hr), phi2, fp);
              e_phiC_cold.add(i, a.phi, (*cu - *cd) / (2.0 * hr), fp);
            }
          } else {
            fam2_failed += 1.0;
          }
        } else {
          fam2_failed += 1.0;
        }
      }
    }
  }
  state.counters["points"] = static_cast<double>(pts.size());
  state.counters["priced"] = priced;
  state.counters["refused"] = refused;
  state.counters["exercised"] = exercised;
  state.counters["one_sided_ref"] = one_sided;
  state.counters["fam2_failed"] = fam2_failed;
  state.counters["carry_failed"] = carry_failed;
  e_delta_b.publish(state, "delta_vs_bundle");
  e_gamma_b.publish(state, "gamma_vs_bundle");
  e_vega_b.publish(state, "vega_vs_bundle");
  e_theta_b.publish(state, "theta_vs_bundle");
  e_vanna_b.publish(state, "vanna_vs_bundle");
  e_volga_b.publish(state, "volga_vs_bundle");
  e_rhoC_b.publish(state, "rhoC_vs_bundle");
  e_rhoY_b.publish(state, "rhoY_vs_bundle");
  e_rhoC_fam2.publish(state, "rhoC_vs_fam2");
  e_phiC_cold.publish(state, "phiC_vs_cold");
  e_phiC_fam2.publish(state, "phiC_vs_fam2");
  e_delta_r.publish(state, "delta_vs_rich");
  e_gamma_r.publish(state, "gamma_vs_rich");
  e_vega_r.publish(state, "vega_vs_rich");
  e_theta_r.publish(state, "theta_vs_rich");
  e_vanna_r.publish(state, "vanna_vs_rich");
  e_volga_r.publish(state, "volga_vs_rich");
  e_rho_y.publish(state, "rhoY_vs_fam2");
  e_rho_bb.publish(state, "rhoB_vs_fam2");
  e_phi_y.publish(state, "phiY_vs_fam2");
  e_phi_bb.publish(state, "phiB_vs_fam2");
  e_rhoB_vs_fam2.publish(state, "bundle_rho_vs_fam2");
  e_phiB_vs_fam2.publish(state, "cold_phi_vs_fam2");
  const auto describe = [&](const char *greek, const GreekErr &e) {
    if (e.worst_rel < 0.0) {
      return;
    }
    const GridPt &p = pts[e.worst_rel_i];
    std::fprintf(stderr,
                 "[family-greeks] %s/n%u %s worst_rel=%.6g fam=%.17g ref=%.17g S=%.17g K=%.17g "
                 "T=%.17g sigma=%.17g r=%.17g q=%.17g side=%s | worst_abs=%.6g (fam=%.17g "
                 "ref=%.17g at K=%.17g sigma=%.17g side=%s)\n",
                 set_name, n_sigma, greek, e.worst_rel, e.worst_rel_fam, e.worst_rel_ref, p.S, p.K,
                 p.T, query_sigma[e.worst_rel_i], p.r, p.q, p.side == Side::Call ? "C" : "P",
                 e.worst_abs, e.worst_abs_fam, e.worst_abs_ref, pts[e.worst_abs_i].K,
                 query_sigma[e.worst_abs_i], pts[e.worst_abs_i].side == Side::Call ? "C" : "P");
  };
  describe("delta_vs_bundle", e_delta_b);
  describe("gamma_vs_bundle", e_gamma_b);
  describe("vega_vs_bundle", e_vega_b);
  describe("theta_vs_bundle", e_theta_b);
  describe("vanna_vs_bundle", e_vanna_b);
  describe("volga_vs_bundle", e_volga_b);
  describe("rhoC_vs_bundle", e_rhoC_b);
  describe("rhoY_vs_bundle", e_rhoY_b);
  describe("rhoC_vs_fam2", e_rhoC_fam2);
  describe("phiC_vs_cold", e_phiC_cold);
  describe("phiC_vs_fam2", e_phiC_fam2);
  describe("delta_vs_rich", e_delta_r);
  describe("gamma_vs_rich", e_gamma_r);
  describe("vega_vs_rich", e_vega_r);
  describe("theta_vs_rich", e_theta_r);
  describe("vanna_vs_rich", e_vanna_r);
  describe("volga_vs_rich", e_volga_r);
  describe("rhoY_vs_fam2", e_rho_y);
  describe("rhoB_vs_fam2", e_rho_bb);
  describe("phiY_vs_fam2", e_phi_y);
  describe("phiB_vs_fam2", e_phi_bb);
  describe("bundle_rho_vs_fam2", e_rhoB_vs_fam2);
  describe("cold_phi_vs_fam2", e_phiB_vs_fam2);
}

// Per-contract µs of one family entry over the SPY 23 d chain (one timed pass).
enum class GreeksRow { Price, Hedge, Eight, All, PriceVega, Chord };

// A0 r6: an FNV-1a fold of a double's bits, reported as a 52-bit counter (a double holds
// it exactly) — two runs with equal folds served the same bits in every folded field
// (the L3g m3 before / after identity of the price and every Greek).
void fold_bits(std::uint64_t &h, double x) noexcept {
  const auto b = std::bit_cast<std::uint64_t>(x);
  for (int i = 0; i < 8; ++i) {
    h ^= (b >> (8 * i)) & 0xFFu;
    h *= 1099511628211ULL;
  }
}
[[nodiscard]] double bits52(std::uint64_t h) noexcept {
  return static_cast<double>(h & ((std::uint64_t{1} << 52) - 1u));
}

void run_spy23_family_greeks(benchmark::State &state, GreeksRow row) {
  using namespace atx::vol;
  using Mask = AlSigmaFamilyGreeksMask;
  const std::vector<GridPt> &pts = err_points(ErrSet::Spy23);
  if (pts.empty()) {
    state.SkipWithError("no contracts (ATX_VOL_AL_SPY23 unreadable or empty)");
    return;
  }
  std::map<FamilyKey, std::vector<std::size_t>> groups;
  std::map<FamilyKey, std::pair<double, double>> chain_range;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const GridPt &p = pts[i];
    groups[FamilyKey{p.S, p.T, p.r, p.q, p.side}].push_back(i);
    auto [it, fresh] = chain_range.try_emplace(FamilyKey{p.S, p.T, 0.0, 0.0, p.side},
                                               std::pair<double, double>{p.sigma, p.sigma});
    if (!fresh) {
      it->second.first = std::min(it->second.first, p.sigma);
      it->second.second = std::max(it->second.second, p.sigma);
    }
  }
  const Mask mask = row == GreeksRow::Hedge ? Mask::hedge()
                    : row == GreeksRow::Eight ? Mask::eight()
                    : row == GreeksRow::All   ? Mask::all()
                                              : Mask::price_only();
  std::vector<double> us;
  std::vector<double> carry_ms; // L3g r4: the carry set's build per (S, T, r, q, side)
  const bool with_carry = mask.has(Mask::kRho) || mask.has(Mask::kPhi);
  double sink = 0.0;
  double refused = 0.0;
  double carry_refused = 0.0;
  std::uint64_t fold = 1469598103934665603ULL;
  AlSigmaFamilyGreeks last;
  for (auto _ : state) {
    us.clear();
    carry_ms.clear();
    double total_us = 0.0;
    for (const auto &[key, idx] : groups) {
      const FamilyBox box =
          family_box(chain_range.at(FamilyKey{key.S, key.T, 0.0, 0.0, key.side}).first,
                     chain_range.at(FamilyKey{key.S, key.T, 0.0, 0.0, key.side}).second);
      const AlExpiryGeometry g = prepare_al_geometry(key.T, key.r, key.q);
      AlSigmaFamily fam(g, key.side, key.S);
      if (!fam.prepare(box.lo, box.hi, 12, AlSigmaFamilyAxis::LogSigma)) {
        refused += static_cast<double>(idx.size());
        continue;
      }
      AlSigmaFamilyCarryBumps carry;
      if (with_carry) {
        const auto c0 = std::chrono::steady_clock::now();
        const bool ok = carry.prepare(fam, mask).has_value();
        const auto c1 = std::chrono::steady_clock::now();
        carry_ms.push_back(std::chrono::duration<double, std::milli>(c1 - c0).count());
        carry_refused += ok ? 0.0 : static_cast<double>(idx.size());
      }
      for (const std::size_t i : idx) {
        const GridPt &p = pts[i];
        const auto t0 = std::chrono::steady_clock::now();
        if (row == GreeksRow::PriceVega) {
          const AlSigmaFamilyPriceVega pv = family_price_and_vega(fam, p.S, p.K, p.sigma);
          sink += pv.vega;
          refused += pv.status == AlSigmaFamilyStatus::Ok ? 0.0 : 1.0;
        } else if (row == GreeksRow::Chord) {
          const AlSigmaFamilyChord c = fam.price_and_chord(p.S, p.K, p.sigma);
          sink += c.price_sigma_bumped;
          refused += c.status == AlSigmaFamilyStatus::Ok ? 0.0 : 1.0;
        } else if (with_carry) {
          last = family_greeks(fam, carry, p.S, p.K, p.sigma, mask);
          sink += last.price + last.delta + last.rho;
          refused += last.status == AlSigmaFamilyStatus::Ok ? 0.0 : 1.0;
        } else {
          last = family_greeks(fam, p.S, p.K, p.sigma, mask);
          sink += last.price + last.delta;
          refused += last.status == AlSigmaFamilyStatus::Ok ? 0.0 : 1.0;
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (row != GreeksRow::PriceVega && row != GreeksRow::Chord) {
          for (const double x : {last.price, last.delta, last.gamma, last.vega, last.theta,
                                 last.rho, last.phi, last.vanna, last.volga, last.charm}) {
            fold_bits(fold, x);
          }
        }
        const double dt = std::chrono::duration<double, std::micro>(t1 - t0).count();
        us.push_back(dt);
        total_us += dt;
      }
    }
    benchmark::DoNotOptimize(sink);
    state.SetIterationTime(total_us * 1.0e-6);
  }
  std::sort(us.begin(), us.end());
  double mean = 0.0;
  for (const double v : us) {
    mean += v;
  }
  state.counters["contracts"] = static_cast<double>(us.size());
  state.counters["refused"] = refused;
  state.counters["bits_fnv52"] = bits52(fold);
  state.counters["p50_us"] = sorted_quantile(us, 0.50);
  state.counters["p90_us"] = sorted_quantile(us, 0.90);
  state.counters["p99_us"] = sorted_quantile(us, 0.99);
  state.counters["mean_us"] = us.empty() ? 0.0 : mean / static_cast<double>(us.size());
  if (with_carry) {
    double cm = 0.0;
    for (const double v : carry_ms) {
      cm += v;
    }
    state.counters["carry_sets"] = static_cast<double>(carry_ms.size());
    state.counters["carry_build_mean_ms"] =
        carry_ms.empty() ? 0.0 : cm / static_cast<double>(carry_ms.size());
    state.counters["carry_refused_rows"] = carry_refused;
  }
}

// The reference bundles' µs on the same contracts: american_greeks_al(g, …) accurate /
// Fast profile, and the adjoint bundle (puts only — it declines calls).
void run_spy23_reference_bundles(benchmark::State &state) {
  using namespace atx::vol;
  const std::vector<GridPt> &pts = err_points(ErrSet::Spy23);
  if (pts.empty()) {
    state.SkipWithError("no contracts (ATX_VOL_AL_SPY23 unreadable or empty)");
    return;
  }
  std::vector<double> acc, fast, adj;
  double sink = 0.0;
  double adj_declined = 0.0;
  for (auto _ : state) {
    acc.clear();
    fast.clear();
    adj.clear();
    adj_declined = 0.0;
    double total_us = 0.0;
    for (const GridPt &p : pts) {
      const AlExpiryGeometry g = prepare_al_geometry(p.T, p.r, p.q);
      const AlExpiryGeometry gf = prepare_al_geometry(p.T, p.r, p.q, al_fast_profile(std::nullopt));
      {
        const auto t0 = std::chrono::steady_clock::now();
        const Result<AmericanGreeks> b = american_greeks_al(g, p.S, p.K, p.sigma, p.side);
        const auto t1 = std::chrono::steady_clock::now();
        sink += b.has_value() ? b->delta : 0.0;
        const double dt = std::chrono::duration<double, std::micro>(t1 - t0).count();
        acc.push_back(dt);
        total_us += dt;
      }
      {
        const auto t0 = std::chrono::steady_clock::now();
        const Result<AmericanGreeks> b = american_greeks_al(gf, p.S, p.K, p.sigma, p.side);
        const auto t1 = std::chrono::steady_clock::now();
        sink += b.has_value() ? b->delta : 0.0;
        fast.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
      }
      if (p.side == Side::Put) {
        bool took = false;
        const auto t0 = std::chrono::steady_clock::now();
        const Result<AmericanGreeks> b = detail::american_greeks_adjoint(
            p.S, p.K, p.T, p.sigma, p.r, p.q, p.side, std::nullopt, &took);
        const auto t1 = std::chrono::steady_clock::now();
        sink += b.has_value() ? b->delta : 0.0;
        if (took) {
          adj.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        } else {
          adj_declined += 1.0;
        }
      }
    }
    benchmark::DoNotOptimize(sink);
    state.SetIterationTime(total_us * 1.0e-6);
  }
  std::sort(acc.begin(), acc.end());
  std::sort(fast.begin(), fast.end());
  std::sort(adj.begin(), adj.end());
  state.counters["contracts"] = static_cast<double>(acc.size());
  state.counters["bundle_accurate_p50_us"] = sorted_quantile(acc, 0.50);
  state.counters["bundle_accurate_p99_us"] = sorted_quantile(acc, 0.99);
  state.counters["bundle_fast_p50_us"] = sorted_quantile(fast, 0.50);
  state.counters["bundle_fast_p99_us"] = sorted_quantile(fast, 0.99);
  state.counters["adjoint_p50_us"] = sorted_quantile(adj, 0.50);
  state.counters["adjoint_p99_us"] = sorted_quantile(adj, 0.99);
  state.counters["adjoint_taken"] = static_cast<double>(adj.size());
  state.counters["adjoint_declined"] = adj_declined;
}

// A chain for the chain / shock rows: the SPY 23 d contracts (one put + one call family
// at the chain's median q) or a synthetic 300-strike chain at one expiry.
struct ChainInputs {
  double S = 0.0, T = 0.0, r = 0.0, q = 0.0;
  std::vector<double> K, sigma;
  std::vector<Side> side;
  double lo = 0.0, hi = 0.0;
};

[[nodiscard]] ChainInputs chain_inputs(bool spy) {
  using namespace atx::vol;
  ChainInputs c;
  if (spy) {
    const std::vector<GridPt> &pts = err_points(ErrSet::Spy23);
    if (pts.empty()) {
      return c;
    }
    std::vector<double> qs;
    double smin = 1.0e9, smax = 0.0;
    for (const GridPt &p : pts) {
      qs.push_back(p.q);
      smin = std::min(smin, p.sigma);
      smax = std::max(smax, p.sigma);
      c.K.push_back(p.K);
      c.sigma.push_back(p.sigma);
      c.side.push_back(p.side);
    }
    std::sort(qs.begin(), qs.end());
    c.S = pts[0].S;
    c.T = pts[0].T;
    c.r = pts[0].r;
    c.q = qs[qs.size() / 2];
    const FamilyBox box = family_box(smin, smax);
    c.lo = box.lo;
    c.hi = box.hi;
    return c;
  }
  c.S = 773.0;
  c.T = 23.0 / 365.0;
  c.r = 0.045;
  c.q = 0.0016;
  double smin = 1.0e9, smax = 0.0;
  for (int i = 0; i < 300; ++i) {
    const double K = 540.0 + 550.0 * static_cast<double>(i) / 299.0;
    const double F = c.S * std::exp((c.r - c.q) * c.T);
    const double sig = representative_sigma(std::log(K / F), c.T);
    c.K.push_back(K);
    c.sigma.push_back(sig);
    c.side.push_back(K < c.S ? Side::Put : Side::Call);
    smin = std::min(smin, sig);
    smax = std::max(smax, sig);
  }
  const FamilyBox box = family_box(smin, smax);
  c.lo = box.lo;
  c.hi = box.hi;
  return c;
}

struct ChainColumns {
  std::vector<double> price, delta, gamma, vega, theta, rho, phi, vanna, volga, charm;
  std::vector<std::uint8_t> flags;
  std::vector<atx::vol::AlSigmaFamilyStatus> status;
  explicit ChainColumns(std::size_t n)
      : price(n), delta(n), gamma(n), vega(n), theta(n), rho(n), phi(n), vanna(n), volga(n),
        charm(n), flags(n), status(n) {}
  [[nodiscard]] atx::vol::AlSigmaFamilyGreekColumns view() {
    atx::vol::AlSigmaFamilyGreekColumns v;
    v.price = price;
    v.delta = delta;
    v.gamma = gamma;
    v.vega = vega;
    v.theta = theta;
    v.rho = rho;
    v.phi = phi;
    v.vanna = vanna;
    v.volga = volga;
    v.charm = charm;
    v.flags = flags;
    return v;
  }
};

void run_family_chain(benchmark::State &state, bool spy, GreeksRow row) {
  using namespace atx::vol;
  using Mask = AlSigmaFamilyGreeksMask;
  const ChainInputs c = chain_inputs(spy);
  if (c.K.empty()) {
    state.SkipWithError("no chain (ATX_VOL_AL_SPY23 unreadable or empty)");
    return;
  }
  const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, c.q);
  AlSigmaFamily put(g, Side::Put, c.S), call(g, Side::Call, c.S);
  const bool put_ok = put.prepare(c.lo, c.hi, 12).has_value();
  const bool call_ok = call.prepare(c.lo, c.hi, 12).has_value();
  const Mask mask = row == GreeksRow::Hedge ? Mask::hedge()
                    : row == GreeksRow::Eight ? Mask::eight()
                    : row == GreeksRow::All   ? Mask::all()
                                              : Mask::price_only();
  // rho / phi rows ride the carry sets of both families (built outside the timed call;
  // their build is reported beside it).
  const bool with_carry = mask.has(Mask::kRho) || mask.has(Mask::kPhi);
  AlSigmaFamilyCarryBumps put_carry, call_carry;
  double carry_build_ms = 0.0;
  if (with_carry) {
    const auto c0 = std::chrono::steady_clock::now();
    const bool pc = put_ok && put_carry.prepare(put, mask).has_value();
    const bool cc = call_ok && call_carry.prepare(call, mask).has_value();
    const auto c1 = std::chrono::steady_clock::now();
    carry_build_ms = std::chrono::duration<double, std::milli>(c1 - c0).count();
    state.counters["put_carry"] = pc ? 1.0 : 0.0;
    state.counters["call_carry"] = cc ? 1.0 : 0.0;
  }
  ChainColumns cols(c.K.size());
  double refused = 0.0;
  for (auto _ : state) {
    const auto t0 = std::chrono::steady_clock::now();
    const Status st = family_chain_greeks(
        put_ok ? &put : nullptr, call_ok ? &call : nullptr, c.S, c.K, c.sigma, c.side, mask,
        cols.view(), cols.status, with_carry && put_carry.prepared() ? &put_carry : nullptr,
        with_carry && call_carry.prepared() ? &call_carry : nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    benchmark::DoNotOptimize(cols.price.data());
    if (!st.has_value()) {
      state.SkipWithError("family_chain_greeks refused");
      return;
    }
    refused = 0.0;
    for (const AlSigmaFamilyStatus s : cols.status) {
      refused += s == AlSigmaFamilyStatus::Ok ? 0.0 : 1.0;
    }
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
  }
  state.counters["rows"] = static_cast<double>(c.K.size());
  state.counters["refused"] = refused;
  state.counters["put_family"] = put_ok ? 1.0 : 0.0;
  state.counters["call_family"] = call_ok ? 1.0 : 0.0;
  if (with_carry) {
    std::size_t carry_refused = 0;
    for (const std::uint8_t f : cols.flags) {
      carry_refused += (f & 8u) != 0 ? 1u : 0u;
    }
    state.counters["carry_build_ms_both_sides"] = carry_build_ms;
    state.counters["rho_phi_refused_rows"] = static_cast<double>(carry_refused);
  }
}

void run_family_shock(benchmark::State &state, bool chain) {
  using namespace atx::vol;
  using Mask = AlSigmaFamilyGreeksMask;
  const ChainInputs c = chain_inputs(false);
  const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, c.q);
  AlSigmaFamily put(g, Side::Put, c.S), call(g, Side::Call, c.S);
  // The shock grid may push σ 10 points either way: widen the box for the row.
  const double lo = std::max(kAlSigmaFamilyMinSigma, c.lo - 0.10);
  const double hi = c.hi + 0.10;
  if (!put.prepare(lo, hi, 12) || !call.prepare(lo, hi, 12)) {
    state.SkipWithError("family prepare refused");
    return;
  }
  std::vector<double> spot_pct, vol_bump;
  for (int i = -5; i <= 5; ++i) {
    spot_pct.push_back(0.02 * i);
    vol_bump.push_back(0.02 * i);
  }
  const std::size_t cells = spot_pct.size() * vol_bump.size();
  ChainColumns cols(cells);
  const std::size_t n = chain ? c.K.size() : 1u;
  double refused = 0.0;
  for (auto _ : state) {
    refused = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < n; ++i) {
      const double K = chain ? c.K[i] : c.S;
      const double sig = chain ? c.sigma[i] : 0.16;
      const AlSigmaFamily &f = chain ? (c.side[i] == Side::Call ? call : put) : put;
      const Status st =
          family_shock_grid(f, c.S, K, sig, spot_pct, vol_bump, Mask::eight(), cols.view(), cols.status);
      if (!st.has_value()) {
        state.SkipWithError("family_shock_grid refused");
        return;
      }
      for (const AlSigmaFamilyStatus s : cols.status) {
        refused += s == AlSigmaFamilyStatus::Ok ? 0.0 : 1.0;
      }
    }
    const auto t1 = std::chrono::steady_clock::now();
    benchmark::DoNotOptimize(cols.price.data());
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
  }
  state.counters["cells"] = static_cast<double>(cells * n);
  state.counters["contracts"] = static_cast<double>(n);
  state.counters["refused"] = refused;
}

// ── L2h-b sigma-q family (2026-10-21): AlSigmaQFamily build / price / error rows ──
//
//   * ATX_VOL_AL_SQFAMILY=1 registers, for n_q in {3, 4, 6} (n_sigma 12, LogSigma):
//       american/sqfamily/spy23/nq<N>    (needs ATX_VOL_AL_SPY23: the SPY 23 d chain)
//       american/sqfamily/spylong/nq<N>  (needs ATX_VOL_AL_SPYLONG: one more chain in the
//                                         same S,K,T,r,q,is_call,bid,ask format — SPY 2.17 y)
//       american/sqfamily/spyall/nq<N>   (needs ATX_VOL_AL_SPYALL: every SPY expiry)
//     and american/sqfamily/<set>/sigma_family_qmid — the representative-q baseline: ONE
//     AlSigmaFamily per expiry side at the box-mid q, every strike priced at that q.
//     A row groups the set's contracts by (S, T, r, side) — one expiry side — takes the
//     group's q box = [min q, max q] of its rows and its σ box = family_box over its
//     admitted σ, builds ONE family per group on a Reference geometry at the box-mid q
//     (median of 5 timed builds), and queries every contract at its OWN q at n_sig σ
//     values spanning the box (log-uniform cell midpoints of [σ_lo, σ_hi - 2h], off the
//     nodes; 16 for spy23 / spylong, 4 for spyall) against the cold Reference
//     andersen_lake(S, K, T, σ, r, q_K) — computed once per set, shared by the rows.
//     Counters: build per family (p50 / max) and the set's build total (ms), price p50 /
//     p99 and chord p50 µs per query, |dP| max / p99, max rel and p99 rel on P >= 0.01,
//     max |dP| / max(P, 1), groups the family refused (whole side cold) and why (stderr).

enum class SqSet { Spy23, SpyLong, SpyAll };

struct SqGroup {
  double S = 0.0;
  double T = 0.0;
  double r = 0.0;
  Side side = Side::Put;
  double q_lo = 0.0;
  double q_hi = 0.0;
  double s_lo = 0.0;
  double s_hi = 0.0;
  std::vector<std::size_t> idx;
  std::vector<double> sigmas; // the queried σ grid
  std::vector<double> ref;    // ref[local * sigmas.size() + k]; NaN where the cold errs
};

struct SqData {
  const std::vector<GridPt> *pts = nullptr;
  std::vector<SqGroup> groups;
  std::size_t queries = 0;
};

[[nodiscard]] SqData build_sq_data(const std::vector<GridPt> &pts, unsigned n_sig) {
  SqData d;
  d.pts = &pts;
  std::map<FamilyKey, std::vector<std::size_t>> by_side;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const GridPt &p = pts[i];
    by_side[FamilyKey{p.S, p.T, p.r, 0.0, p.side}].push_back(i);
  }
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const auto &[key, idx] : by_side) {
    SqGroup g;
    g.S = key.S;
    g.T = key.T;
    g.r = key.r;
    g.side = key.side;
    g.idx = idx;
    g.q_lo = std::numeric_limits<double>::infinity();
    g.q_hi = -std::numeric_limits<double>::infinity();
    double smin = std::numeric_limits<double>::infinity();
    double smax = 0.0;
    for (const std::size_t i : idx) {
      g.q_lo = std::min(g.q_lo, pts[i].q);
      g.q_hi = std::max(g.q_hi, pts[i].q);
      if (pts[i].sigma >= atx::vol::kAlSigmaFamilyMinSigma) {
        smin = std::min(smin, pts[i].sigma);
        smax = std::max(smax, pts[i].sigma);
      }
    }
    if (!(smax > 0.0)) {
      smin = 0.2;
      smax = 0.2;
    }
    const FamilyBox box = family_box(smin, smax);
    g.s_lo = box.lo;
    g.s_hi = box.hi;
    const double a = std::log(box.lo);
    const double b = std::log(box.hi - 2.0 * atx::vol::kAlPackSigmaBump);
    for (unsigned k = 0; k < n_sig; ++k) {
      g.sigmas.push_back(
          std::exp(a + (b - a) * (static_cast<double>(k) + 0.5) / static_cast<double>(n_sig)));
    }
    g.ref.assign(idx.size() * n_sig, nan);
    for (std::size_t l = 0; l < idx.size(); ++l) {
      const GridPt &p = pts[idx[l]];
      for (unsigned k = 0; k < n_sig; ++k) {
        const atx::vol::Result<double> c =
            atx::vol::andersen_lake(p.S, p.K, p.T, g.sigmas[k], p.r, p.q, p.side);
        g.ref[l * n_sig + k] = c.has_value() ? *c : nan;
      }
    }
    d.queries += idx.size() * n_sig;
    d.groups.push_back(std::move(g));
  }
  return d;
}

[[nodiscard]] const SqData &sq_data(SqSet s) {
  static const std::vector<GridPt> lng = load_quote_inputs(env_text("ATX_VOL_AL_SPYLONG"));
  static const std::vector<GridPt> all = load_quote_inputs(env_text("ATX_VOL_AL_SPYALL"));
  switch (s) {
  case SqSet::Spy23: {
    static const SqData d = build_sq_data(err_points(ErrSet::Spy23), 16);
    return d;
  }
  case SqSet::SpyLong: {
    static const SqData d = build_sq_data(lng, 16);
    return d;
  }
  case SqSet::SpyAll:
    break;
  }
  static const SqData d = build_sq_data(all, 4);
  return d;
}

// One group's family (the q-family at n_q, or the representative-q σ-family baseline
// when n_q == 0): build timing + prices into got[], or the refusal text.
struct SqGroupRun {
  bool built = false;
  double build_us = 0.0;
  std::string refusal;
  std::size_t state_bytes = 0; // L2h-c: the family's own (exactly sized) state
};

// got[local * n_sig + k] receives the family price (NaN on a refusal); price_us /
// chord_us one sample per query per timed pass (3 passes each).
[[nodiscard]] SqGroupRun run_sq_group(const SqGroup &g, unsigned n_q, unsigned n_sigma,
                                      const std::vector<GridPt> &pts, std::vector<double> &got,
                                      std::vector<double> &price_us,
                                      std::vector<double> &chord_us) {
  using namespace atx::vol;
  SqGroupRun out;
  const AlExpiryGeometry geo = prepare_al_geometry(g.T, g.r, 0.5 * (g.q_lo + g.q_hi));
  AlSigmaQFamily qf(geo, g.side, g.S);
  AlSigmaFamily sf(geo, g.side, g.S);
  const auto ns = static_cast<std::uint16_t>(n_sigma);
  std::vector<double> reps;
  for (int rep = 0; rep < 5; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    const Status st = (n_q == 0) ? sf.prepare(g.s_lo, g.s_hi, ns)
                                 : qf.prepare(g.s_lo, g.s_hi, ns, g.q_lo, g.q_hi,
                                              static_cast<std::uint16_t>(n_q));
    const auto t1 = std::chrono::steady_clock::now();
    if (!st) {
      out.refusal = st.error().to_string();
      return out;
    }
    reps.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }
  std::sort(reps.begin(), reps.end());
  out.built = true;
  out.build_us = reps[reps.size() / 2];
  out.state_bytes = (n_q == 0) ? AlSigmaFamily::state_bytes() : qf.state_bytes();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::size_t n_sig = g.sigmas.size();
  got.assign(g.idx.size() * n_sig, nan);
  for (std::size_t l = 0; l < g.idx.size(); ++l) {
    const GridPt &p = pts[g.idx[l]];
    for (std::size_t k = 0; k < n_sig; ++k) {
      const double sigma = g.sigmas[k];
      double price = nan;
      for (int pass = 0; pass < 3; ++pass) {
        const auto t0 = std::chrono::steady_clock::now();
        if (n_q == 0) {
          const AlSigmaFamilyPrice fp = sf.price(p.S, p.K, sigma);
          price = fp.status == AlSigmaFamilyStatus::Ok ? fp.price : nan;
        } else {
          const AlSigmaQFamilyPrice fp = qf.price(p.S, p.K, sigma, p.q);
          price = fp.status == AlSigmaQFamilyStatus::Ok ? fp.price : nan;
        }
        const auto t1 = std::chrono::steady_clock::now();
        benchmark::DoNotOptimize(price);
        price_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
      }
      for (int pass = 0; pass < 3 && n_q != 0; ++pass) {
        const auto t0 = std::chrono::steady_clock::now();
        AlSigmaQFamilyChord c = qf.price_and_chord(p.S, p.K, sigma, p.q);
        const auto t1 = std::chrono::steady_clock::now();
        benchmark::DoNotOptimize(c);
        chord_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
      }
      got[l * n_sig + k] = price;
    }
  }
  return out;
}

// n_q == 0: the representative-q σ-family baseline.
void run_sqfamily(benchmark::State &state, SqSet set, const char *set_name, unsigned n_q,
                  unsigned n_sigma) {
  using namespace atx::vol;
  const SqData &d = sq_data(set);
  if (d.pts == nullptr || d.pts->empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  const std::vector<GridPt> &pts = *d.pts;
  std::vector<double> build_us;
  std::vector<double> price_us;
  std::vector<double> chord_us;
  ErrDist p_abs;
  ErrDist p_rel;
  double max_scaled = 0.0;
  double worst_rel = -1.0;
  std::string worst_at;
  double refused = 0.0;
  double cold_groups = 0.0;
  double priced = 0.0;
  double q_spread_max = 0.0;
  double state_bytes_max = 0.0;
  std::vector<double> got;
  for (auto _ : state) {
    for (const SqGroup &g : d.groups) {
      const SqGroupRun run = run_sq_group(g, n_q, n_sigma, pts, got, price_us, chord_us);
      state_bytes_max = std::max(state_bytes_max, static_cast<double>(run.state_bytes));
      if (!run.built) {
        cold_groups += 1.0;
        refused += static_cast<double>(g.idx.size() * g.sigmas.size());
        std::fprintf(stderr,
                     "[sqfamily] %s/nq%u/ns%u cold group T=%.6g side=%s q=[%.6g, %.6g]: %s\n",
                     set_name, n_q, n_sigma, g.T, g.side == Side::Call ? "C" : "P", g.q_lo,
                     g.q_hi, run.refusal.c_str());
        continue;
      }
      build_us.push_back(run.build_us);
      q_spread_max = std::max(q_spread_max, g.q_hi - g.q_lo);
      const std::size_t n_sig = g.sigmas.size();
      for (std::size_t l = 0; l < g.idx.size(); ++l) {
        for (std::size_t k = 0; k < n_sig; ++k) {
          const double ref = g.ref[l * n_sig + k];
          const double fam = got[l * n_sig + k];
          if (!std::isfinite(ref) || !std::isfinite(fam)) {
            refused += 1.0;
            continue;
          }
          priced += 1.0;
          const double dd = std::abs(fam - ref);
          p_abs.add(dd);
          max_scaled = std::max(max_scaled, dd / std::max(ref, 1.0));
          if (ref >= 0.01) {
            p_rel.add(dd / ref);
            if (dd / ref > worst_rel) {
              worst_rel = dd / ref;
              const GridPt &p = pts[g.idx[l]];
              char buf[400];
              std::snprintf(buf, sizeof(buf),
                            "S=%.17g K=%.17g T=%.17g sigma=%.17g r=%.17g q=%.17g side=%s "
                            "q_box=[%.17g, %.17g] ref=%.17g got=%.17g",
                            p.S, p.K, p.T, g.sigmas[k], p.r, p.q,
                            p.side == Side::Call ? "C" : "P", g.q_lo, g.q_hi, ref, fam);
              worst_at = buf;
            }
          }
        }
      }
    }
  }
  double build_total_us = 0.0;
  for (const double b : build_us) {
    build_total_us += b;
  }
  std::sort(build_us.begin(), build_us.end());
  std::sort(price_us.begin(), price_us.end());
  std::sort(chord_us.begin(), chord_us.end());
  state.counters["contracts"] = static_cast<double>(pts.size());
  state.counters["queries"] = static_cast<double>(d.queries);
  state.counters["groups"] = static_cast<double>(d.groups.size());
  state.counters["families"] = static_cast<double>(build_us.size());
  state.counters["cold_groups"] = cold_groups;
  state.counters["refused"] = refused;
  state.counters["priced"] = priced;
  state.counters["n_q"] = static_cast<double>(n_q);
  state.counters["q_spread_max"] = q_spread_max;
  state.counters["build_us_p50"] = sorted_quantile(build_us, 0.50);
  state.counters["build_us_max"] = build_us.empty() ? 0.0 : build_us.back();
  state.counters["build_ms_total"] = build_total_us * 1.0e-3;
  state.counters["price_us_p50"] = sorted_quantile(price_us, 0.50);
  state.counters["price_us_p99"] = sorted_quantile(price_us, 0.99);
  state.counters["chord_us_p50"] = sorted_quantile(chord_us, 0.50);
  state.counters["price_max_abs"] = p_abs.max();
  state.counters["price_p99_abs"] = p_abs.quantile(0.99);
  state.counters["price_max_rel"] = p_rel.max();
  state.counters["price_p99_rel"] = p_rel.quantile(0.99);
  state.counters["price_max_scaled"] = max_scaled;
  state.counters["state_bytes"] = state_bytes_max;
  state.counters["n_sigma"] = static_cast<double>(n_sigma);
  if (worst_rel >= 0.0) {
    std::fprintf(stderr, "[sqfamily] %s/nq%u/ns%u worst_rel=%.6g %s\n", set_name, n_q, n_sigma,
                 worst_rel, worst_at.c_str());
  }
}

const bool kRegisteredSigmaQ = [] {
  if (env_text("ATX_VOL_AL_SQFAMILY").empty()) {
    return false;
  }
  const std::pair<const char *, SqSet> sets[] = {
      {"spy23", SqSet::Spy23}, {"spylong", SqSet::SpyLong}, {"spyall", SqSet::SpyAll}};
  const char *envs[] = {"ATX_VOL_AL_SPY23", "ATX_VOL_AL_SPYLONG", "ATX_VOL_AL_SPYALL"};
  for (std::size_t si = 0; si < 3; ++si) {
    if (env_text(envs[si]).empty()) {
      continue;
    }
    const SqSet s = sets[si].second;
    const char *set_name = sets[si].first;
    // (n_q, n_sigma): the n_q sweep at the σ-family's 12 nodes, then 16 σ nodes at n_q 4
    // (the long-dated σ floor); n_q 0 = the representative-q σ-family baseline.
    const std::pair<unsigned, unsigned> shapes[] = {{0u, 12u}, {3u, 12u}, {4u, 12u},
                                                    {6u, 12u}, {4u, 16u}};
    for (const auto &[nq_v, ns_v] : shapes) {
      const unsigned n_q = nq_v;
      const unsigned n_sigma = ns_v;
      const std::string tail = (n_q == 0) ? "sigma_family_qmid" : "nq" + std::to_string(n_q);
      benchmark::RegisterBenchmark(std::string("american/sqfamily/") + set_name + "/" + tail +
                                       "/ns" + std::to_string(n_sigma),
                                   [s, set_name, n_q, n_sigma](benchmark::State &state) {
                                     run_sqfamily(state, s, set_name, n_q, n_sigma);
                                   })
          ->Iterations(1)
          ->Unit(benchmark::kMillisecond);
    }
  }
  return true;
}();

// ── L2h-c (2026-10-21): AlSigmaQFamily BUILD rows — AVX2-batched nodes vs the scalar build ──
//
//   * ATX_VOL_AL_SQBUILD=1 registers (inputs: the sqfamily env files above):
//       american/sqbuild/<set>/<isa>/nq<N>/ns<M>  for (N, M) in {(3, 12), (4, 12), (4, 16)},
//         isa scalar (SimdIsa::ForceScalar: every node the scalar prepared solve — the L2h-b
//         build) / avx2 (ForceAvx2: nodes 4 at a time through the AVX2 boundary kernel). One
//         family per (S, T, r, side) group as run_sq_group lays it out, built 5x (median):
//         build_us p50 / max per family, build_ms_total over the set, cold groups, state_bytes;
//       american/sqbuild/parts/<set>  where one (4, 12) build's time goes, on the set's first
//         group: one scalar prepared solve (median of the 48 nodes), one AVX2 pack of 4 nodes
//         (median of 12), one al_prepare_oriented_geometry (median of the 4 q nodes), the
//         whole AVX2 prepare (median of 5) and the OffNode self-check's extra cost;
//       american/sqbuild/spy23/price_strikes300  each 23 d side at (4, 12): 300 strikes in
//         [0.7 S, 1.3 S] at one (σ, q) through price_strikes vs 300 price() calls (median
//         of 21), plus the bit-identity count.

struct SqIsaScope {
  explicit SqIsaScope(atx::vol::simd::SimdIsa isa) : prev_(atx::vol::simd::simd_isa_override()) {
    atx::vol::simd::set_simd_isa_override(isa);
  }
  ~SqIsaScope() { atx::vol::simd::set_simd_isa_override(prev_); }
  SqIsaScope(const SqIsaScope &) = delete;
  SqIsaScope &operator=(const SqIsaScope &) = delete;
  atx::vol::simd::SimdIsa prev_;
};

[[nodiscard]] double us_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

[[nodiscard]] double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : v[v.size() / 2];
}

void run_sqbuild(benchmark::State &state, SqSet set, const char *set_name, bool avx2,
                 unsigned n_q, unsigned n_sigma) {
  using namespace atx::vol;
  const SqData &d = sq_data(set);
  if (d.pts == nullptr || d.pts->empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  if (avx2 && !simd::have_avx2()) {
    state.SkipWithError("host has no AVX2");
    return;
  }
  const SqIsaScope scope(avx2 ? simd::SimdIsa::ForceAvx2 : simd::SimdIsa::ForceScalar);
  std::vector<double> build_us;
  std::vector<double> bytes;
  double cold_groups = 0.0;
  for (auto _ : state) {
    for (const SqGroup &g : d.groups) {
      const AlExpiryGeometry geo = prepare_al_geometry(g.T, g.r, 0.5 * (g.q_lo + g.q_hi));
      AlSigmaQFamily qf(geo, g.side, g.S);
      std::vector<double> reps;
      for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = qf.prepare(g.s_lo, g.s_hi, static_cast<std::uint16_t>(n_sigma), g.q_lo,
                                     g.q_hi, static_cast<std::uint16_t>(n_q));
        const double us = us_since(t0);
        if (!st) {
          reps.clear();
          break;
        }
        reps.push_back(us);
      }
      if (reps.empty()) {
        cold_groups += 1.0;
        continue;
      }
      build_us.push_back(median_of(reps));
      bytes.push_back(static_cast<double>(qf.state_bytes()));
      if (set != SqSet::SpyAll) {
        std::fprintf(stderr, "[sqbuild] %s/%s/nq%u/ns%u T=%.6g side=%s build_us=%.1f bytes=%zu\n",
                     set_name, avx2 ? "avx2" : "scalar", n_q, n_sigma, g.T,
                     g.side == Side::Call ? "C" : "P", build_us.back(), qf.state_bytes());
      }
    }
  }
  double total = 0.0;
  for (const double b : build_us) {
    total += b;
  }
  std::sort(build_us.begin(), build_us.end());
  state.counters["families"] = static_cast<double>(build_us.size());
  state.counters["cold_groups"] = cold_groups;
  state.counters["build_us_p50"] = sorted_quantile(build_us, 0.50);
  state.counters["build_us_max"] = build_us.empty() ? 0.0 : build_us.back();
  state.counters["build_ms_total"] = total * 1.0e-3;
  state.counters["state_bytes_p50"] = median_of(bytes);
  state.counters["n_q"] = static_cast<double>(n_q);
  state.counters["n_sigma"] = static_cast<double>(n_sigma);
}

void run_sqbuild_parts(benchmark::State &state, SqSet set) {
  using namespace atx::vol;
  const SqData &d = sq_data(set);
  if (d.pts == nullptr || d.groups.empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  if (!simd::have_avx2()) {
    state.SkipWithError("host has no AVX2");
    return;
  }
  constexpr unsigned kNs = 12;
  constexpr unsigned kNq = 4;
  // The first group the family builds (a q <= 0 call side is refused: no boundary).
  const SqGroup *first = nullptr;
  for (const SqGroup &cand : d.groups) {
    const AlExpiryGeometry cg = prepare_al_geometry(cand.T, cand.r, 0.5 * (cand.q_lo + cand.q_hi));
    AlSigmaQFamily probe(cg, cand.side, cand.S);
    if (probe.prepare(cand.s_lo, cand.s_hi, kNs, cand.q_lo, cand.q_hi, kNq)) {
      first = &cand;
      break;
    }
  }
  if (first == nullptr) {
    state.SkipWithError("no group builds a family");
    return;
  }
  const SqGroup &g = *first;
  const double pi = 3.14159265358979323846;
  double sig[kNs] = {};
  for (unsigned s = 0; s < kNs; ++s) {
    const double z = -std::cos(pi * static_cast<double>(s) / static_cast<double>(kNs - 1));
    sig[s] = std::exp(std::log(g.s_lo) + 0.5 * (std::log(g.s_hi) - std::log(g.s_lo)) * (z + 1.0));
  }
  double qn[kNq] = {};
  for (unsigned j = 0; j < kNq; ++j) {
    const double z = -std::cos(pi * static_cast<double>(j) / static_cast<double>(kNq - 1));
    qn[j] = g.q_lo + 0.5 * (g.q_hi - g.q_lo) * (z + 1.0);
  }
  const amer::AlScheme sch = amer::scheme_from_opts(std::nullopt);
  const auto geo = std::make_unique<amer::AlOrientedGeometry>();
  const auto sc = std::make_unique<amer::AlPreparedScratch>();
  const auto pb = std::make_unique<amer::AlBoundary[]>(4);
  const auto pw = std::make_unique<amer::AlWorkspace[]>(4);
  const auto pg = std::make_unique<simd::detail::NodePackScratch>();
  std::vector<double> geo_us;
  std::vector<double> solve_us;
  std::vector<double> pack_us;
  double declined = 0.0;
  for (auto _ : state) {
    for (unsigned j = 0; j < kNq; ++j) {
      const bool put = g.side == Side::Put;
      const double rp = put ? g.r : qn[j];
      const double qp = put ? qn[j] : g.r;
      auto t0 = std::chrono::steady_clock::now();
      amer::al_prepare_oriented_geometry(g.T, rp, qp, sch, *geo);
      geo_us.push_back(us_since(t0));
      for (unsigned s = 0; s < kNs; ++s) {
        t0 = std::chrono::steady_clock::now();
        amer::AlSolveStatus st =
            amer::al_solve_put_boundary_prepared(*geo, g.S, sig[s], sc->bnd, sc->ws, &sc->loaded);
        solve_us.push_back(us_since(t0));
        benchmark::DoNotOptimize(st);
      }
      for (unsigned s0 = 0; s0 < kNs; s0 += 4) {
        const double K4[4] = {g.S, g.S, g.S, g.S};
        const double r4[4] = {rp, rp, rp, rp};
        const double q4[4] = {qp, qp, qp, qp};
        t0 = std::chrono::steady_clock::now();
        const unsigned mask = simd::detail::solve_put_boundaries4_avx2(
            K4, g.T, &sig[s0], r4, q4, sch, pb.get(), pw.get(), *pg);
        pack_us.push_back(us_since(t0));
        declined += (mask == 0xFu) ? 0.0 : 1.0;
      }
    }
  }
  const AlExpiryGeometry egeo = prepare_al_geometry(g.T, g.r, 0.5 * (g.q_lo + g.q_hi));
  AlSigmaQFamily qf(egeo, g.side, g.S);
  std::vector<double> prep_us;
  std::vector<double> prep_check_us;
  {
    const SqIsaScope scope(simd::SimdIsa::ForceAvx2);
    for (int rep = 0; rep < 5; ++rep) {
      auto t0 = std::chrono::steady_clock::now();
      const Status a = qf.prepare(g.s_lo, g.s_hi, kNs, g.q_lo, g.q_hi, kNq);
      prep_us.push_back(us_since(t0));
      t0 = std::chrono::steady_clock::now();
      const Status b = qf.prepare(g.s_lo, g.s_hi, kNs, g.q_lo, g.q_hi, kNq,
                                  AlSigmaFamilyAxis::LogSigma, AlSigmaQFamilyCheck::OffNode);
      prep_check_us.push_back(us_since(t0));
      if (!a || !b) {
        state.SkipWithError("prepare refused");
        return;
      }
    }
  }
  const double pack = median_of(pack_us);
  const double prep = median_of(prep_us);
  state.counters["T"] = g.T;
  state.counters["solve_scalar_us_p50"] = median_of(solve_us);
  state.counters["pack4_us_p50"] = pack;
  state.counters["pack_us_per_node"] = 0.25 * pack;
  state.counters["geometry_us_p50"] = median_of(geo_us);
  state.counters["prepare_avx2_us_p50"] = prep;
  state.counters["prepare_minus_packs_us"] = prep - 12.0 * pack;
  state.counters["self_check_extra_us"] = median_of(prep_check_us) - prep;
  state.counters["self_check_max_rel"] = qf.self_check().max_rel;
  state.counters["packs_declined"] = declined;
}

void run_sqbuild_strikes(benchmark::State &state) {
  using namespace atx::vol;
  const SqData &d = sq_data(SqSet::Spy23);
  if (d.pts == nullptr || d.groups.empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  constexpr std::size_t kStrikes = 300;
  std::vector<double> batch_us;
  std::vector<double> single_us;
  double mismatched = 0.0;
  double sides = 0.0;
  for (auto _ : state) {
    for (const SqGroup &g : d.groups) {
      const AlExpiryGeometry geo = prepare_al_geometry(g.T, g.r, 0.5 * (g.q_lo + g.q_hi));
      AlSigmaQFamily qf(geo, g.side, g.S);
      if (!qf.prepare(g.s_lo, g.s_hi, 12, g.q_lo, g.q_hi, 4)) {
        continue;
      }
      sides += 1.0;
      std::vector<double> K(kStrikes);
      for (std::size_t i = 0; i < kStrikes; ++i) {
        K[i] = g.S * (0.7 + 0.6 * static_cast<double>(i) / static_cast<double>(kStrikes - 1));
      }
      const double sigma = std::sqrt(g.s_lo * g.s_hi);
      const double q = 0.5 * (g.q_lo + g.q_hi);
      std::vector<double> out(kStrikes, 0.0);
      std::vector<double> one(kStrikes, 0.0);
      for (int rep = 0; rep < 21; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        AlSigmaQFamilyStatus st = qf.price_strikes(g.S, K, sigma, q, out);
        batch_us.push_back(us_since(t0));
        benchmark::DoNotOptimize(st);
        t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < kStrikes; ++i) {
          one[i] = qf.price(g.S, K[i], sigma, q).price;
        }
        single_us.push_back(us_since(t0));
        benchmark::DoNotOptimize(one.data());
      }
      for (std::size_t i = 0; i < kStrikes; ++i) {
        const bool same =
            std::bit_cast<std::uint64_t>(out[i]) == std::bit_cast<std::uint64_t>(one[i]);
        mismatched += same ? 0.0 : 1.0;
      }
    }
  }
  const double b = median_of(batch_us);
  const double s = median_of(single_us);
  state.counters["sides"] = sides;
  state.counters["strikes"] = static_cast<double>(kStrikes);
  state.counters["batch_us_300"] = b;
  state.counters["single_us_300"] = s;
  state.counters["batch_us_per_strike"] = b / static_cast<double>(kStrikes);
  state.counters["single_us_per_strike"] = s / static_cast<double>(kStrikes);
  state.counters["speedup"] = (b > 0.0) ? s / b : 0.0;
  state.counters["mismatched"] = mismatched;
}

const bool kRegisteredSqBuild = [] {
  if (env_text("ATX_VOL_AL_SQBUILD").empty()) {
    return false;
  }
  const std::pair<const char *, SqSet> sets[] = {
      {"spy23", SqSet::Spy23}, {"spylong", SqSet::SpyLong}, {"spyall", SqSet::SpyAll}};
  const char *envs[] = {"ATX_VOL_AL_SPY23", "ATX_VOL_AL_SPYLONG", "ATX_VOL_AL_SPYALL"};
  const std::pair<unsigned, unsigned> shapes[] = {{3u, 12u}, {4u, 12u}, {4u, 16u}};
  for (std::size_t si = 0; si < 3; ++si) {
    if (env_text(envs[si]).empty()) {
      continue;
    }
    const SqSet s = sets[si].second;
    const char *set_name = sets[si].first;
    for (const bool avx2 : {false, true}) {
      for (const auto &[nq_v, ns_v] : shapes) {
        const unsigned n_q = nq_v;
        const unsigned n_sigma = ns_v;
        benchmark::RegisterBenchmark(std::string("american/sqbuild/") + set_name + "/" +
                                         (avx2 ? "avx2" : "scalar") + "/nq" +
                                         std::to_string(n_q) + "/ns" + std::to_string(n_sigma),
                                     [s, set_name, avx2, n_q, n_sigma](benchmark::State &state) {
                                       run_sqbuild(state, s, set_name, avx2, n_q, n_sigma);
                                     })
            ->Iterations(1)
            ->Unit(benchmark::kMillisecond);
      }
    }
    if (s != SqSet::SpyAll) {
      benchmark::RegisterBenchmark(std::string("american/sqbuild/parts/") + set_name,
                                   [s](benchmark::State &state) { run_sqbuild_parts(state, s); })
          ->Iterations(1)
          ->Unit(benchmark::kMillisecond);
    }
    if (s == SqSet::Spy23) {
      benchmark::RegisterBenchmark("american/sqbuild/spy23/price_strikes300",
                                   [](benchmark::State &state) { run_sqbuild_strikes(state); })
          ->Iterations(1)
          ->Unit(benchmark::kMillisecond);
    }
  }
  return true;
}();

// ── K1 (r5, 2026-10-22): fused price + vega cost table on the (σ, q) family ─────────────
//
//   * ATX_VOL_AL_SQFUSED=1 registers (inputs: the sqfamily env files above):
//       american/sqfused/<set>/nq4/ns<M>  for M in {12, 16}. One family per (S, T, r, side)
//         group as run_sq_group lays it out (default ISA build: the AVX2 nodes on an AVX2
//         host), every contract at its OWN q at the group's σ grid. Per family (median of
//         5): build_us (prepare), table_us (build_premium_table into a fresh table: the
//         time-grid precontraction), tile_us (contract_q, per distinct q). Per query (3
//         timed passes each, p50): price_us (price(): price_on, the default), chord_us
//         (price_and_chord: the current two-price slope), fused_po_us (the one-shot
//         stages with want_vega false: the fused PRICE-only), fused_us (price_and_vega:
//         one-shot price + vega), pre_us (price_vega_on on a prebuilt precontracted
//         tile), strikes_us_per_strike (price_vega_strikes over
//         the group's strikes at one (σ, q)), and the scalar baseline on a sample of 16
//         queries per group: scalar_price_us (one cold andersen_lake at the contract's q)
//         and scalar_pv_us (two cold solves, σ and σ + kAlPackSigmaBump: the scalar price
//         + slope). Break-even (evaluations per family at which build + N fused calls
//         beat N scalar price + vega calls): breakeven_oneshot = build / (scalar_pv −
//         fused), breakeven_table = (build + table + tile) / (scalar_pv − pre).
//         Memory: state_bytes (family), table_bytes, tile_bytes. Accuracy: the fused vs
//         price() max |dP| / max(P, 1).

void run_sqfused(benchmark::State &state, SqSet set, unsigned n_sigma) {
  using namespace atx::vol;
  namespace sq = sigma_q_family_detail;
  const SqData &d = sq_data(set);
  if (d.pts == nullptr || d.pts->empty()) {
    state.SkipWithError("no contracts (unreadable or empty input file)");
    return;
  }
  const std::vector<GridPt> &pts = *d.pts;
  constexpr unsigned kNq = 4;
  std::vector<double> build_us, table_us, tile_us, state_b, table_b;
  std::vector<double> price_us, chord_us, fused_po_us, fused_us, pre_us, strike_us;
  std::vector<double> scalar_price_us, scalar_pv_us, be_oneshot, be_table;
  double cold_groups = 0.0;
  double max_dp = 0.0;
  double queries = 0.0;
  for (auto _ : state) {
    for (const SqGroup &g : d.groups) {
      const AlExpiryGeometry geo = prepare_al_geometry(g.T, g.r, 0.5 * (g.q_lo + g.q_hi));
      AlSigmaQFamily qf(geo, g.side, g.S);
      std::vector<double> reps;
      for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = qf.prepare(g.s_lo, g.s_hi, static_cast<std::uint16_t>(n_sigma), g.q_lo,
                                     g.q_hi, static_cast<std::uint16_t>(kNq));
        const double us = us_since(t0);
        if (!st) {
          reps.clear();
          break;
        }
        reps.push_back(us);
      }
      if (reps.empty()) {
        cold_groups += 1.0;
        continue;
      }
      const double b_us = median_of(reps);
      build_us.push_back(b_us);
      state_b.push_back(static_cast<double>(qf.state_bytes()));
      // The time-grid precontraction: a fresh table per rep (its allocation included).
      std::vector<double> treps;
      std::unique_ptr<sq::PremiumTable> table;
      for (int rep = 0; rep < 5; ++rep) {
        auto t = std::make_unique<sq::PremiumTable>();
        const auto t0 = std::chrono::steady_clock::now();
        const sq::FusedStatus ts = sq::build_premium_table(qf, *t);
        treps.push_back(us_since(t0));
        if (ts != sq::FusedStatus::Ok) {
          state.SkipWithError("build_premium_table refused");
          return;
        }
        table = std::move(t);
      }
      const double t_us = median_of(treps);
      table_us.push_back(t_us);
      table_b.push_back(static_cast<double>(table->bytes()));
      // One precontracted tile per distinct q of the group.
      std::map<double, sq::QTile> tiles;
      std::vector<double> tile_reps;
      for (const std::size_t i : g.idx) {
        if (tiles.count(pts[i].q) != 0u) {
          continue;
        }
        auto &slot = tiles[pts[i].q];
        const auto t0 = std::chrono::steady_clock::now();
        const sq::FusedStatus a = sq::contract_q(*table, pts[i].q, slot);
        tile_reps.push_back(us_since(t0));
        if (a != sq::FusedStatus::Ok) {
          state.SkipWithError("contract_q refused");
          return;
        }
      }
      const double tl_us = median_of(tile_reps);
      tile_us.push_back(tl_us);
      const auto *st = sq::QFamilyAccess::state(qf);
      std::vector<double> g_fused;
      std::vector<double> g_pre;
      for (const std::size_t i : g.idx) {
        const GridPt &p = pts[i];
        const auto &tl = tiles.at(p.q);
        for (const double sigma : g.sigmas) {
          queries += 1.0;
          for (int pass = 0; pass < 3; ++pass) {
            auto t0 = std::chrono::steady_clock::now();
            AlSigmaQFamilyPrice pp = qf.price(p.S, p.K, sigma, p.q);
            price_us.push_back(us_since(t0));
            benchmark::DoNotOptimize(pp);
            t0 = std::chrono::steady_clock::now();
            AlSigmaQFamilyChord ch = qf.price_and_chord(p.S, p.K, sigma, p.q);
            chord_us.push_back(us_since(t0));
            benchmark::DoNotOptimize(ch);
            t0 = std::chrono::steady_clock::now();
            {
              sq::PremiumNodes pn;
              (void)st->lay_premium_nodes(pn);
              sq::QCarry c;
              sq::carry_at(pn, g.side, g.r, p.q, c);
              sq::NodeBoundary nb;
              st->boundary_at_nodes(pn, sigma, p.q, nb);
              const bool call = g.side == Side::Call;
              sq::FusedPriceVega po = sq::premium_pass(
                  pn, c, nb, call ? p.K : p.S, call ? p.S : p.K, st->ws->fast, false);
              benchmark::DoNotOptimize(po);
            }
            fused_po_us.push_back(us_since(t0));
            t0 = std::chrono::steady_clock::now();
            sq::FusedPriceVega fv = sq::price_and_vega(qf, p.S, p.K, sigma, p.q);
            const double fu = us_since(t0);
            fused_us.push_back(fu);
            g_fused.push_back(fu);
            benchmark::DoNotOptimize(fv);
            t0 = std::chrono::steady_clock::now();
            sq::FusedPriceVega tv = sq::price_vega_on(*table, tl, p.S, p.K, sigma);
            const double pu = us_since(t0);
            pre_us.push_back(pu);
            g_pre.push_back(pu);
            benchmark::DoNotOptimize(tv);
            if (pass == 0 && pp.status == AlSigmaQFamilyStatus::Ok) {
              max_dp = std::max(max_dp, std::abs(fv.price - pp.price) / std::max(pp.price, 1.0));
            }
          }
        }
      }
      // The strike vector at one (σ, q): every strike of the group.
      {
        std::vector<double> K;
        for (const std::size_t i : g.idx) {
          K.push_back(pts[i].K);
        }
        std::vector<double> op(K.size(), 0.0);
        std::vector<double> ov(K.size(), 0.0);
        const double sigma = std::sqrt(g.s_lo * g.s_hi);
        const double q = 0.5 * (g.q_lo + g.q_hi);
        std::vector<double> sreps;
        for (int rep = 0; rep < 5; ++rep) {
          const auto t0 = std::chrono::steady_clock::now();
          sq::FusedStatus fs = sq::price_vega_strikes(qf, g.S, K, sigma, q, op, ov);
          sreps.push_back(us_since(t0) / static_cast<double>(K.size()));
          benchmark::DoNotOptimize(fs);
        }
        strike_us.push_back(median_of(sreps));
      }
      // The scalar andersen_lake baseline on 16 spread queries of the group.
      std::vector<double> g_scalar_pv;
      const std::size_t stride = std::max<std::size_t>(1u, g.idx.size() / 8u);
      for (std::size_t l = 0; l < g.idx.size() && g_scalar_pv.size() < 16u; l += stride) {
        const GridPt &p = pts[g.idx[l]];
        for (const double sigma : {g.sigmas.front(), g.sigmas[g.sigmas.size() / 2]}) {
          auto t0 = std::chrono::steady_clock::now();
          Result<double> a = andersen_lake(p.S, p.K, p.T, sigma, p.r, p.q, p.side);
          scalar_price_us.push_back(us_since(t0));
          benchmark::DoNotOptimize(a);
          t0 = std::chrono::steady_clock::now();
          Result<double> b0 = andersen_lake(p.S, p.K, p.T, sigma, p.r, p.q, p.side);
          Result<double> b1 =
              andersen_lake(p.S, p.K, p.T, sigma + kAlPackSigmaBump, p.r, p.q, p.side);
          const double pv = us_since(t0);
          benchmark::DoNotOptimize(b0);
          benchmark::DoNotOptimize(b1);
          scalar_pv_us.push_back(pv);
          g_scalar_pv.push_back(pv);
        }
      }
      const double s_pv = median_of(g_scalar_pv);
      const double f_pv = median_of(g_fused);
      const double t_pv = median_of(g_pre);
      if (s_pv > f_pv) {
        be_oneshot.push_back(b_us / (s_pv - f_pv));
      }
      if (s_pv > t_pv) {
        be_table.push_back((b_us + t_us + tl_us * static_cast<double>(tiles.size())) /
                           (s_pv - t_pv));
      }
    }
  }
  double build_total = 0.0;
  double table_total = 0.0;
  for (const double b : build_us) {
    build_total += b;
  }
  for (const double t : table_us) {
    table_total += t;
  }
  for (std::vector<double> *v :
       {&price_us, &chord_us, &fused_po_us, &fused_us, &pre_us, &scalar_price_us, &scalar_pv_us}) {
    std::sort(v->begin(), v->end());
  }
  state.counters["families"] = static_cast<double>(build_us.size());
  state.counters["cold_groups"] = cold_groups;
  state.counters["queries"] = queries;
  state.counters["n_sigma"] = static_cast<double>(n_sigma);
  state.counters["build_us_p50"] = median_of(build_us);
  state.counters["build_ms_total"] = build_total * 1.0e-3;
  state.counters["table_us_p50"] = median_of(table_us);
  state.counters["table_ms_total"] = table_total * 1.0e-3;
  state.counters["tile_us_p50"] = median_of(tile_us);
  state.counters["state_bytes_p50"] = median_of(state_b);
  state.counters["table_bytes_p50"] = median_of(table_b);
  state.counters["tile_bytes"] = static_cast<double>(sizeof(sq::QTile));
  state.counters["price_us_p50"] = sorted_quantile(price_us, 0.50);
  state.counters["price_us_p99"] = sorted_quantile(price_us, 0.99);
  state.counters["chord_us_p50"] = sorted_quantile(chord_us, 0.50);
  state.counters["fused_po_us_p50"] = sorted_quantile(fused_po_us, 0.50);
  state.counters["fused_us_p50"] = sorted_quantile(fused_us, 0.50);
  state.counters["fused_us_p99"] = sorted_quantile(fused_us, 0.99);
  state.counters["pre_us_p50"] = sorted_quantile(pre_us, 0.50);
  state.counters["pre_us_p99"] = sorted_quantile(pre_us, 0.99);
  state.counters["strikes_us_per_strike"] = median_of(strike_us);
  state.counters["scalar_price_us_p50"] = sorted_quantile(scalar_price_us, 0.50);
  state.counters["scalar_pv_us_p50"] = sorted_quantile(scalar_pv_us, 0.50);
  state.counters["breakeven_oneshot_p50"] = median_of(be_oneshot);
  state.counters["breakeven_table_p50"] = median_of(be_table);
  state.counters["fused_vs_price_max_scaled"] = max_dp;
}

const bool kRegisteredSqFused = [] {
  if (env_text("ATX_VOL_AL_SQFUSED").empty()) {
    return false;
  }
  const std::pair<const char *, SqSet> sets[] = {
      {"spy23", SqSet::Spy23}, {"spylong", SqSet::SpyLong}, {"spyall", SqSet::SpyAll}};
  const char *envs[] = {"ATX_VOL_AL_SPY23", "ATX_VOL_AL_SPYLONG", "ATX_VOL_AL_SPYALL"};
  for (std::size_t si = 0; si < 3; ++si) {
    if (env_text(envs[si]).empty()) {
      continue;
    }
    const SqSet s = sets[si].second;
    for (const unsigned n_sigma : {12u, 16u}) {
      benchmark::RegisterBenchmark(std::string("american/sqfused/") + sets[si].first + "/nq4/ns" +
                                       std::to_string(n_sigma),
                                   [s, n_sigma](benchmark::State &state) {
                                     run_sqfused(state, s, n_sigma);
                                   })
          ->Iterations(1)
          ->Unit(benchmark::kMillisecond);
    }
  }
  return true;
}();

// ── A0 (r6, 2026-10-23): the (σ, q) family's primary Greek bundle — µs per bundle / chain ──
//
//   * ATX_VOL_AL_SQGREEKS=1 registers:
//       american/sqgreeks/spy23/nq4/ns12 (needs ATX_VOL_AL_SPY23): one family per (S, T,
//         r, side) group as run_sqfused lays it out (default ISA build), every contract at
//         its OWN q at the group's σ grid, 3 timed passes each, p50 / p99 of: price_us
//         (price()), fused_us (price_and_vega), primary_us (family_greeks, primary():
//         Δ Γ vega θ phi), all_us (family_greeks with the group's rate-bump set, all():
//         + rho). Per group: build_us (prepare), bumps_us (the rate-bump set: two more
//         family builds). price_bits_differ: rows whose bundle price is not K1's fused
//         price bit for bit (0 by contract).
//       american/sqgreeks_chain/<chain>/<mask>  chain in spy23 (317 rows at their own q;
//         ATX_VOL_AL_SPY23) | synthetic300 (S 773, 23 d, 300 strikes 540..1090 on the
//         representative skew, puts below spot and calls above, each strike at its own q
//         spread over [0.00155, 0.00170] like a funding-pv chain); one put and one call
//         family on the chain's (σ, q) box; mask primary | all (the rate-bump sets built
//         outside the timed call). 5 warm-up then 101 timed chain evaluations (both sides'
//         family_greeks_many): chain_us p50 / p99 / max and per-lane µs.

void run_sqgreeks(benchmark::State &state, unsigned n_sigma) {
  using namespace atx::vol;
  namespace sq = sigma_q_family_detail;
  const SqData &d = sq_data(SqSet::Spy23);
  if (d.pts == nullptr || d.pts->empty()) {
    state.SkipWithError("no contracts (ATX_VOL_AL_SPY23 unreadable or empty)");
    return;
  }
  const std::vector<GridPt> &pts = *d.pts;
  constexpr unsigned kNq = 4;
  std::vector<double> build_us, bumps_us, price_us, fused_us, primary_us, all_us;
  double bits_differ = 0.0;
  double cold_groups = 0.0;
  double queries = 0.0;
  double refused = 0.0;
  double bumps_refused = 0.0;
  AlSigmaQGreeksRequest primary;
  AlSigmaQGreeksRequest all;
  all.mask = AlFamilyGreekMask::all();
  std::uint64_t fold = 1469598103934665603ULL;
  for (auto _ : state) {
    for (const SqGroup &g : d.groups) {
      const AlExpiryGeometry geo = prepare_al_geometry(g.T, g.r, 0.5 * (g.q_lo + g.q_hi));
      AlSigmaQFamily qf(geo, g.side, g.S);
      auto t0 = std::chrono::steady_clock::now();
      const Status st = qf.prepare(g.s_lo, g.s_hi, static_cast<std::uint16_t>(n_sigma), g.q_lo,
                                   g.q_hi, static_cast<std::uint16_t>(kNq));
      const double b_us = us_since(t0);
      if (!st) {
        cold_groups += 1.0;
        continue;
      }
      build_us.push_back(b_us);
      AlSigmaQFamilyRateBumps bumps;
      t0 = std::chrono::steady_clock::now();
      const bool bok = bumps.prepare(qf).has_value();
      bumps_us.push_back(us_since(t0));
      bumps_refused += bok ? 0.0 : 1.0;
      for (const std::size_t i : g.idx) {
        const GridPt &p = pts[i];
        for (const double sigma : g.sigmas) {
          queries += 1.0;
          for (int pass = 0; pass < 3; ++pass) {
            t0 = std::chrono::steady_clock::now();
            AlSigmaQFamilyPrice pp = qf.price(p.S, p.K, sigma, p.q);
            price_us.push_back(us_since(t0));
            benchmark::DoNotOptimize(pp);
            t0 = std::chrono::steady_clock::now();
            sq::FusedPriceVega fv = sq::price_and_vega(qf, p.S, p.K, sigma, p.q);
            fused_us.push_back(us_since(t0));
            benchmark::DoNotOptimize(fv);
            t0 = std::chrono::steady_clock::now();
            AlSigmaQFamilyGreeks gp = family_greeks(qf, p.S, p.K, sigma, p.q, primary);
            primary_us.push_back(us_since(t0));
            benchmark::DoNotOptimize(gp);
            t0 = std::chrono::steady_clock::now();
            AlSigmaQFamilyGreeks ga = bok ? family_greeks(qf, bumps, p.S, p.K, sigma, p.q, all)
                                          : family_greeks(qf, p.S, p.K, sigma, p.q, all);
            all_us.push_back(us_since(t0));
            benchmark::DoNotOptimize(ga);
            if (pass == 0) {
              for (const double x : {gp.price, gp.delta, gp.gamma, gp.vega, gp.theta, gp.phi,
                                     ga.rho}) {
                fold_bits(fold, x);
              }
              refused += gp.status == AlSigmaQFamilyStatus::Ok ? 0.0 : 1.0;
              const auto bits = [](double x) { return std::bit_cast<std::uint64_t>(x); };
              if (gp.status == AlSigmaQFamilyStatus::Ok && bits(gp.price) != bits(fv.price)) {
                bits_differ += 1.0;
              }
            }
          }
        }
      }
    }
  }
  for (std::vector<double> *v : {&price_us, &fused_us, &primary_us, &all_us}) {
    std::sort(v->begin(), v->end());
  }
  state.counters["families"] = static_cast<double>(build_us.size());
  state.counters["cold_groups"] = cold_groups;
  state.counters["queries"] = queries;
  state.counters["refused"] = refused;
  state.counters["bumps_refused"] = bumps_refused;
  state.counters["n_sigma"] = static_cast<double>(n_sigma);
  state.counters["build_us_p50"] = median_of(build_us);
  state.counters["bumps_us_p50"] = median_of(bumps_us);
  state.counters["price_us_p50"] = sorted_quantile(price_us, 0.50);
  state.counters["fused_us_p50"] = sorted_quantile(fused_us, 0.50);
  state.counters["primary_us_p50"] = sorted_quantile(primary_us, 0.50);
  state.counters["primary_us_p99"] = sorted_quantile(primary_us, 0.99);
  state.counters["all_us_p50"] = sorted_quantile(all_us, 0.50);
  state.counters["all_us_p99"] = sorted_quantile(all_us, 0.99);
  state.counters["price_bits_differ"] = bits_differ;
  state.counters["bits_fnv52"] = bits52(fold);
}

void run_sqgreeks_chain(benchmark::State &state, bool spy, bool all_mask) {
  using namespace atx::vol;
  const ChainInputs c = chain_inputs(spy);
  if (c.K.empty()) {
    state.SkipWithError("no chain (ATX_VOL_AL_SPY23 unreadable or empty)");
    return;
  }
  const std::size_t n = c.K.size();
  std::vector<double> q(n);
  if (spy) {
    const std::vector<GridPt> &pts = err_points(ErrSet::Spy23);
    for (std::size_t i = 0; i < n; ++i) {
      q[i] = pts[i].q;
    }
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      q[i] = 0.00155 + 0.00015 * static_cast<double>((i * 37u) % n) / static_cast<double>(n - 1);
    }
  }
  const double q_lo = *std::min_element(q.begin(), q.end());
  const double q_hi = *std::max_element(q.begin(), q.end());
  const AlExpiryGeometry g = prepare_al_geometry(c.T, c.r, 0.5 * (q_lo + q_hi));
  AlSigmaQFamily put(g, Side::Put, c.S);
  AlSigmaQFamily call(g, Side::Call, c.S);
  const auto t_build = std::chrono::steady_clock::now();
  const bool put_ok = put.prepare(c.lo, c.hi, 12, q_lo, q_hi, 4).has_value();
  const bool call_ok = call.prepare(c.lo, c.hi, 12, q_lo, q_hi, 4).has_value();
  const double build_ms = us_since(t_build) * 1.0e-3;
  if (!put_ok || !call_ok) {
    state.SkipWithError("a chain family refused its box");
    return;
  }
  AlSigmaQFamilyRateBumps put_bumps;
  AlSigmaQFamilyRateBumps call_bumps;
  double bumps_ms = 0.0;
  if (all_mask) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool a = put_bumps.prepare(put).has_value();
    const bool b = call_bumps.prepare(call).has_value();
    bumps_ms = us_since(t0) * 1.0e-3;
    if (!a || !b) {
      state.SkipWithError("a rate-bump set refused");
      return;
    }
  }
  std::vector<std::uint8_t> active_put(n), active_call(n);
  for (std::size_t i = 0; i < n; ++i) {
    active_put[i] = c.side[i] == Side::Put ? 1u : 0u;
    active_call[i] = c.side[i] == Side::Call ? 1u : 0u;
  }
  std::vector<double> price(n), delta(n), gamma(n), vega(n), theta(n), rho(n), phi(n);
  std::vector<AlSigmaQFamilyStatus> status(n);
  std::vector<AlFamilyGreekMask> supported(n);
  AlSigmaQGreekColumns cols;
  cols.price = price;
  cols.delta = delta;
  cols.gamma = gamma;
  cols.vega = vega;
  cols.theta = theta;
  cols.rho = rho;
  cols.phi = phi;
  cols.status = status;
  cols.supported = supported;
  AlSigmaQGreeksRequest req;
  if (all_mask) {
    req.mask = AlFamilyGreekMask::all();
  }
  const auto chain = [&] {
    const Status a = family_greeks_many(put, c.S, c.K, c.sigma, q, active_put, cols, req,
                                        all_mask ? &put_bumps : nullptr);
    const Status b = family_greeks_many(call, c.S, c.K, c.sigma, q, active_call, cols, req,
                                        all_mask ? &call_bumps : nullptr);
    return a.has_value() && b.has_value();
  };
  std::vector<double> chain_us;
  for (auto _ : state) {
    for (int w = 0; w < 5; ++w) {
      if (!chain()) {
        state.SkipWithError("family_greeks_many refused");
        return;
      }
    }
    for (int it = 0; it < 101; ++it) {
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = chain();
      chain_us.push_back(us_since(t0));
      benchmark::DoNotOptimize(price.data());
      if (!ok) {
        state.SkipWithError("family_greeks_many refused");
        return;
      }
    }
  }
  std::sort(chain_us.begin(), chain_us.end());
  double refused = 0.0;
  double full = 0.0;
  std::uint64_t fold = 1469598103934665603ULL;
  for (std::size_t i = 0; i < n; ++i) {
    refused += status[i] == AlSigmaQFamilyStatus::Ok ? 0.0 : 1.0;
    full += supported[i].bits == req.mask.bits ? 1.0 : 0.0;
    for (const double x : {price[i], delta[i], gamma[i], vega[i], theta[i], rho[i], phi[i]}) {
      fold_bits(fold, x);
    }
  }
  state.counters["bits_fnv52"] = bits52(fold);
  state.counters["rows"] = static_cast<double>(n);
  state.counters["refused"] = refused;
  state.counters["rows_every_greek_supported"] = full;
  state.counters["build_ms_both_sides"] = build_ms;
  state.counters["bumps_ms_both_sides"] = bumps_ms;
  state.counters["chain_us_p50"] = sorted_quantile(chain_us, 0.50);
  state.counters["chain_us_p99"] = sorted_quantile(chain_us, 0.99);
  state.counters["chain_us_max"] = chain_us.empty() ? 0.0 : chain_us.back();
  state.counters["lane_us_p50"] = sorted_quantile(chain_us, 0.50) / static_cast<double>(n);
}

const bool kRegisteredSqGreeks = [] {
  if (env_text("ATX_VOL_AL_SQGREEKS").empty()) {
    return false;
  }
  const bool spy = !env_text("ATX_VOL_AL_SPY23").empty();
  if (spy) {
    benchmark::RegisterBenchmark("american/sqgreeks/spy23/nq4/ns12",
                                 [](benchmark::State &state) { run_sqgreeks(state, 12); })
        ->Iterations(1)
        ->Unit(benchmark::kMillisecond);
  }
  for (const bool chain_spy : {true, false}) {
    if (chain_spy && !spy) {
      continue;
    }
    for (const bool all_mask : {false, true}) {
      benchmark::RegisterBenchmark(std::string("american/sqgreeks_chain/") +
                                       (chain_spy ? "spy23/" : "synthetic300/") +
                                       (all_mask ? "all" : "primary"),
                                   [chain_spy, all_mask](benchmark::State &state) {
                                     run_sqgreeks_chain(state, chain_spy, all_mask);
                                   })
          ->Iterations(1)
          ->Unit(benchmark::kMillisecond);
    }
  }
  return true;
}();

const bool kRegistered = [] {
  using atx::vol::bench::apply_common;
  for (const Rung &rung : rungs()) {
    const std::string name = std::string("american/ladder/") + rung.name;
    apply_common(benchmark::RegisterBenchmark(
                     name, [&rung](benchmark::State &state) { run_rung(state, rung); }))
        ->Unit(benchmark::kMicrosecond);
  }
  for (const Preset &preset : presets()) {
    const std::string name = std::string("american/ladder_latency/") + preset.name;
    apply_common(benchmark::RegisterBenchmark(
                     name, [&preset](benchmark::State &state) { run_latency(state, preset); }))
        ->UseManualTime()
        ->Unit(benchmark::kMicrosecond);
  }
  if (!env_text("ATX_VOL_AL_DEV200").empty()) {
    for (const Preset &preset : presets()) {
      const std::string name = std::string("american/ladder_latency_dev200/") + preset.name;
      apply_common(benchmark::RegisterBenchmark(name,
                                                [&preset](benchmark::State &state) {
                                                  run_latency_on(
                                                      state, err_points(ErrSet::Dev200), preset);
                                                }))
          ->UseManualTime()
          ->Unit(benchmark::kMicrosecond);
    }
  }
  for (const Preset &preset : presets()) {
    const std::string name = std::string("american/greeks_latency/") + preset.name;
    apply_common(benchmark::RegisterBenchmark(
                     name,
                     [&preset](benchmark::State &state) { run_greeks_latency(state, preset); }))
        ->UseManualTime()
        ->Unit(benchmark::kMicrosecond);
  }
  if (!env_text("ATX_VOL_AL_FAST_ERR").empty()) {
    std::vector<std::pair<const char *, ErrSet>> sets = {{"grid", ErrSet::Grid},
                                                         {"random", ErrSet::Random}};
    if (!env_text("ATX_VOL_AL_DEV200").empty()) {
      sets.emplace_back("dev200", ErrSet::Dev200);
    }
    for (const auto &[label, set] : sets) {
      for (std::size_t i = 0; i < reference_preset_count(); ++i) {
        const Preset &preset = presets()[i];
        const ErrSet s = set;
        const std::string tail = std::string(label) + "/" + preset.name;
        benchmark::RegisterBenchmark(
            "american/fast_profile_error/" + tail,
            [s, &preset](benchmark::State &state) { run_fast_error(state, s, preset); })
            ->Iterations(1)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark(
            "american/fast_profile_error/greeks/" + tail,
            [s, &preset](benchmark::State &state) { run_fast_greeks_error(state, s, preset); })
            ->Iterations(1)
            ->Unit(benchmark::kMillisecond);
      }
    }
  }
  if (!env_text("ATX_VOL_AL_SPY23").empty()) {
    for (const Preset &preset : presets()) {
      const std::string tail = preset.name;
      apply_common(benchmark::RegisterBenchmark("american/spy23_latency/" + tail,
                                                [&preset](benchmark::State &state) {
                                                  run_latency_on(
                                                      state, err_points(ErrSet::Spy23), preset);
                                                }))
          ->UseManualTime()
          ->Unit(benchmark::kMicrosecond);
      benchmark::RegisterBenchmark("american/spy23_profile/" + tail,
                                   [&preset](benchmark::State &state) {
                                     run_profile_on(state, err_points(ErrSet::Spy23), preset);
                                   })
          ->MinTime(1.0)
          ->Unit(benchmark::kMicrosecond);
      benchmark::RegisterBenchmark("american/spy23_iv/" + tail,
                                   [&preset](benchmark::State &state) {
                                     run_iv_on(state, err_points(ErrSet::Spy23), preset);
                                   })
          ->Iterations(3)
          ->UseManualTime()
          ->Unit(benchmark::kMicrosecond);
    }
  }
  if (!env_text("ATX_VOL_AL_SCREEN_SCAN").empty()) {
    std::vector<std::pair<const char *, ErrSet>> sets = {{"grid", ErrSet::Grid},
                                                         {"random", ErrSet::Random}};
    if (!env_text("ATX_VOL_AL_SPY23").empty()) {
      sets.emplace_back("spy23", ErrSet::Spy23);
    }
    if (!env_text("ATX_VOL_AL_DEV200").empty()) {
      sets.emplace_back("dev200", ErrSet::Dev200);
    }
    for (const auto &[label, set] : sets) {
      for (const ScreenCandidate &cand : screen_candidates()) {
        const ErrSet s = set;
        const char *set_name = label;
        benchmark::RegisterBenchmark(std::string("american/screen_scan/") + label + "/" +
                                         cand.name,
                                     [s, set_name, &cand](benchmark::State &state) {
                                       run_screen_scan(state, s, set_name, cand);
                                     })
            ->Iterations(1)
            ->Unit(benchmark::kMillisecond);
      }
    }
  }
  if (!env_text("ATX_VOL_AL_FAMILY").empty()) {
    using atx::vol::AlSigmaFamilyAxis;
    std::vector<std::pair<const char *, FamilySet>> sets = {
        {"grid", FamilySet::Grid}, {"fastgrid", FamilySet::FastGrid},
        {"deepcarry", FamilySet::DeepCarry}};
    if (!env_text("ATX_VOL_AL_DEV200").empty()) {
      sets.emplace_back("dev200", FamilySet::Dev200);
    }
    if (!env_text("ATX_VOL_AL_SPY23").empty()) {
      sets.emplace_back("spy23offnode", FamilySet::Spy23OffNode);
    }
    const std::pair<const char *, AlSigmaFamilyAxis> axes[] = {
        {"sigma", AlSigmaFamilyAxis::Sigma}, {"logsigma", AlSigmaFamilyAxis::LogSigma}};
    for (const unsigned n : {8u, 12u, 16u}) {
      for (const auto &[axis_name, axis] : axes) {
        const AlSigmaFamilyAxis ax = axis;
        if (!env_text("ATX_VOL_AL_SPY23").empty()) {
          benchmark::RegisterBenchmark(std::string("american/spy23_family/") + axis_name + "/n" +
                                           std::to_string(n),
                                       [n, ax](benchmark::State &state) {
                                         run_family(state, FamilySet::Spy23, "spy23", n, ax);
                                       })
              ->Iterations(1)
              ->Unit(benchmark::kMillisecond);
        }
        for (const auto &[label, set] : sets) {
          const FamilySet s = set;
          const char *set_name = label;
          benchmark::RegisterBenchmark(std::string("american/family_error/") + label + "/" +
                                           axis_name + "/n" + std::to_string(n),
                                       [s, set_name, n, ax](benchmark::State &state) {
                                         run_family(state, s, set_name, n, ax);
                                       })
              ->Iterations(1)
              ->Unit(benchmark::kMillisecond);
        }
      }
    }
  }
  if (!env_text("ATX_VOL_AL_FAMILY_GREEKS").empty()) {
    const std::pair<const char *, GreeksRow> rows[] = {
        {"price", GreeksRow::Price},          {"hedge", GreeksRow::Hedge},
        {"eight", GreeksRow::Eight},          {"all", GreeksRow::All},
        {"price_and_vega", GreeksRow::PriceVega}, {"chord", GreeksRow::Chord}};
    const bool spy = !env_text("ATX_VOL_AL_SPY23").empty();
    if (spy) {
      for (const auto &[label, row] : rows) {
        const GreeksRow r = row;
        benchmark::RegisterBenchmark(std::string("american/spy23_family_greeks/") + label,
                                     [r](benchmark::State &state) {
                                       run_spy23_family_greeks(state, r);
                                     })
            ->Iterations(1)
            ->UseManualTime()
            ->Unit(benchmark::kMillisecond);
      }
      benchmark::RegisterBenchmark("american/spy23_family_greeks/reference_bundles",
                                   [](benchmark::State &state) {
                                     run_spy23_reference_bundles(state);
                                   })
          ->Iterations(1)
          ->UseManualTime()
          ->Unit(benchmark::kMillisecond);
    }
    std::vector<std::pair<const char *, FamilySet>> sets = {
        {"grid", FamilySet::Grid}, {"fastgrid", FamilySet::FastGrid},
        {"deepcarry", FamilySet::DeepCarry}};
    if (spy) {
      sets.emplace_back("spy23", FamilySet::Spy23);
      sets.emplace_back("spy23offnode", FamilySet::Spy23OffNode);
    }
    for (const auto &[label, set] : sets) {
      const FamilySet s = set;
      const char *set_name = label;
      for (const unsigned n : {12u, 16u}) {
        if (n == 16u && s != FamilySet::FastGrid) {
          continue;
        }
        benchmark::RegisterBenchmark(std::string("american/family_greeks_error/") + label + "/n" +
                                         std::to_string(n),
                                     [s, set_name, n](benchmark::State &state) {
                                       run_family_greeks_error(state, s, set_name, n);
                                     })
            ->Iterations(1)
            ->Unit(benchmark::kMillisecond);
      }
    }
    for (const bool chain_spy : {true, false}) {
      if (chain_spy && !spy) {
        continue;
      }
      const char *chain_name = chain_spy ? "spy23" : "synthetic300";
      for (const auto &[label, row] : rows) {
        if (row == GreeksRow::PriceVega || row == GreeksRow::Chord) {
          continue;
        }
        const GreeksRow r = row;
        const bool cs = chain_spy;
        benchmark::RegisterBenchmark(std::string("american/family_chain/") + chain_name + "/" + label,
                                     [cs, r](benchmark::State &state) {
                                       run_family_chain(state, cs, r);
                                     })
            ->Iterations(1)
            ->UseManualTime()
            ->Unit(benchmark::kMillisecond);
      }
    }
    benchmark::RegisterBenchmark("american/family_shock/11x11_contract",
                                 [](benchmark::State &state) { run_family_shock(state, false); })
        ->Iterations(1)
        ->UseManualTime()
        ->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark("american/family_shock/11x11_chain300",
                                 [](benchmark::State &state) { run_family_shock(state, true); })
        ->Iterations(1)
        ->UseManualTime()
        ->Unit(benchmark::kMillisecond);
  }
  if (const std::string path = env_text("ATX_VOL_AL_FINGERPRINT"); !path.empty()) {
    benchmark::RegisterBenchmark("american/ladder_fingerprint",
                                 [path](benchmark::State &state) { run_fingerprint(state, path); })
        ->Iterations(1)
        ->Unit(benchmark::kMillisecond);
  }
  if (!env_text("ATX_VOL_AL_PROFILE").empty()) {
    for (const Preset &preset : presets()) {
      const std::string name = std::string("american/ladder_profile/") + preset.name;
      benchmark::RegisterBenchmark(
          name, [&preset](benchmark::State &state) { run_profile(state, preset); })
          ->MinTime(2.0)
          ->Unit(benchmark::kMicrosecond);
    }
    for (const unsigned nb : {7u, 12u}) {
      const std::string name = "american/ladder_profile/seed_census/nb" + std::to_string(nb);
      benchmark::RegisterBenchmark(
          name, [nb](benchmark::State &state) { run_seed_census(state, nb); })
          ->Iterations(1);
    }
    const std::pair<const char *, Fn> fns[] = {{"exp", Fn::Exp},
                                               {"log", Fn::Log},
                                               {"sqrt", Fn::Sqrt},
                                               {"erfc", Fn::Erfc},
                                               {"div", Fn::Div},
                                               {"norm_cdf", Fn::NormCdf},
                                               {"cody_norm_cdf", Fn::CodyNormCdf},
                                               {"cody_exp", Fn::CodyExp}};
    for (const auto &[label, fn] : fns) {
      const std::string name = std::string("american/ladder_profile/libm/") + label;
      const Fn f = fn;
      benchmark::RegisterBenchmark(name, [f](benchmark::State &state) { run_libm(state, f); })
          ->Unit(benchmark::kMicrosecond);
    }
    // alprobe reads its environment in another TU's dynamic initializer, whose order
    // against this one is unspecified, so decide at exit rather than here.
    std::atexit([] {
      if (atx::vol::alprobe::enabled()) {
        atx::vol::alprobe::dump(stderr);
      }
    });
  }
  return true;
}();

} // namespace

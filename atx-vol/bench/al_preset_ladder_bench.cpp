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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_batch.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "pricing/al_probe.hpp"
#include "pricing/american_detail.hpp"
#include "pricing/scalar_erfc.hpp"
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

// Chain-refresh benchmark — pricing hot-path audit 2026-10-16 §4.
//
// Question: after a spot tick, how long does it take to re-mark a listed
// 2,000-contract chain (price + delta + gamma + vega + theta) on ONE pinned core,
// and how accurate is each route against the cold Andersen-Lake reference?
// Target (research Q4 / rec. 8): <= 300 us per tick, single core.
//
// ── Inputs (deterministic) ──────────────────────────────────────────────────
//   * Synthetic chain: S = 600, r = 0.043; 20 expiries T in {1 .. 730} days/365;
//     50 strikes per expiry spaced uniformly in k = ln(K/F) over +/-3 sigma_ATM
//     sqrt(T); calls and puts -> 2,000 contracts. sigma from a fixed eSSVI smile,
//     per-expiry q_eff from a fixed borrow curve (both below).
//   * Spot tape: a fixed-seed walk of 1 bp steps where 1% of ticks jump 10 bp.
//     The seed and a hash of the chain spec land in the JSON context.
//
// ── Timing ──────────────────────────────────────────────────────────────────
//   One benchmark iteration == one tick. Each tick is timed on its own with
//   steady_clock (QueryPerformanceCounter on Windows) into a preallocated array;
//   UseManualTime feeds the per-tick time to Google Benchmark, so real_time is
//   the mean tick latency, and the p50/p90/p99/p99.9/max counters come from the
//   per-tick array. Before the timed ticks of every run the route is reset to its
//   refit-time state (untimed) and a block of discarded warm ticks runs. The
//   timed tick INCLUDES any in-tick rebuild a route performs; the rebuild count
//   is reported. Routes never touch the pricing pool (chain_tick.hpp contract).
//
// ── Accuracy (untimed) ──────────────────────────────────────────────────────
//   On 1% of the timed ticks the outputs of a rotating 5% contract subset are
//   copied out (after the tick's clock stops) and, after the run, compared with
//   the cold reference american_greeks_al(accurate AL, nullopt) — price, delta,
//   gamma, vega, theta from one call (its price is bit-identical to andersen_lake).
//   The reference values are computed once per process and reused.
//
// ── Routes (rows chain/refresh/<route>/n2000) ───────────────────────────────
//   cached_scalar      refresh_chain_cached (api/pricing/chain_tick.hpp): the
//                      per-contract cached-correction Greek bundle, inline.
//   cached_ladder      american_price_cached_ladder per (expiry, side) for the
//                      price + the cached Greek bundle for the Greeks.
//   cold_al_qlfast     american_greeks_al(al_bulk_opts) per contract, vega on,
//                      rho/charm off (3 boundary solves per contract).
//   cold_al_avx2_batch american_greeks_batch, analytic AVX2 laned route, accurate
//                      AL, executor = nullptr (serial, inline).
//   record_direct      refresh_chain_records: per-expiry ExpiryRecord (audit
//                      item 2), price + delta + gamma + vega + PDE theta.
//   record_price_only  ExpiryRecord::price per contract (the <= 150 ns cached-
//                      price micro target); Greek columns stay NaN.
//   record_ladder      ChainSpotLadder (audit items 1 + 4): per-contract
//                      Chebyshev price/vega series in spot built from the
//                      records; 10,000 timed ticks; in-tick window-exit
//                      rebuilds and record fallbacks are counted.
//   The cold routes cost ~0.1-1 s per tick, so they run a handful of ticks; the
//   cached routes a few hundred. Each row's `ticks` counter says how many.
//   Record routes report their untimed refit build cost (`build_ms`) and the
//   worst build-time certified error (`cert_err_fwd_max`, forward units).
//   chain/iv/record_{warm,cold}/n2000 time American IV against the records for
//   all 2,000 contracts (audit item 3); see run_iv below.
//   chain/serve/<route>/n2000 compare the serving routes at S0 (run_serve) and
//   chain/catchup/jump7/<order>_b<budget>/n{600,2000} time the spot ladder's
//   catch-up after a +7% jump (run_catchup).
//   NOT IMPLEMENTED: markov_batch (opt-in, never gated per §4).
//
// Every number this bench prints on a shared host is UNQUALIFIED. Only a quiet
// pinned run against bench/baselines/ is citable (bench_main.cpp protocol).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/fitting/correction.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_batch.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

// ── Chain spec ───────────────────────────────────────────────────────────────

constexpr double kSpot0 = 600.0;
constexpr double kRate = 0.043;
constexpr std::array<int, 20> kExpiryDays = {1,   2,   5,   7,   14,  21,  30,  45,  60,  90,
                                             120, 150, 180, 240, 300, 365, 450, 540, 640, 730};
constexpr std::size_t kStrikesPerExpiry = 50;
constexpr std::size_t kContracts = kExpiryDays.size() * kStrikesPerExpiry * 2u;
constexpr std::uint64_t kTapeSeed = 0x20261016A7C0FFEEull;
constexpr std::size_t kTapeLength = 20000; // covers warm + timed ticks of every route
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Fixed ATM term structure: elevated front end decaying to ~18%.
[[nodiscard]] double atm_vol(double T) noexcept { return 0.18 + 0.10 * std::exp(-T / 0.15); }

// Fixed effective-carry (borrow + dividend yield) curve.
[[nodiscard]] double carry_q(double T) noexcept { return 0.011 + 0.006 * std::exp(-T / 0.4); }

// eSSVI (Gatheral-Jacquier) with a power-law phi; eta*(1+|rho|) = 1.395 < 2 keeps
// the slices butterfly-free.
[[nodiscard]] double essvi_vol(double k, double T) noexcept {
  constexpr double kRho = -0.55;
  constexpr double kEta = 0.90;
  constexpr double kGamma = 0.45;
  const double a = atm_vol(T);
  const double theta = a * a * T;
  const double phi = kEta / (std::pow(theta, kGamma) * std::pow(1.0 + theta, 1.0 - kGamma));
  const double pk = phi * k;
  const double w =
      0.5 * theta * (1.0 + kRho * pk + std::sqrt((pk + kRho) * (pk + kRho) + 1.0 - kRho * kRho));
  return std::sqrt(w / T);
}

struct SyntheticChain {
  std::vector<double> strike, expiry, sigma, rate, carry;
  std::vector<Side> side;
  // Contiguous equal-(T, side) runs: [begin, begin + kStrikesPerExpiry).
  std::vector<std::size_t> run_begin;
  std::uint64_t spec_hash = 0;

  [[nodiscard]] ChainContractsView view() const noexcept {
    return ChainContractsView{strike, expiry, sigma, rate, carry, side};
  }
};

void fnv_mix(std::uint64_t &h, double v) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof bits);
  for (int b = 0; b < 8; ++b) {
    h ^= (bits >> (8 * b)) & 0xFFu;
    h *= 0x100000001B3ull;
  }
}

[[nodiscard]] SyntheticChain make_chain() {
  SyntheticChain c;
  c.spec_hash = 0xCBF29CE484222325ull;
  for (const int days : kExpiryDays) {
    const double T = static_cast<double>(days) / 365.0;
    const double q = carry_q(T);
    const double F = kSpot0 * std::exp((kRate - q) * T);
    const double half = 3.0 * atm_vol(T) * std::sqrt(T);
    for (const Side sd : {Side::Call, Side::Put}) {
      c.run_begin.push_back(c.strike.size());
      for (std::size_t j = 0; j < kStrikesPerExpiry; ++j) {
        const double k = -half + 2.0 * half * static_cast<double>(j) /
                                     static_cast<double>(kStrikesPerExpiry - 1u);
        c.strike.push_back(F * std::exp(k));
        c.expiry.push_back(T);
        c.sigma.push_back(essvi_vol(k, T));
        c.rate.push_back(kRate);
        c.carry.push_back(q);
        c.side.push_back(sd);
        fnv_mix(c.spec_hash, c.strike.back());
        fnv_mix(c.spec_hash, T);
        fnv_mix(c.spec_hash, c.sigma.back());
        fnv_mix(c.spec_hash, q);
        fnv_mix(c.spec_hash, sd == Side::Call ? 1.0 : -1.0);
      }
    }
  }
  return c;
}

[[nodiscard]] const SyntheticChain &chain() {
  static const SyntheticChain c = make_chain();
  return c;
}

// ── Spot tape ────────────────────────────────────────────────────────────────

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

[[nodiscard]] const std::vector<double> &tape() {
  static const std::vector<double> t = [] {
    std::vector<double> s(kTapeLength);
    std::uint64_t st = kTapeSeed;
    double spot = kSpot0;
    for (double &x : s) {
      const std::uint64_t u = splitmix64(st);
      const double step = ((u % 100u) == 0u) ? 10.0e-4 : 1.0e-4;
      spot *= ((u >> 40) & 1u) ? (1.0 + step) : (1.0 - step);
      x = spot;
    }
    return s;
  }();
  return t;
}

// ── Outputs ──────────────────────────────────────────────────────────────────

struct OutputStore {
  std::vector<double> price, delta, gamma, vega, theta;
  std::vector<TickStatus> status;
  // NaN-initialized so a column a route never writes reads as "no evidence".
  explicit OutputStore(std::size_t n)
      : price(n, kNaN), delta(n, kNaN), gamma(n, kNaN), vega(n, kNaN), theta(n, kNaN),
        status(n, TickStatus::Ok) {}
  [[nodiscard]] ChainTickOutputs view() noexcept {
    return ChainTickOutputs{price, delta, gamma, vega, theta, status};
  }
};

// ── Accuracy sampling against the cold reference ─────────────────────────────

constexpr std::size_t kSampleEveryTicks = 100; // 1% of ticks
constexpr std::size_t kSampleStride = 20;      // 5% of contracts per sampled tick

struct RefGreeks {
  double price = 0.0, delta = 0.0, gamma = 0.0, vega = 0.0, theta = 0.0;
  bool ok = false;
};

// Reference values for (tape index, contract), computed once per process.
[[nodiscard]] const RefGreeks &reference(std::size_t tape_idx, std::size_t contract) {
  static std::map<std::pair<std::size_t, std::size_t>, RefGreeks> cache;
  const auto key = std::make_pair(tape_idx, contract);
  if (const auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }
  const SyntheticChain &c = chain();
  RefGreeks ref;
  const Result<AmericanGreeks> g =
      american_greeks_al(tape()[tape_idx], c.strike[contract], c.expiry[contract],
                         c.sigma[contract], c.rate[contract], c.carry[contract],
                         c.side[contract], std::nullopt, /*need_vega=*/true,
                         /*need_rho=*/false, /*need_charm=*/false);
  if (g) {
    ref = RefGreeks{g->price, g->delta, g->gamma, g->vega, g->theta, true};
  }
  return cache.emplace(key, ref).first->second;
}

struct Sample {
  std::size_t tape_idx = 0;
  std::size_t contract = 0;
  double price = 0.0, delta = 0.0, gamma = 0.0, vega = 0.0, theta = 0.0;
  TickStatus status = TickStatus::Ok;
};

struct ErrStats {
  std::vector<double> abs_err;
  double max_abs = 0.0;
  void add(double e) {
    const double a = std::isfinite(e) ? std::abs(e) : 1.0e300;
    abs_err.push_back(a);
    max_abs = std::max(max_abs, a);
  }
  [[nodiscard]] double p99() const {
    if (abs_err.empty()) {
      return 0.0;
    }
    std::vector<double> s(abs_err);
    std::sort(s.begin(), s.end());
    return s[static_cast<std::size_t>(0.99 * static_cast<double>(s.size() - 1u))];
  }
};

void report_accuracy(benchmark::State &state, const std::vector<Sample> &samples) {
  const SyntheticChain &c = chain();
  ErrStats ep, ed, eg, ev, et;
  double max_bp_k = 0.0;
  std::size_t n_gt_mil = 0;
  std::array<std::size_t, 5> n_status{};
  for (const Sample &s : samples) {
    ++n_status[static_cast<std::size_t>(s.status)];
    if (s.status == TickStatus::OutOfWindow || s.status == TickStatus::Unpriceable) {
      continue;
    }
    const RefGreeks &r = reference(s.tape_idx, s.contract);
    if (!r.ok) {
      continue;
    }
    const double dp = s.price - r.price;
    ep.add(dp);
    max_bp_k = std::max(max_bp_k, 1.0e4 * std::abs(dp) / c.strike[s.contract]);
    if (!(std::abs(dp) <= 1.0e-3)) {
      ++n_gt_mil;
    }
    // A price-only route leaves its Greek columns NaN by contract; those rows
    // carry no Greek evidence rather than an infinite error.
    if (!std::isnan(s.delta)) {
      // Name the worst theta / gamma sample on stderr: the counters say how big,
      // this says where (expiry, side, strike, spot, status).
      if (std::abs(s.theta - r.theta) > et.max_abs || std::abs(s.gamma - r.gamma) > eg.max_abs) {
        std::fprintf(stderr,
                     "[chain-refresh] worst-so-far %s: i=%zu T=%.4f %s K=%.2f S=%.3f sig=%.4f "
                     "st=%d gamma %.6e/%.6e theta %.5f/%.5f\n",
                     state.name().c_str(), s.contract, c.expiry[s.contract],
                     c.side[s.contract] == Side::Put ? "put" : "call", c.strike[s.contract],
                     tape()[s.tape_idx], c.sigma[s.contract], static_cast<int>(s.status), s.gamma,
                     r.gamma, s.theta, r.theta);
      }
      ed.add(s.delta - r.delta);
      eg.add(s.gamma - r.gamma);
      ev.add(s.vega - r.vega);
      et.add(s.theta - r.theta);
    }
  }
  state.counters["acc_greek_samples"] = static_cast<double>(ed.abs_err.size());
  state.counters["acc_samples"] = static_cast<double>(samples.size());
  state.counters["price_max_abs"] = ep.max_abs;
  state.counters["price_p99_abs"] = ep.p99();
  state.counters["price_n_gt_1mil"] = static_cast<double>(n_gt_mil);
  state.counters["price_max_bp_K"] = max_bp_k;
  state.counters["delta_max_abs"] = ed.max_abs;
  state.counters["delta_p99_abs"] = ed.p99();
  state.counters["gamma_max_abs"] = eg.max_abs;
  state.counters["gamma_p99_abs"] = eg.p99();
  state.counters["vega_max_abs"] = ev.max_abs;
  state.counters["vega_p99_abs"] = ev.p99();
  state.counters["theta_max_abs"] = et.max_abs;
  state.counters["theta_p99_abs"] = et.p99();
  state.counters["st_exercised"] = static_cast<double>(n_status[1]);
  state.counters["st_out_of_window"] = static_cast<double>(n_status[2]);
  state.counters["st_fallback"] = static_cast<double>(n_status[3]);
  state.counters["st_unpriceable"] = static_cast<double>(n_status[4]);
}

// ── The per-route harness ────────────────────────────────────────────────────

[[nodiscard]] double quantile_us(const std::vector<double> &sorted_s, double p) {
  if (sorted_s.empty()) {
    return 0.0;
  }
  const double rank = p * static_cast<double>(sorted_s.size() - 1u);
  const auto lo = static_cast<std::size_t>(std::floor(rank));
  const auto hi = static_cast<std::size_t>(std::ceil(rank));
  return 1.0e6 *
         (sorted_s[lo] + (sorted_s[hi] - sorted_s[lo]) * (rank - static_cast<double>(lo)));
}

// A route: reset() restores refit-time state (untimed), refresh(spot, out) is
// the timed tick, rebuilds() counts in-tick rebuilds since reset().
template <class Route>
void run_route(benchmark::State &state, Route &route, std::size_t warm_ticks) {
  const std::vector<double> &spots = tape();
  OutputStore out(kContracts);
  const ChainTickOutputs ov = out.view();
  route.reset();
  for (std::size_t t = 0; t < warm_ticks; ++t) {
    route.refresh(spots[t % spots.size()], ov);
  }
  const std::uint64_t rebuilds_before = route.rebuilds();
  [[maybe_unused]] std::uint64_t fallbacks_before = 0;
  if constexpr (requires { route.fallbacks(); }) {
    fallbacks_before = route.fallbacks();
  }
  const auto planned = static_cast<std::size_t>(state.max_iterations);
  std::vector<double> lat;
  lat.reserve(planned);
  std::vector<Sample> samples;
  samples.reserve((planned / kSampleEveryTicks + 1u) * (kContracts / kSampleStride + 1u));
  std::size_t tick = 0;
  using Clock = std::chrono::steady_clock;
  for (auto _ : state) {
    const std::size_t idx = (warm_ticks + tick) % spots.size();
    const double spot = spots[idx];
    const Clock::time_point t0 = Clock::now();
    route.refresh(spot, ov);
    benchmark::DoNotOptimize(out.price.data());
    benchmark::DoNotOptimize(out.theta.data());
    benchmark::ClobberMemory();
    const Clock::time_point t1 = Clock::now();
    const double dt = std::chrono::duration<double>(t1 - t0).count();
    state.SetIterationTime(dt);
    if (lat.size() < lat.capacity()) {
      lat.push_back(dt);
    }
    if (tick % kSampleEveryTicks == 0u) {
      const std::size_t offset = (tick / kSampleEveryTicks) % kSampleStride;
      for (std::size_t i = offset; i < kContracts; i += kSampleStride) {
        samples.push_back(Sample{idx, i, out.price[i], out.delta[i], out.gamma[i], out.vega[i],
                                 out.theta[i], out.status[i]});
      }
    }
    ++tick;
  }
  std::vector<double> sorted(lat);
  std::sort(sorted.begin(), sorted.end());
  double mean = 0.0;
  for (const double x : lat) {
    mean += x;
  }
  mean = lat.empty() ? 0.0 : mean / static_cast<double>(lat.size());
  const double p50 = quantile_us(sorted, 0.50);
  state.counters["ticks"] = static_cast<double>(lat.size());
  state.counters["p50_us"] = p50;
  state.counters["p90_us"] = quantile_us(sorted, 0.90);
  state.counters["p99_us"] = quantile_us(sorted, 0.99);
  state.counters["p999_us"] = quantile_us(sorted, 0.999);
  state.counters["max_us"] = sorted.empty() ? 0.0 : 1.0e6 * sorted.back();
  state.counters["mean_us"] = 1.0e6 * mean;
  state.counters["ns_per_contract_p50"] = 1.0e3 * p50 / static_cast<double>(kContracts);
  const std::uint64_t rebuilds = route.rebuilds() - rebuilds_before;
  state.counters["rebuilds"] = static_cast<double>(rebuilds);
  state.counters["rebuilds_per_1k_ticks"] =
      lat.empty() ? 0.0
                  : 1.0e3 * static_cast<double>(rebuilds) / static_cast<double>(lat.size());
  state.counters["contracts"] = static_cast<double>(kContracts);
  // Refit-time (untimed) build cost of the route's records/ladders, when it has one.
  if constexpr (requires { route.build_ms(); }) {
    state.counters["build_ms"] = route.build_ms();
  }
  // Worst build-time certified error (forward units) of the route's records/ladders.
  if constexpr (requires { route.cert_fwd(); }) {
    state.counters["cert_err_fwd_max"] = route.cert_fwd();
  }
  // Spot-ladder diagnostics: record fallbacks during the timed ticks, refused
  // contracts, worst per-contract certificate ($) and the tick working set.
  if constexpr (requires { route.fallbacks(); }) {
    state.counters["fallbacks"] = static_cast<double>(route.fallbacks() - fallbacks_before);
    state.counters["refused"] = route.refused();
    state.counters["cert_usd_max"] = route.cert_usd_max();
    state.counters["tick_kb"] = route.tick_kb();
  }
  report_accuracy(state, samples);
}

// ── Route: cached_scalar (the library tick entry point) ──────────────────────

struct CorrectionCaches {
  std::optional<CorrectionCache> call, put;
  [[nodiscard]] AmericanCorrectionCaches view() const noexcept {
    return AmericanCorrectionCaches{call ? &*call : nullptr, put ? &*put : nullptr};
  }
};

// The production session cache recipe (session.cpp build_session_caches): one
// 16x8x12 cache per side over the chain's (k, T) box padded by 0.05 in k and
// [0.9 T_lo, 1.1 T_hi], sigma in [0.05, 1.5], baked at the mid expiry's carry,
// sampled with al_fast_opts.
[[nodiscard]] const CorrectionCaches &session_style_caches() {
  static const CorrectionCaches caches = [] {
    const SyntheticChain &c = chain();
    double k_min = 1.0e300;
    double k_max = -1.0e300;
    for (const double K : c.strike) {
      k_min = std::min(k_min, std::log(K / kSpot0));
      k_max = std::max(k_max, std::log(K / kSpot0));
    }
    const double T_lo = static_cast<double>(kExpiryDays.front()) / 365.0;
    const double T_hi = static_cast<double>(kExpiryDays.back()) / 365.0;
    const double q_rep =
        carry_q(static_cast<double>(kExpiryDays[kExpiryDays.size() / 2]) / 365.0);
    CorrectionCaches out;
    for (const Side sd : {Side::Call, Side::Put}) {
      Result<CorrectionCache> cc =
          CorrectionCache::build(16, 8, 12, kRate, q_rep, k_min - 0.05, k_max + 0.05, 0.9 * T_lo,
                                 1.1 * T_hi, 0.05, 1.5, sd, al_fast_opts());
      if (cc) {
        (sd == Side::Call ? out.call : out.put) = std::move(*cc);
      }
    }
    return out;
  }();
  return caches;
}

struct CachedScalarRoute {
  AmericanCorrectionCaches caches = session_style_caches().view();
  void reset() noexcept {}
  void refresh(double spot, const ChainTickOutputs &out) {
    const Status st = refresh_chain_cached(spot, chain().view(), caches, out);
    bool ok = st.has_value();
    benchmark::DoNotOptimize(ok);
  }
  [[nodiscard]] std::uint64_t rebuilds() const noexcept { return 0; }
};

// ── Route: cached_ladder (equal-T plane price + cached Greek bundle) ─────────

struct CachedLadderRoute {
  AmericanCorrectionCaches caches = session_style_caches().view();
  std::vector<double> ladder_px = std::vector<double>(kStrikesPerExpiry);
  void reset() noexcept {}
  void refresh(double spot, const ChainTickOutputs &out) {
    const SyntheticChain &c = chain();
    const Status st = refresh_chain_cached(spot, c.view(), caches, out);
    bool ok = st.has_value();
    benchmark::DoNotOptimize(ok);
    for (const std::size_t b : c.run_begin) {
      const Side sd = c.side[b];
      const std::span<const double> strikes(c.strike.data() + b, kStrikesPerExpiry);
      const std::span<const double> sigmas(c.sigma.data() + b, kStrikesPerExpiry);
      const Status ls = american_price_cached_ladder(
          spot, strikes, sigmas, c.expiry[b], c.rate[b], c.carry[b], sd,
          CorrectionBlend::single(caches.for_side(sd)), ladder_px);
      if (ls) {
        std::copy(ladder_px.begin(), ladder_px.end(), out.price.begin() + static_cast<long long>(b));
      }
    }
  }
  [[nodiscard]] std::uint64_t rebuilds() const noexcept { return 0; }
};

// ── Route: cold_al_qlfast (per-contract analytic AL Greeks, ql_fast rung) ─────

struct ColdAlQlFastRoute {
  void reset() noexcept {}
  void refresh(double spot, const ChainTickOutputs &out) {
    const SyntheticChain &c = chain();
    const AlOpts opts = al_bulk_opts();
    for (std::size_t i = 0; i < kContracts; ++i) {
      const Result<AmericanGreeks> g =
          american_greeks_al(spot, c.strike[i], c.expiry[i], c.sigma[i], c.rate[i], c.carry[i],
                             c.side[i], opts, /*need_vega=*/true, /*need_rho=*/false,
                             /*need_charm=*/false);
      if (g) {
        out.price[i] = g->price;
        out.delta[i] = g->delta;
        out.gamma[i] = g->gamma;
        out.vega[i] = g->vega;
        out.theta[i] = g->theta;
        out.status[i] = TickStatus::Ok;
      } else {
        out.status[i] = TickStatus::Unpriceable;
      }
    }
  }
  [[nodiscard]] std::uint64_t rebuilds() const noexcept { return 0; }
};

// ── Route: cold_al_avx2_batch (laned AVX2 analytic Greeks, serial) ───────────

struct ColdAlAvx2BatchRoute {
  std::vector<double> spot_col = std::vector<double>(kContracts);
  PricingWorkspace ws;
  void reset() noexcept {}
  void refresh(double spot, const ChainTickOutputs &out) {
    const SyntheticChain &c = chain();
    std::fill(spot_col.begin(), spot_col.end(), spot);
    const AmericanBatchInput in{spot_col, c.strike, c.expiry, c.sigma, c.rate, c.carry, c.side};
    simd::GreeksBatchSoA g{};
    g.price = out.price.data();
    g.delta = out.delta.data();
    g.gamma = out.gamma.data();
    g.vega = out.vega.data();
    g.theta = out.theta.data();
    PricingKernel kernel{simd::SimdIsa::Auto, /*analytic_greeks=*/true, /*executor=*/nullptr};
    const GreekFieldMask fields = GreekFieldMask::Price | GreekFieldMask::Delta |
                                  GreekFieldMask::Gamma | GreekFieldMask::Vega |
                                  GreekFieldMask::Theta;
    const Result<std::size_t> r = american_greeks_batch(in, fields, g, kernel, ws);
    bool ok = r.has_value();
    benchmark::DoNotOptimize(ok);
    const std::span<const LaneStatus> ls = ws.lane_status_view();
    for (std::size_t i = 0; i < kContracts && i < ls.size(); ++i) {
      out.status[i] = (ls[i] == LaneStatus::Ok) ? TickStatus::Ok : TickStatus::Unpriceable;
    }
  }
  [[nodiscard]] std::uint64_t rebuilds() const noexcept { return 0; }
};

// ── Records (audit item 2): one ExpiryRecord per (expiry, side) at S0 ─────────

struct BuiltRecords {
  std::optional<ChainRecords> recs;
  double build_ms = 0.0;
  double worst_cert_fwd = 0.0;
};

// Built once per process, untimed: the refit-time cost lands in `build_ms`.
[[nodiscard]] const BuiltRecords &chain_records() {
  static const BuiltRecords built = [] {
    BuiltRecords b;
    const auto t0 = std::chrono::steady_clock::now();
    Result<ChainRecords> r = ChainRecords::build(kSpot0, chain().view(), ChainRecordPolicy{});
    const auto t1 = std::chrono::steady_clock::now();
    b.build_ms = 1.0e3 * std::chrono::duration<double>(t1 - t0).count();
    if (r) {
      b.worst_cert_fwd = r->worst_certified_error_fwd();
      b.recs = std::move(*r);
    } else {
      std::fprintf(stderr, "[chain-refresh] ChainRecords::build failed: %s\n",
                   r.error().to_string().c_str());
    }
    return b;
  }();
  return built;
}

// record_direct: refresh_chain_records — every contract from its record (price,
// delta, gamma, vega, PDE theta): one second-order 2-D Clenshaw per contract.
struct RecordDirectRoute {
  const BuiltRecords &b = chain_records();
  void reset() noexcept {}
  void refresh(double spot, const ChainTickOutputs &out) {
    if (b.recs) {
      const Status st = refresh_chain_records(spot, chain().view(), *b.recs, out);
      bool ok = st.has_value();
      benchmark::DoNotOptimize(ok);
    }
  }
  [[nodiscard]] std::uint64_t rebuilds() const noexcept { return 0; }
  [[nodiscard]] double build_ms() const noexcept { return b.build_ms; }
  [[nodiscard]] double cert_fwd() const noexcept { return b.worst_cert_fwd; }
};

// record_price_only: the cached-PRICE micro path (audit target <= 150 ns per
// arbitrary-strike price): ExpiryRecord::price per contract, one 192-term
// Clenshaw plus the Black-76 leg. Greek columns are left NaN by design.
struct RecordPriceOnlyRoute {
  const BuiltRecords &b = chain_records();
  void reset() noexcept {}
  void refresh(double spot, const ChainTickOutputs &out) {
    if (!b.recs) {
      return;
    }
    const SyntheticChain &c = chain();
    const std::span<const std::uint32_t> rec_of = b.recs->record_of_contract();
    const ExpiryRecordBank &bank = b.recs->bank();
    for (std::size_t i = 0; i < kContracts; ++i) {
      out.price[i] = bank.at(rec_of[i])->price(spot, c.strike[i], c.sigma[i]);
    }
  }
  [[nodiscard]] std::uint64_t rebuilds() const noexcept { return 0; }
  [[nodiscard]] double build_ms() const noexcept { return b.build_ms; }
};

// record_ladder (audit items 1 + 4): ChainSpotLadder — per-contract Chebyshev
// price and vega series in spot, delta/gamma from the price series, PDE theta.
// reset() rebuilds every ladder at S0 from the records (untimed); window exits
// during the timed ticks rebuild in-tick and are counted.
struct RecordLadderRoute {
  const BuiltRecords &b = chain_records();
  ChainSpotLadder ladder;
  double first_build_ms = 0.0;
  std::uint64_t fallbacks_at_reset = 0;
  void reset() {
    if (!b.recs) {
      return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    Result<ChainSpotLadder> l = ChainSpotLadder::build(kSpot0, chain().view(), *b.recs);
    const auto t1 = std::chrono::steady_clock::now();
    if (first_build_ms == 0.0) {
      first_build_ms = 1.0e3 * std::chrono::duration<double>(t1 - t0).count();
    }
    if (l) {
      ladder = std::move(*l);
    }
  }
  void refresh(double spot, const ChainTickOutputs &out) {
    const Status st = ladder.refresh(spot, out);
    bool ok = st.has_value();
    benchmark::DoNotOptimize(ok);
  }
  [[nodiscard]] std::uint64_t rebuilds() const noexcept { return ladder.rebuilds(); }
  [[nodiscard]] std::uint64_t fallbacks() const noexcept { return ladder.fallbacks(); }
  // Ladder build from the records; the records' own build is in the record rows.
  [[nodiscard]] double build_ms() const noexcept { return first_build_ms; }
  [[nodiscard]] double cert_usd_max() const noexcept {
    double w = 0.0;
    for (const double e : ladder.certified_error_usd()) {
      w = std::max(w, e);
    }
    return w;
  }
  [[nodiscard]] double refused() const noexcept {
    return static_cast<double>(ladder.refused_contracts());
  }
  [[nodiscard]] double tick_kb() const noexcept {
    return static_cast<double>(ladder.tick_bytes()) / 1024.0;
  }
};

// ── American IV against the records (audit item 3) ───────────────────────────
//
// Rows chain/iv/record_{warm,cold}/n2000: one iteration inverts all 2,000
// contracts. Targets are COLD accurate AL prices at sigma_true = 1.03 x the
// surface vol (a 3% vol move), computed once. `warm` seeds each inversion at the
// surface vol (implied_vols_from_records); `cold` seeds at the record's sigma-box
// midpoint. Counters: ns_per_iv (p50 iteration / 2,000), evals_mean, and the IV
// error against sigma_true over the Ok contracts (vol units).

struct IvTargets {
  std::vector<double> target, sigma_true;
};

[[nodiscard]] const IvTargets &iv_targets() {
  static const IvTargets t = [] {
    const SyntheticChain &c = chain();
    IvTargets out;
    out.target.assign(kContracts, kNaN);
    out.sigma_true.assign(kContracts, kNaN);
    for (std::size_t i = 0; i < kContracts; ++i) {
      out.sigma_true[i] = 1.03 * c.sigma[i];
      const Result<double> p = andersen_lake(kSpot0, c.strike[i], c.expiry[i], out.sigma_true[i],
                                             c.rate[i], c.carry[i], c.side[i], std::nullopt);
      out.target[i] = p ? *p : kNaN;
    }
    return out;
  }();
  return t;
}

void run_iv(benchmark::State &state, bool warm) {
  const BuiltRecords &b = chain_records();
  if (!b.recs) {
    state.SkipWithError("ChainRecords::build failed");
    return;
  }
  const SyntheticChain &c = chain();
  const IvTargets &t = iv_targets();
  const std::span<const std::uint32_t> rec_of = b.recs->record_of_contract();
  const ExpiryRecordBank &bank = b.recs->bank();
  std::vector<double> sig(kContracts, kNaN);
  std::vector<RecordIvStatus> st(kContracts, RecordIvStatus::Invalid);
  std::vector<double> lat;
  lat.reserve(static_cast<std::size_t>(state.max_iterations));
  using Clock = std::chrono::steady_clock;
  for (auto _ : state) {
    const Clock::time_point t0 = Clock::now();
    if (warm) {
      const Status s = implied_vols_from_records(kSpot0, c.view(), *b.recs, t.target, sig, st);
      bool ok = s.has_value();
      benchmark::DoNotOptimize(ok);
    } else {
      for (std::size_t i = 0; i < kContracts; ++i) {
        const RecordIv iv =
            bank.at(rec_of[i])->implied_vol(kSpot0, c.strike[i], t.target[i], kNaN);
        sig[i] = iv.sigma;
        st[i] = iv.status;
      }
    }
    benchmark::DoNotOptimize(sig.data());
    benchmark::ClobberMemory();
    const Clock::time_point t1 = Clock::now();
    const double dt = std::chrono::duration<double>(t1 - t0).count();
    state.SetIterationTime(dt);
    if (lat.size() < lat.capacity()) {
      lat.push_back(dt);
    }
  }
  std::vector<double> sorted(lat);
  std::sort(sorted.begin(), sorted.end());
  state.counters["ns_per_iv"] = 1.0e3 * quantile_us(sorted, 0.5) / static_cast<double>(kContracts);
  state.counters["p99_us"] = quantile_us(sorted, 0.99);
  // Untimed: evaluation counts and accuracy against sigma_true.
  double evals = 0.0;
  ErrStats err;
  std::array<std::size_t, 7> n_status{};
  for (std::size_t i = 0; i < kContracts; ++i) {
    const RecordIv iv = bank.at(rec_of[i])->implied_vol(kSpot0, c.strike[i], t.target[i],
                                                        warm ? c.sigma[i] : kNaN);
    evals += iv.evaluations;
    ++n_status[static_cast<std::size_t>(iv.status)];
    if (iv.status == RecordIvStatus::Ok) {
      if (std::abs(iv.sigma - t.sigma_true[i]) > err.max_abs) {
        double vega = 0.0;
        const double p = bank.at(rec_of[i])->price_and_vega(kSpot0, c.strike[i], iv.sigma, &vega);
        std::fprintf(stderr,
                     "[chain-iv] worst-so-far %s: i=%zu T=%.4f %s K=%.2f iv=%.6f true=%.6f "
                     "target=%.6e price=%.6e vega=%.3e evals=%d\n",
                     state.name().c_str(), i, c.expiry[i], c.side[i] == Side::Put ? "put" : "call",
                     c.strike[i], iv.sigma, t.sigma_true[i], t.target[i], p, vega,
                     static_cast<int>(iv.evaluations));
      }
      err.add(iv.sigma - t.sigma_true[i]);
    }
  }
  state.counters["evals_mean"] = evals / static_cast<double>(kContracts);
  state.counters["iv_max_abs"] = err.max_abs;
  state.counters["iv_p99_abs"] = err.p99();
  state.counters["st_ok"] = static_cast<double>(n_status[0]);
  state.counters["st_vega_floor"] = static_cast<double>(n_status[4]);
  state.counters["st_box"] = static_cast<double>(n_status[2] + n_status[3]);
  state.counters["st_other"] =
      static_cast<double>(n_status[1] + n_status[5] + n_status[6]);
}

// ── Serve-path gate (serve-path lane, 2026-10-16, item 1) ────────────────────
//
// Rows chain/serve/<route>/n2000: one iteration re-marks all 2,000 contracts at
// S0 the way a session serve path would, and the row reports the FULL-chain
// price error against cold accurate Andersen-Lake at S0 (every contract, not the
// rotating 5% sample of the tick rows):
//   cached        the correction tensor everywhere (session default today)
//   gated_bX      the exercise-edge gate at band X sigma sqrt(T)
//                 (CorrectionBlend::clear_of_exercise_edge on the cache's own
//                 recorded edge, the SessionServePolicy predicate): a contract
//                 that fails it is re-priced cold, american_greeks_al with
//                 al_fast_opts and vega (3 boundary solves) — the Fast preset's
//                 scheme; the session's own cold Greek route
//                 (american_greeks_fd) costs more
//   records       refresh_chain_records: the records serve mode's route
// Counters: flips (contracts routed cold), price_max_abs / p99 / n_gt_1mil over
// all 2,000, served_cached_max_abs (the tensor-served subset), ms_per_chain p50.

[[nodiscard]] const std::vector<double> &cold_accurate_s0() {
  static const std::vector<double> ref = [] {
    const SyntheticChain &c = chain();
    std::vector<double> out(kContracts, kNaN);
    for (std::size_t i = 0; i < kContracts; ++i) {
      const Result<double> p = andersen_lake(kSpot0, c.strike[i], c.expiry[i], c.sigma[i],
                                             c.rate[i], c.carry[i], c.side[i], std::nullopt);
      out[i] = p ? *p : kNaN;
    }
    return out;
  }();
  return ref;
}

// band < 0: ungated tensor; band >= 0: gated. records: the record route.
void run_serve(benchmark::State &state, double band, bool records) {
  const SyntheticChain &c = chain();
  const std::vector<double> &ref = cold_accurate_s0();
  const AmericanCorrectionCaches caches = session_style_caches().view();
  const BuiltRecords &b = chain_records();
  if (records && !b.recs) {
    state.SkipWithError("ChainRecords::build failed");
    return;
  }
  OutputStore out(kContracts);
  const ChainTickOutputs ov = out.view();
  // Which contracts the gate routes cold (fixed at S0, computed once, untimed).
  std::vector<std::uint8_t> cold(kContracts, 0u);
  std::size_t flips = 0;
  if (!records && band >= 0.0) {
    for (std::size_t i = 0; i < kContracts; ++i) {
      const CorrectionBlend blend = CorrectionBlend::single(caches.for_side(c.side[i]));
      const double F = kSpot0 * std::exp((c.rate[i] - c.carry[i]) * c.expiry[i]);
      const double k = std::log(c.strike[i] / F);
      if (!blend.clear_of_exercise_edge(k, c.expiry[i], c.sigma[i], band)) {
        cold[i] = 1u;
        ++flips;
      }
    }
  }
  const AlOpts cold_opts = al_fast_opts();
  std::vector<double> lat;
  lat.reserve(static_cast<std::size_t>(state.max_iterations));
  using Clock = std::chrono::steady_clock;
  for (auto _ : state) {
    const Clock::time_point t0 = Clock::now();
    if (records) {
      const Status st = refresh_chain_records(kSpot0, c.view(), *b.recs, ov);
      bool ok = st.has_value();
      benchmark::DoNotOptimize(ok);
    } else {
      const Status st = refresh_chain_cached(kSpot0, c.view(), caches, ov);
      bool ok = st.has_value();
      benchmark::DoNotOptimize(ok);
      for (std::size_t i = 0; i < kContracts; ++i) {
        if (cold[i] == 0u) {
          continue;
        }
        const Result<AmericanGreeks> g =
            american_greeks_al(kSpot0, c.strike[i], c.expiry[i], c.sigma[i], c.rate[i],
                               c.carry[i], c.side[i], cold_opts, /*need_vega=*/true,
                               /*need_rho=*/false, /*need_charm=*/false);
        out.price[i] = g ? g->price : kNaN;
        out.delta[i] = g ? g->delta : kNaN;
        out.gamma[i] = g ? g->gamma : kNaN;
        out.vega[i] = g ? g->vega : kNaN;
        out.theta[i] = g ? g->theta : kNaN;
        out.status[i] = g ? TickStatus::Fallback : TickStatus::Unpriceable;
      }
    }
    benchmark::DoNotOptimize(out.price.data());
    benchmark::ClobberMemory();
    const double dt = std::chrono::duration<double>(Clock::now() - t0).count();
    state.SetIterationTime(dt);
    if (lat.size() < lat.capacity()) {
      lat.push_back(dt);
    }
  }
  std::vector<double> sorted(lat);
  std::sort(sorted.begin(), sorted.end());
  ErrStats all;
  double served_cached_max = 0.0;
  std::size_t n_gt_mil = 0;
  std::size_t worst = 0;
  for (std::size_t i = 0; i < kContracts; ++i) {
    const double e = std::abs(out.price[i] - ref[i]);
    if (e > all.max_abs || !std::isfinite(e)) {
      worst = i;
    }
    all.add(out.price[i] - ref[i]);
    n_gt_mil += (e <= 1.0e-3) ? 0u : 1u;
    if (!records && cold[i] == 0u) {
      served_cached_max = std::max(served_cached_max, std::isfinite(e) ? e : 1.0e300);
    }
  }
  std::fprintf(stderr, "[chain-serve] %s worst: i=%zu T=%.4f %s K=%.2f err=%.4e\n",
               state.name().c_str(), worst, c.expiry[worst],
               c.side[worst] == Side::Put ? "put" : "call", c.strike[worst],
               out.price[worst] - ref[worst]);
  state.counters["ms_per_chain_p50"] = 1.0e-3 * quantile_us(sorted, 0.5);
  state.counters["flips"] = static_cast<double>(flips);
  state.counters["price_max_abs"] = all.max_abs;
  state.counters["price_p99_abs"] = all.p99();
  state.counters["price_n_gt_1mil"] = static_cast<double>(n_gt_mil);
  state.counters["served_cached_max_abs"] = records ? 0.0 : served_cached_max;
  state.counters["contracts"] = static_cast<double>(kContracts);
}

// ── Catch-up after a +7% jump (serve-path lane, 2026-10-17, item 3) ──────────
//
// Rows chain/catchup/jump7/<order>_b<budget>/n{600,2000}: one iteration builds a
// ChainSpotLadder at S0 (untimed), then the spot jumps +7% in one tick and walks
// the 1 bp tape from there (spot_t = 1.07 x tape[t]). Every in-box contract has
// left its window, so the per-tick rebuild budget binds. The iteration ends on
// the first tick with no budget fallback (the "clean" tick), then times
// kCatchupSettleTicks more ticks. <order> is the SpotLadderPolicy rebuild order:
// `index` = ContractIndex (default), `atm` = NearestAtmFirst (opt-in). n600 is
// the strikes with j % 10 in {0, 3, 6} of every (expiry, side) run, with its own
// records. Manual time = the catch-up ticks, jump tick through the clean tick.
// Counters (identical every iteration: the scenario is deterministic):
//   catchup_ticks        ticks that carried a budget fallback
//   atm_catchup_tick     1-based tick from which every in-box contract with
//                        z = |ln(K / F)| / (sigma sqrt T) <= 1 is served Ok /
//                        Exercised (0 = never within kCatchupMaxTicks)
//   atm_lanes            in-box z <= 1 contracts on the jump tick
//   out_of_box_lanes     out-of-box record fallbacks on the clean tick: lanes the
//                        jump took outside their record box, which need a refit
//   jump_tick_us, catchup_tick_p50_us / _max_us, settled_tick_p50_us

constexpr double kCatchupJump = 1.07;
constexpr std::size_t kCatchupMaxTicks = 256;
constexpr std::size_t kCatchupSettleTicks = 50;

// Strikes j % 10 in {0, 3, 6} of every run: 15 of 50 -> 600 contracts.
[[nodiscard]] const SyntheticChain &chain600() {
  static const SyntheticChain s = [] {
    const SyntheticChain &full = chain();
    SyntheticChain out;
    for (std::size_t i = 0; i < full.strike.size(); ++i) {
      const std::size_t j = (i % kStrikesPerExpiry) % 10u;
      if (j != 0u && j != 3u && j != 6u) {
        continue;
      }
      out.strike.push_back(full.strike[i]);
      out.expiry.push_back(full.expiry[i]);
      out.sigma.push_back(full.sigma[i]);
      out.rate.push_back(full.rate[i]);
      out.carry.push_back(full.carry[i]);
      out.side.push_back(full.side[i]);
    }
    return out;
  }();
  return s;
}

[[nodiscard]] const BuiltRecords &chain600_records() {
  static const BuiltRecords built = [] {
    BuiltRecords b;
    const auto t0 = std::chrono::steady_clock::now();
    Result<ChainRecords> r = ChainRecords::build(kSpot0, chain600().view(), ChainRecordPolicy{});
    b.build_ms = 1.0e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (r) {
      b.worst_cert_fwd = r->worst_certified_error_fwd();
      b.recs = std::move(*r);
    } else {
      std::fprintf(stderr, "[chain-catchup] ChainRecords::build (n600) failed: %s\n",
                   r.error().to_string().c_str());
    }
    return b;
  }();
  return built;
}

// What one catch-up scenario observed (the timings go to the caller's arrays).
struct CatchupObs {
  std::size_t catchup_ticks = 0;
  std::size_t atm_catchup_tick = 0;
  std::size_t atm_lanes = 0;
  std::uint64_t out_of_box_lanes = 0;
  double total_s = 0.0;
  bool clean = false;
};

// In-box z <= 1 contracts still served Fallback at `spot`, after the tick.
[[nodiscard]] std::size_t atm_fallbacks(const SyntheticChain &c, const ChainRecords &recs,
                                        double spot, std::span<const TickStatus> status,
                                        std::size_t *atm_lanes) {
  const std::span<const std::uint32_t> rec_of = recs.record_of_contract();
  std::size_t waiting = 0;
  std::size_t lanes = 0;
  for (std::size_t i = 0; i < c.strike.size(); ++i) {
    const ExpiryRecord &rec = *recs.bank().at(rec_of[i]);
    const double k = std::log(c.strike[i] / (spot * rec.forward_factor()));
    if (!(std::abs(k) <= c.sigma[i] * std::sqrt(c.expiry[i])) || !rec.contains(k, c.sigma[i])) {
      continue;
    }
    ++lanes;
    waiting += (status[i] == TickStatus::Fallback) ? 1u : 0u;
  }
  if (atm_lanes != nullptr) {
    *atm_lanes = lanes;
  }
  return waiting;
}

void run_catchup(benchmark::State &state, bool n600, SpotLadderRebuildOrder order,
                 std::uint32_t budget) {
  const SyntheticChain &c = n600 ? chain600() : chain();
  const BuiltRecords &b = n600 ? chain600_records() : chain_records();
  if (!b.recs) {
    state.SkipWithError("ChainRecords::build failed");
    return;
  }
  const std::size_t n = c.strike.size();
  SpotLadderPolicy pol;
  pol.max_rebuilds_per_tick = budget;
  pol.rebuild_order = order;
  OutputStore out(n);
  const ChainTickOutputs ov = out.view();
  const std::vector<double> &spots = tape();
  std::vector<double> jump_lat, catchup_lat, settled_lat;
  const auto iters = static_cast<std::size_t>(state.max_iterations);
  jump_lat.reserve(iters);
  catchup_lat.reserve(iters * kCatchupMaxTicks);
  settled_lat.reserve(iters * kCatchupSettleTicks);
  CatchupObs last;
  using Clock = std::chrono::steady_clock;
  for (auto _ : state) {
    Result<ChainSpotLadder> built = ChainSpotLadder::build(kSpot0, c.view(), *b.recs, pol);
    if (!built) {
      state.SkipWithError("ChainSpotLadder::build failed");
      break;
    }
    ChainSpotLadder lad = std::move(*built);
    CatchupObs obs;
    std::size_t t = 0;
    for (; t < kCatchupMaxTicks && !obs.clean; ++t) {
      const double spot = kCatchupJump * spots[t];
      const std::uint64_t budget_before = lad.counters().budget_fallbacks;
      const std::uint64_t oob_before = lad.counters().out_of_box_fallbacks;
      const Clock::time_point t0 = Clock::now();
      const Status st = lad.refresh(spot, ov);
      benchmark::DoNotOptimize(out.price.data());
      benchmark::ClobberMemory();
      const double dt = std::chrono::duration<double>(Clock::now() - t0).count();
      bool ok = st.has_value();
      benchmark::DoNotOptimize(ok);
      obs.total_s += dt;
      if (t == 0u) {
        jump_lat.push_back(dt);
      }
      const bool pressure = lad.counters().budget_fallbacks != budget_before;
      if (pressure) {
        ++obs.catchup_ticks;
        catchup_lat.push_back(dt);
      } else {
        obs.clean = true;
        obs.out_of_box_lanes = lad.counters().out_of_box_fallbacks - oob_before;
      }
      if (obs.atm_catchup_tick == 0u &&
          atm_fallbacks(c, *b.recs, spot, out.status, t == 0u ? &obs.atm_lanes : nullptr) == 0u) {
        obs.atm_catchup_tick = t + 1u;
      }
    }
    for (std::size_t s = 0; s < kCatchupSettleTicks; ++s, ++t) {
      const Clock::time_point t0 = Clock::now();
      const Status st = lad.refresh(kCatchupJump * spots[t], ov);
      benchmark::DoNotOptimize(out.price.data());
      benchmark::ClobberMemory();
      settled_lat.push_back(std::chrono::duration<double>(Clock::now() - t0).count());
      bool ok = st.has_value();
      benchmark::DoNotOptimize(ok);
    }
    state.SetIterationTime(obs.total_s);
    last = obs;
  }
  std::sort(jump_lat.begin(), jump_lat.end());
  std::sort(catchup_lat.begin(), catchup_lat.end());
  std::sort(settled_lat.begin(), settled_lat.end());
  state.counters["catchup_ticks"] = static_cast<double>(last.catchup_ticks);
  state.counters["caught_up"] = last.clean ? 1.0 : 0.0;
  state.counters["atm_catchup_tick"] = static_cast<double>(last.atm_catchup_tick);
  state.counters["atm_lanes"] = static_cast<double>(last.atm_lanes);
  state.counters["out_of_box_lanes"] = static_cast<double>(last.out_of_box_lanes);
  state.counters["jump_tick_us"] = quantile_us(jump_lat, 0.5);
  state.counters["catchup_tick_p50_us"] = quantile_us(catchup_lat, 0.5);
  state.counters["catchup_tick_max_us"] = catchup_lat.empty() ? 0.0 : 1.0e6 * catchup_lat.back();
  state.counters["settled_tick_p50_us"] = quantile_us(settled_lat, 0.5);
  state.counters["budget"] = static_cast<double>(budget);
  state.counters["contracts"] = static_cast<double>(n);
}

// ── Registration ─────────────────────────────────────────────────────────────

void add_context_once() {
  static const bool done = [] {
    benchmark::AddCustomContext("chain_refresh_tick_seed", std::to_string(kTapeSeed));
    std::array<char, 32> hash{};
    std::snprintf(hash.data(), hash.size(), "%016llx",
                  static_cast<unsigned long long>(chain().spec_hash));
    benchmark::AddCustomContext("chain_refresh_spec_hash", hash.data());
    benchmark::AddCustomContext("chain_refresh_contracts", std::to_string(kContracts));
    benchmark::AddCustomContext("chain_refresh_spec",
                                "S=600 r=0.043 20 expiries x 50 strikes x 2 sides; eSSVI "
                                "rho=-0.55 eta=0.90 gamma=0.45; 1bp walk, 1% 10bp jumps");
    return true;
  }();
  (void)done;
}

template <class Route>
void register_route(const char *name, std::size_t timed_ticks, std::size_t warm_ticks) {
  benchmark::internal::Benchmark *b =
      benchmark::RegisterBenchmark(name, [warm_ticks](benchmark::State &state) {
        static Route route;
        run_route(state, route, warm_ticks);
      });
  apply_common(b)
      ->UseManualTime()
      ->Iterations(static_cast<benchmark::IterationCount>(timed_ticks))
      ->Unit(benchmark::kMicrosecond);
}

const bool kRegistered = [] {
  // Context must be added before RunSpecifiedBenchmarks prints it, i.e. here at
  // registration (static-init) time, not inside a running benchmark.
  add_context_once();
  register_route<CachedScalarRoute>("chain/refresh/cached_scalar/n2000", 300, 20);
  register_route<CachedLadderRoute>("chain/refresh/cached_ladder/n2000", 300, 20);
  register_route<ColdAlQlFastRoute>("chain/refresh/cold_al_qlfast/n2000", 5, 1);
  register_route<ColdAlAvx2BatchRoute>("chain/refresh/cold_al_avx2_batch/n2000", 3, 1);
  register_route<RecordDirectRoute>("chain/refresh/record_direct/n2000", 2000, 200);
  register_route<RecordPriceOnlyRoute>("chain/refresh/record_price_only/n2000", 2000, 200);
  // §4: 10,000 timed ticks after 1,000 discarded ones.
  register_route<RecordLadderRoute>("chain/refresh/record_ladder/n2000", 10000, 1000);
  for (const bool warm : {true, false}) {
    benchmark::internal::Benchmark *b = benchmark::RegisterBenchmark(
        warm ? "chain/iv/record_warm/n2000" : "chain/iv/record_cold/n2000",
        [warm](benchmark::State &state) { run_iv(state, warm); });
    apply_common(b)->UseManualTime()->Iterations(200)->Unit(benchmark::kMicrosecond);
  }
  // Serve-path gate (item 1): full-chain accuracy at S0 per serving route.
  struct ServeRow {
    const char *name;
    double band;
    bool records;
    int iterations;
  };
  constexpr std::array<ServeRow, 6> kServeRows = {{
      {"chain/serve/cached/n2000", -1.0, false, 20},
      {"chain/serve/gated_b0.5/n2000", 0.5, false, 5},
      {"chain/serve/gated_b1/n2000", 1.0, false, 5},
      {"chain/serve/gated_b2/n2000", 2.0, false, 5},
      {"chain/serve/gated_b3/n2000", 3.0, false, 5},
      {"chain/serve/records/n2000", -1.0, true, 50},
  }};
  for (const ServeRow &row : kServeRows) {
    benchmark::internal::Benchmark *b = benchmark::RegisterBenchmark(
        row.name, [row](benchmark::State &state) { run_serve(state, row.band, row.records); });
    apply_common(b)
        ->UseManualTime()
        ->Iterations(row.iterations)
        ->Unit(benchmark::kMillisecond);
  }
  // Catch-up after a +7% jump (item 3): rebuild order x budget x chain size.
  struct CatchupRow {
    const char *name;
    bool n600;
    SpotLadderRebuildOrder order;
    std::uint32_t budget;
  };
  constexpr SpotLadderRebuildOrder kIndex = SpotLadderRebuildOrder::ContractIndex;
  constexpr SpotLadderRebuildOrder kAtm = SpotLadderRebuildOrder::NearestAtmFirst;
  constexpr std::array<CatchupRow, 6> kCatchupRows = {{
      {"chain/catchup/jump7/index_b32/n600", true, kIndex, 32},
      {"chain/catchup/jump7/atm_b32/n600", true, kAtm, 32},
      {"chain/catchup/jump7/atm_b64/n600", true, kAtm, 64},
      {"chain/catchup/jump7/index_b32/n2000", false, kIndex, 32},
      {"chain/catchup/jump7/atm_b32/n2000", false, kAtm, 32},
      {"chain/catchup/jump7/atm_b64/n2000", false, kAtm, 64},
  }};
  for (const CatchupRow &row : kCatchupRows) {
    benchmark::internal::Benchmark *b =
        benchmark::RegisterBenchmark(row.name, [row](benchmark::State &state) {
          run_catchup(state, row.n600, row.order, row.budget);
        });
    apply_common(b)->UseManualTime()->Iterations(10)->Unit(benchmark::kMicrosecond);
  }
  return true;
}();

} // namespace
} // namespace atx::vol::bench

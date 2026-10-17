// Chain batch bench — batch-pricing lane 2026-10-17 (api/pricing/batch_chain.hpp).
//
// Question: how many contracts per second does the batch API price (price + delta +
// gamma + vega + theta + rho, accurate Andersen-Lake) and invert (American IV) on a
// 2,000-contract chain, single-thread and fanned out over the pinned pricing pool,
// against the naive per-contract loop it replaces, and are its outputs the loop's?
//
// ── Inputs (deterministic) ──────────────────────────────────────────────────
//   * The chain-refresh bench's synthetic chain, rebuilt from the same spec (S = 600,
//     r = 0.043, 20 expiries x 50 strikes x 2 sides, eSSVI smile, per-expiry carry);
//     its FNV spec hash is printed as chain_batch_spec_hash and must equal
//     chain_refresh_spec_hash (5414dd10c5a59ebd).
//   * IV targets: cold accurate andersen_lake at 1.03 x the surface vol, seeded at
//     the surface vol (the chain-refresh IV rows' recipe).
//   * Optional dev200 sample (rows chain/batch/dev200/...): a CSV named by
//     ATX_VOL_BATCH_DEV200_CSV with columns spot,expiry,years,rate,carry,div_pv,
//     strike,is_call,bid,ask (one underlier, one snapshot, every expiry). Its pricing
//     sigma is the PerContract implied vol of the quote mid (seed 0.3; 0.3 where the
//     mid has none). Rows skip with an error when the variable is unset.
//
// ── Rows ────────────────────────────────────────────────────────────────────
//   chain/batch/loop/{greeks,price}/n2000      the per-contract loop (baseline)
//   chain/batch/scalar/{greeks,price}/t<k>/n2000  Scalar route, k contexts (t1 inline)
//   chain/batch/laned_avx2/{greeks,price}/t<k>/n2000  opt-in LanedAvx2 route
//   chain/batch/iv/loop/n2000                  per-contract american_implied_vol loop
//   chain/batch/iv/{per_contract,lockstep}/t<k>/n2000
//   chain/batch/dev200/...                     the same on the dev200 sample
//   t<k> with k > 1 dispatches through pricing_executor() with n_threads = k: the
//   pinned main thread plus k - 1 pool workers from bench_main's pool CPU set
//   (atx_pool_cpu_mask; which P-core each worker lands on is the scheduler's).
//
// ── Counters ────────────────────────────────────────────────────────────────
//   ms_per_chain (p50 of the timed iterations), contracts_per_s, ns_per_contract;
//   fp_hi / fp_lo: the two 32-bit halves of an FNV-1a hash over every output bit and
//   status byte (equal hashes == bit-identical outputs); identical_to_loop (Scalar /
//   PerContract: 1 iff bit-identical to the per-contract loop); identical_to_t1
//   (every route: 1 iff bit-identical to the same route inline); max_abs_<col>
//   against the loop (LanedAvx2 / Lockstep); st_ok and the non-Ok status counts.
//
// Every number this bench prints on a shared host is UNQUALIFIED.

#include <algorithm>
#include <array>
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
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/batch_chain.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ── Chain (the chain-refresh spec) ───────────────────────────────────────────

constexpr double kSpot0 = 600.0;
constexpr double kRate = 0.043;
constexpr std::array<int, 20> kExpiryDays = {1,   2,   5,   7,   14,  21,  30,  45,  60,  90,
                                             120, 150, 180, 240, 300, 365, 450, 540, 640, 730};
constexpr std::size_t kStrikesPerExpiry = 50;

[[nodiscard]] double atm_vol(double T) noexcept { return 0.18 + 0.10 * std::exp(-T / 0.15); }
[[nodiscard]] double carry_q(double T) noexcept { return 0.011 + 0.006 * std::exp(-T / 0.4); }
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

void fnv_mix(std::uint64_t &h, double v) noexcept {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof bits);
  for (int b = 0; b < 8; ++b) {
    h ^= (bits >> (8 * b)) & 0xFFu;
    h *= 0x100000001B3ull;
  }
}

struct BatchChain {
  double spot = 0.0;
  std::vector<double> T, rate, carry, div_pv;
  std::vector<double> strike, sigma, target, sigma_true;
  std::vector<std::uint32_t> expiry;
  std::vector<Side> side;
  std::uint64_t spec_hash = 0;
  std::string error; // non-empty: the chain could not be built

  [[nodiscard]] ChainExpiryColumns exp() const noexcept { return {T, rate, carry, div_pv}; }
  [[nodiscard]] ChainContractColumns con() const noexcept { return {strike, expiry, side, sigma}; }
  [[nodiscard]] std::size_t size() const noexcept { return strike.size(); }
};

[[nodiscard]] double q_eff_of(const BatchChain &c, std::uint32_t e) noexcept {
  return chain_effective_carry(c.spot, c.T[e], c.carry[e], c.div_pv.empty() ? 0.0 : c.div_pv[e]);
}

// IV targets: cold accurate AL at 1.03 x sigma.
void fill_iv_targets(BatchChain &c) {
  c.target.assign(c.size(), kNaN);
  c.sigma_true.assign(c.size(), kNaN);
  for (std::size_t i = 0; i < c.size(); ++i) {
    const std::uint32_t e = c.expiry[i];
    c.sigma_true[i] = 1.03 * c.sigma[i];
    const Result<double> p = andersen_lake(c.spot, c.strike[i], c.T[e], c.sigma_true[i],
                                           c.rate[e], q_eff_of(c, e), c.side[i], std::nullopt);
    c.target[i] = p ? *p : kNaN;
  }
}

// `with_targets` = false builds the spec only (no pricing call), which is what the
// static-init context registration needs: no kernel or pool use before main().
[[nodiscard]] BatchChain make_synthetic(bool with_targets) {
  BatchChain c;
  c.spot = kSpot0;
  c.spec_hash = 0xCBF29CE484222325ull;
  for (std::size_t e = 0; e < kExpiryDays.size(); ++e) {
    const double T = static_cast<double>(kExpiryDays[e]) / 365.0;
    const double q = carry_q(T);
    c.T.push_back(T);
    c.rate.push_back(kRate);
    c.carry.push_back(q);
    const double F = kSpot0 * std::exp((kRate - q) * T);
    const double half = 3.0 * atm_vol(T) * std::sqrt(T);
    for (const Side sd : {Side::Call, Side::Put}) {
      for (std::size_t j = 0; j < kStrikesPerExpiry; ++j) {
        const double k = -half + 2.0 * half * static_cast<double>(j) /
                                     static_cast<double>(kStrikesPerExpiry - 1u);
        c.strike.push_back(F * std::exp(k));
        c.sigma.push_back(essvi_vol(k, T));
        c.expiry.push_back(static_cast<std::uint32_t>(e));
        c.side.push_back(sd);
        fnv_mix(c.spec_hash, c.strike.back());
        fnv_mix(c.spec_hash, T);
        fnv_mix(c.spec_hash, c.sigma.back());
        fnv_mix(c.spec_hash, q);
        fnv_mix(c.spec_hash, sd == Side::Call ? 1.0 : -1.0);
      }
    }
  }
  if (with_targets) {
    fill_iv_targets(c);
  }
  return c;
}

[[nodiscard]] const BatchChain &synthetic() {
  static const BatchChain c = make_synthetic(true);
  return c;
}

// ── dev200 sample ────────────────────────────────────────────────────────────

[[nodiscard]] std::string env_or_empty(const char *name) {
  char *raw = nullptr;
  std::size_t len = 0;
  std::string out;
  if (_dupenv_s(&raw, &len, name) == 0 && raw != nullptr) {
    out = raw;
  }
  std::free(raw);
  return out;
}

[[nodiscard]] BatchChain load_dev200() {
  BatchChain c;
  const std::string path = env_or_empty("ATX_VOL_BATCH_DEV200_CSV");
  if (path.empty()) {
    c.error = "ATX_VOL_BATCH_DEV200_CSV is not set";
    return c;
  }
  std::ifstream in(path);
  std::string line;
  if (!in || !std::getline(in, line)) {
    c.error = "cannot read " + path;
    return c;
  }
  std::vector<double> mids;
  while (std::getline(in, line)) {
    std::array<double, 10> v{};
    std::stringstream ss(line);
    std::string cell;
    for (double &x : v) {
      if (!std::getline(ss, cell, ',')) {
        c.error = "short row in " + path;
        return c;
      }
      x = std::strtod(cell.c_str(), nullptr);
    }
    const auto e = static_cast<std::uint32_t>(v[1]);
    c.spot = v[0];
    if (e >= c.T.size()) {
      c.T.resize(e + 1u, kNaN);
      c.rate.resize(e + 1u, kNaN);
      c.carry.resize(e + 1u, kNaN);
      c.div_pv.resize(e + 1u, kNaN);
    }
    c.T[e] = v[2];
    c.rate[e] = v[3];
    c.carry[e] = v[4];
    c.div_pv[e] = v[5];
    c.strike.push_back(v[6]);
    c.side.push_back(v[7] != 0.0 ? Side::Call : Side::Put);
    c.expiry.push_back(e);
    mids.push_back(v[8] > 0.0 && v[9] > v[8] ? 0.5 * (v[8] + v[9]) : kNaN);
    fnv_mix(c.spec_hash, v[6]);
  }
  // Pricing sigma: the PerContract IV of the mid (seed 0.3), 0.3 where none.
  c.sigma.assign(c.size(), 0.3);
  std::vector<double> iv(c.size(), kNaN);
  std::vector<ChainRowStatus> st(c.size(), ChainRowStatus::InvalidInput);
  ChainBatchWorkspace ws;
  const Result<std::size_t> r =
      implied_vol_chain_batch(c.spot, c.exp(), c.con(), mids, iv, st, ChainIvOptions{}, ws);
  if (!r) {
    c.error = "mid IV failed: " + r.error().to_string();
    return c;
  }
  for (std::size_t i = 0; i < c.size(); ++i) {
    if (st[i] == ChainRowStatus::Ok) {
      c.sigma[i] = iv[i];
    }
  }
  fill_iv_targets(c);
  return c;
}

[[nodiscard]] const BatchChain &dev200() {
  static const BatchChain c = load_dev200();
  return c;
}

// ── Outputs, hashing, reference loops ────────────────────────────────────────

struct Outs {
  std::vector<double> price, delta, gamma, vega, theta, rho, iv;
  std::vector<ChainRowStatus> status;
  explicit Outs(std::size_t n = 0)
      : price(n, kNaN), delta(n, kNaN), gamma(n, kNaN), vega(n, kNaN), theta(n, kNaN),
        rho(n, kNaN), iv(n, kNaN), status(n, ChainRowStatus::InvalidInput) {}
  [[nodiscard]] ChainGreekColumns greeks() {
    return {price, delta, gamma, vega, theta, rho, status};
  }
  [[nodiscard]] ChainGreekColumns prices() { return {price, {}, {}, {}, {}, {}, status}; }

  [[nodiscard]] std::uint64_t fp() const noexcept {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::vector<double> *col : {&price, &delta, &gamma, &vega, &theta, &rho, &iv}) {
      for (const double v : *col) {
        fnv_mix(h, v);
      }
    }
    for (const ChainRowStatus s : status) {
      fnv_mix(h, static_cast<double>(s));
    }
    return h;
  }
};

enum class Kind : std::uint8_t { Greeks, Price, Iv };

void fail_row(Outs &o, std::size_t i, ChainRowStatus st) {
  o.price[i] = o.delta[i] = o.gamma[i] = o.vega[i] = o.theta[i] = o.rho[i] = kNaN;
  o.status[i] = st;
}

[[nodiscard]] bool contract_valid(const BatchChain &c, std::size_t i, double *q) {
  const std::uint32_t e = c.expiry[i];
  *q = q_eff_of(c, e);
  return std::isfinite(*q) && std::isfinite(c.rate[e]) && c.strike[i] > 0.0 &&
         std::isfinite(c.strike[i]) && c.sigma[i] > 0.0 && std::isfinite(c.sigma[i]);
}

// The naive per-contract loop: one kernel call per contract at the effective carry.
void loop_greeks(const BatchChain &c, Outs &o, bool price_only) {
  for (std::size_t i = 0; i < c.size(); ++i) {
    double q = 0.0;
    if (!contract_valid(c, i, &q)) {
      fail_row(o, i, ChainRowStatus::InvalidInput);
      continue;
    }
    const std::uint32_t e = c.expiry[i];
    if (price_only) {
      const Result<double> p = andersen_lake(c.spot, c.strike[i], c.T[e], c.sigma[i], c.rate[e],
                                             q, c.side[i], std::nullopt);
      if (p && std::isfinite(*p)) {
        o.price[i] = *p;
        o.status[i] = ChainRowStatus::Ok;
      } else {
        fail_row(o, i, ChainRowStatus::Unpriceable);
      }
      continue;
    }
    const Result<AmericanGreeks> g =
        american_greeks_al(c.spot, c.strike[i], c.T[e], c.sigma[i], c.rate[e], q, c.side[i],
                           std::nullopt, /*need_vega=*/true, /*need_rho=*/true, false);
    if (!g || !std::isfinite(g->price) || !std::isfinite(g->delta) || !std::isfinite(g->gamma) ||
        !std::isfinite(g->vega) || !std::isfinite(g->theta) || !std::isfinite(g->rho)) {
      fail_row(o, i, ChainRowStatus::Unpriceable);
      continue;
    }
    o.price[i] = g->price;
    o.delta[i] = g->delta;
    o.gamma[i] = g->gamma;
    o.vega[i] = g->vega;
    o.theta[i] = g->theta;
    o.rho[i] = g->rho;
    o.status[i] = ChainRowStatus::Ok;
  }
}

// The per-contract IV loop: the batch screen, then american_implied_vol warm-seeded
// at the contract's sigma.
void loop_iv(const BatchChain &c, Outs &o) {
  const ChainIvOptions opt{};
  for (std::size_t i = 0; i < c.size(); ++i) {
    double q = 0.0;
    o.iv[i] = kNaN;
    const double P = c.target[i];
    if (!contract_valid(c, i, &q) || !std::isfinite(P)) {
      o.status[i] = ChainRowStatus::InvalidInput;
      continue;
    }
    const double K = c.strike[i];
    const bool call = c.side[i] == Side::Call;
    const double intr = call ? std::max(c.spot - K, 0.0) : std::max(K - c.spot, 0.0);
    const double band = opt.intrinsic_tol * K;
    if (P < intr - band || P >= (call ? c.spot : K)) {
      o.status[i] = ChainRowStatus::OutOfBand;
      continue;
    }
    if (P <= intr + band) {
      o.status[i] = ChainRowStatus::VegaFloor;
      continue;
    }
    const std::uint32_t e = c.expiry[i];
    const Result<double> iv = american_implied_vol(P, c.spot, K, c.T[e], c.rate[e], q, c.side[i],
                                                   AmericanMethod::AndersenLake, opt.sigma_tol,
                                                   opt.max_iter, opt.al_opts, nullptr,
                                                   c.sigma[i], 0.0);
    if (iv) {
      if (*iv > kIvMin) {
        o.iv[i] = *iv;
        o.status[i] = ChainRowStatus::Ok;
      } else {
        o.status[i] = ChainRowStatus::VegaFloor;
      }
    } else if (iv.error().code() == ErrorCode::OutOfRange) {
      o.status[i] = ChainRowStatus::OutOfBand;
    } else if (iv.error().code() == ErrorCode::Unavailable) {
      o.status[i] = ChainRowStatus::NoConvergence;
    } else {
      o.status[i] = ChainRowStatus::Unpriceable;
    }
  }
}

// One evaluation of `kind` through the loop (route < 0) or the batch.
struct Eval {
  Kind kind = Kind::Greeks;
  bool loop = false;
  ChainBatchRoute route = ChainBatchRoute::Scalar;
  ChainIvRoute iv_route = ChainIvRoute::PerContract;
  unsigned threads = 1;
};

void evaluate(const BatchChain &c, const Eval &ev, Outs &o, ChainBatchWorkspace &ws) {
  if (ev.loop) {
    if (ev.kind == Kind::Iv) {
      loop_iv(c, o);
    } else {
      loop_greeks(c, o, ev.kind == Kind::Price);
    }
    return;
  }
  PricingExecutor *ex = ev.threads > 1u ? &pricing_executor() : nullptr;
  if (ev.kind == Kind::Iv) {
    ChainIvOptions opt;
    opt.route = ev.iv_route;
    opt.executor = ex;
    opt.n_threads = ev.threads;
    const Result<std::size_t> r =
        implied_vol_chain_batch(c.spot, c.exp(), c.con(), c.target, o.iv, o.status, opt, ws);
    bool ok = r.has_value();
    benchmark::DoNotOptimize(ok);
    return;
  }
  ChainBatchOptions opt;
  opt.route = ev.route;
  opt.executor = ex;
  opt.n_threads = ev.threads;
  const Result<std::size_t> r = price_chain_batch(
      c.spot, c.exp(), c.con(), ev.kind == Kind::Price ? o.prices() : o.greeks(), opt, ws);
  bool ok = r.has_value();
  benchmark::DoNotOptimize(ok);
}

// Reference outputs (untimed, computed once per chain and kind).
[[nodiscard]] const Outs &reference(const BatchChain &c, Kind kind) {
  static Outs syn[3];
  static Outs dev[3];
  static bool done_syn[3] = {false, false, false};
  static bool done_dev[3] = {false, false, false};
  const auto k = static_cast<std::size_t>(kind);
  const bool is_syn = &c == &synthetic();
  Outs &slot = is_syn ? syn[k] : dev[k];
  bool &done = is_syn ? done_syn[k] : done_dev[k];
  if (!done) {
    slot = Outs(c.size());
    ChainBatchWorkspace ws;
    evaluate(c, Eval{kind, true}, slot, ws);
    done = true;
  }
  return slot;
}

[[nodiscard]] double quantile(std::vector<double> v, double p) {
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  const double rank = p * static_cast<double>(v.size() - 1u);
  const auto lo = static_cast<std::size_t>(std::floor(rank));
  const auto hi = static_cast<std::size_t>(std::ceil(rank));
  return v[lo] + (v[hi] - v[lo]) * (rank - static_cast<double>(lo));
}

[[nodiscard]] bool same_bits(const std::vector<double> &a, const std::vector<double> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

[[nodiscard]] double max_abs_ok(const std::vector<double> &a, const std::vector<double> &b,
                                const std::vector<ChainRowStatus> &sa,
                                const std::vector<ChainRowStatus> &sb) {
  double m = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (sa[i] == ChainRowStatus::Ok && sb[i] == ChainRowStatus::Ok) {
      m = std::max(m, std::abs(a[i] - b[i]));
    }
  }
  return m;
}

void run_row(benchmark::State &state, const BatchChain &c, Eval ev) {
  if (!c.error.empty()) {
    state.SkipWithError(c.error.c_str());
    return;
  }
  const std::size_t n = c.size();
  Outs o(n);
  ChainBatchWorkspace ws;
  ws.reserve(c.T.size());
  std::vector<double> lat;
  lat.reserve(static_cast<std::size_t>(state.max_iterations));
  using Clock = std::chrono::steady_clock;
  for (auto _ : state) {
    const Clock::time_point t0 = Clock::now();
    evaluate(c, ev, o, ws);
    benchmark::DoNotOptimize(o.price.data());
    benchmark::DoNotOptimize(o.iv.data());
    benchmark::ClobberMemory();
    const double dt = std::chrono::duration<double>(Clock::now() - t0).count();
    state.SetIterationTime(dt);
    if (lat.size() < lat.capacity()) {
      lat.push_back(dt);
    }
  }
  // Untimed: identity and accuracy evidence.
  const double p50 = quantile(lat, 0.5);
  state.counters["ms_per_chain"] = 1.0e3 * p50;
  state.counters["contracts_per_s"] = p50 > 0.0 ? static_cast<double>(n) / p50 : 0.0;
  state.counters["ns_per_contract"] = 1.0e9 * p50 / static_cast<double>(n);
  state.counters["contracts"] = static_cast<double>(n);
  state.counters["threads"] = static_cast<double>(ev.threads);
  state.counters["pool_size"] = static_cast<double>(pricing_executor().size());
  const std::uint64_t fp = o.fp();
  state.counters["fp_hi"] = static_cast<double>(fp >> 32);
  state.counters["fp_lo"] = static_cast<double>(fp & 0xFFFFFFFFull);
  std::array<std::size_t, 6> n_st{};
  for (const ChainRowStatus s : o.status) {
    ++n_st[static_cast<std::size_t>(s)];
  }
  state.counters["st_ok"] = static_cast<double>(n_st[0]);
  state.counters["st_invalid"] = static_cast<double>(n_st[1]);
  state.counters["st_unpriceable"] = static_cast<double>(n_st[2]);
  state.counters["st_vega_floor"] = static_cast<double>(n_st[3]);
  state.counters["st_out_of_band"] = static_cast<double>(n_st[4]);
  state.counters["st_no_convergence"] = static_cast<double>(n_st[5]);
  if (ev.loop) {
    return;
  }
  const Outs &ref = reference(c, ev.kind);
  const bool identical = same_bits(o.price, ref.price) && same_bits(o.delta, ref.delta) &&
                         same_bits(o.gamma, ref.gamma) && same_bits(o.vega, ref.vega) &&
                         same_bits(o.theta, ref.theta) && same_bits(o.rho, ref.rho) &&
                         same_bits(o.iv, ref.iv) && o.status == ref.status;
  state.counters["identical_to_loop"] = identical ? 1.0 : 0.0;
  // The same route inline: thread-count invariance.
  Eval inline_ev = ev;
  inline_ev.threads = 1;
  Outs t1(n);
  evaluate(c, inline_ev, t1, ws);
  state.counters["identical_to_t1"] = (t1.fp() == fp && same_bits(t1.price, o.price) &&
                                       same_bits(t1.iv, o.iv) && t1.status == o.status)
                                          ? 1.0
                                          : 0.0;
  if (ev.kind == Kind::Iv) {
    state.counters["max_abs_iv"] = max_abs_ok(o.iv, ref.iv, o.status, ref.status);
    double vs_true = 0.0;
    std::size_t worst = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (o.status[i] == ChainRowStatus::Ok && std::abs(o.iv[i] - c.sigma_true[i]) > vs_true) {
        vs_true = std::abs(o.iv[i] - c.sigma_true[i]);
        worst = i;
      }
    }
    state.counters["max_abs_iv_vs_true"] = vs_true;
    std::size_t status_moves = 0;
    for (std::size_t i = 0; i < n; ++i) {
      status_moves += o.status[i] != ref.status[i] ? 1u : 0u;
      if (o.status[i] != ref.status[i]) {
        std::fprintf(stderr, "[chain-batch] %s status moved: i=%zu st=%d loop=%d\n",
                     state.name().c_str(), i, static_cast<int>(o.status[i]),
                     static_cast<int>(ref.status[i]));
      }
    }
    // Name the worst row on stderr: the counter says how big, this says where.
    const std::uint32_t we = c.expiry[worst];
    std::fprintf(stderr,
                 "[chain-batch] %s worst |iv - true|: i=%zu T=%.5f %s K=%.2f S=%.3f target=%.6e "
                 "iv=%.6f true=%.6f loop=%.6f loop_st=%d\n",
                 state.name().c_str(), worst, c.T[we], c.side[worst] == Side::Put ? "put" : "call",
                 c.strike[worst], c.spot, c.target[worst], o.iv[worst], c.sigma_true[worst],
                 ref.iv[worst], static_cast<int>(ref.status[worst]));
    state.counters["status_diff_vs_loop"] = static_cast<double>(status_moves);
    return;
  }
  state.counters["max_abs_price"] = max_abs_ok(o.price, ref.price, o.status, ref.status);
  if (ev.kind == Kind::Greeks) {
    state.counters["max_abs_delta"] = max_abs_ok(o.delta, ref.delta, o.status, ref.status);
    state.counters["max_abs_gamma"] = max_abs_ok(o.gamma, ref.gamma, o.status, ref.status);
    state.counters["max_abs_vega"] = max_abs_ok(o.vega, ref.vega, o.status, ref.status);
    state.counters["max_abs_theta"] = max_abs_ok(o.theta, ref.theta, o.status, ref.status);
    state.counters["max_abs_rho"] = max_abs_ok(o.rho, ref.rho, o.status, ref.status);
  }
}

// ── Registration ─────────────────────────────────────────────────────────────

void add_context_once() {
  static const bool done = [] {
    const BatchChain spec = make_synthetic(/*with_targets=*/false);
    std::array<char, 32> hash{};
    std::snprintf(hash.data(), hash.size(), "%016llx",
                  static_cast<unsigned long long>(spec.spec_hash));
    benchmark::AddCustomContext("chain_batch_spec_hash", hash.data());
    benchmark::AddCustomContext("chain_batch_contracts", std::to_string(spec.size()));
    benchmark::AddCustomContext(
        "chain_batch_spec",
        "chain-refresh spec: S=600 r=0.043 20 expiries x 50 strikes x 2 sides; eSSVI; accurate "
        "AL; greeks = price+delta+gamma+vega+theta+rho; IV targets AL at 1.03 sigma");
    return true;
  }();
  (void)done;
}

void reg(const std::string &name, bool dev, Eval ev, int iterations) {
  benchmark::internal::Benchmark *b =
      benchmark::RegisterBenchmark(name.c_str(), [dev, ev](benchmark::State &state) {
        run_row(state, dev ? dev200() : synthetic(), ev);
      });
  apply_common(b)
      ->UseManualTime()
      ->Iterations(iterations)
      ->Unit(benchmark::kMillisecond);
}

const bool kRegistered = [] {
  add_context_once();
  for (const bool dev : {false, true}) {
    const std::string pre = dev ? "chain/batch/dev200/" : "chain/batch/";
    const std::string suf = dev ? "" : "/n2000";
    for (const Kind kind : {Kind::Greeks, Kind::Price}) {
      const std::string k = kind == Kind::Greeks ? "greeks" : "price";
      reg(pre + "loop/" + k + suf, dev, Eval{kind, true}, 1);
      for (const unsigned t : {1u, 2u, 3u, 4u}) {
        reg(pre + "scalar/" + k + "/t" + std::to_string(t) + suf, dev,
            Eval{kind, false, ChainBatchRoute::Scalar, ChainIvRoute::PerContract, t}, 1);
      }
      for (const unsigned t : {1u, 3u}) {
        reg(pre + "laned_avx2/" + k + "/t" + std::to_string(t) + suf, dev,
            Eval{kind, false, ChainBatchRoute::LanedAvx2, ChainIvRoute::PerContract, t}, 1);
      }
    }
    reg(pre + "iv/loop" + suf, dev, Eval{Kind::Iv, true}, 1);
    for (const ChainIvRoute r : {ChainIvRoute::PerContract, ChainIvRoute::Lockstep}) {
      const std::string rn = r == ChainIvRoute::PerContract ? "per_contract" : "lockstep";
      for (const unsigned t : {1u, 3u}) {
        reg(pre + "iv/" + rn + "/t" + std::to_string(t) + suf, dev,
            Eval{Kind::Iv, false, ChainBatchRoute::Scalar, r, t}, 1);
      }
    }
  }
  return true;
}();

} // namespace
} // namespace atx::vol::bench

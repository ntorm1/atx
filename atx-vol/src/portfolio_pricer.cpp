// PortfolioPricer implementation — dedup, parallel Greeks fan-out, and the
// Taylor PnL-explain decomposition. See portfolio_pricer.hpp for the model.

#include "atx/vol/portfolio_pricer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <unordered_map>

#include "atx/vol/american.hpp" // AmericanGreeks
#include "atx/vol/counters.hpp" // ATX_VOL_COUNT (opt-in P0.2; no-op when OFF)

namespace atx::vol {

namespace {

// "No value" sentinel for a non-Ok lane's numeric columns.
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Effective deliverable: non-finite or non-positive multiplier defaults to 100
// (matches the legacy portfolio dollar convention).
[[nodiscard]] double eff_multiplier(double m) noexcept {
  return (std::isfinite(m) && m > 0.0) ? m : 100.0;
}

// Run body(i) for i in [0, n), splitting [0, n) into `n_threads` disjoint
// contiguous blocks across std::jthreads (block 0 on the calling thread). Each i
// writes its own output slot, so there is no shared mutable state; the caller's
// serial scatter after this call keeps the reduction order fixed. `n_threads==0`
// selects hardware concurrency; the count is clamped to n.
template <class F> void parallel_blocks(std::size_t n, unsigned n_threads, F &&body) {
  if (n == 0) {
    return;
  }
  unsigned nt = n_threads;
  if (nt == 0) {
    nt = std::max(1u, std::thread::hardware_concurrency());
  }
  nt = std::min<unsigned>(nt, static_cast<unsigned>(n));
  if (nt <= 1) {
    for (std::size_t i = 0; i < n; ++i) {
      body(i);
    }
    return;
  }
  const std::size_t block = (n + nt - 1) / nt;
  std::vector<std::jthread> workers;
  workers.reserve(nt - 1);
  ATX_VOL_COUNT_N(WorkerLaunches, nt - 1);  // helper threads (block 0 stays inline)
  for (unsigned t = 1; t < nt; ++t) {
    const std::size_t lo = std::min(n, static_cast<std::size_t>(t) * block);
    const std::size_t hi = std::min(n, lo + block);
    if (lo >= hi) {
      break;
    }
    workers.emplace_back([lo, hi, &body] {
      for (std::size_t i = lo; i < hi; ++i) {
        body(i);
      }
    });
  }
  const std::size_t hi0 = std::min(n, block);
  for (std::size_t i = 0; i < hi0; ++i) {
    body(i);
  }
  // jthreads join on destruction.
}

// Bit-exact contract identity for dedup.
struct ContractKey {
  std::uint32_t uid;
  std::uint64_t kbits;
  std::uint64_t tbits;
  std::uint8_t side;
  bool operator==(const ContractKey &) const noexcept = default;
};

struct ContractKeyHash {
  [[nodiscard]] std::size_t operator()(const ContractKey &k) const noexcept {
    std::size_t h = std::hash<std::uint32_t>{}(k.uid);
    auto mix = [&h](std::uint64_t v) {
      h ^= std::hash<std::uint64_t>{}(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(k.kbits);
    mix(k.tbits);
    mix(static_cast<std::uint64_t>(k.side));
    return h;
  }
};

[[nodiscard]] ContractKey key_of(const OptionContract &c) noexcept {
  return ContractKey{c.uid, std::bit_cast<std::uint64_t>(c.K), std::bit_cast<std::uint64_t>(c.T),
                     static_cast<std::uint8_t>(c.side)};
}

} // namespace

// ── Portfolio ─────────────────────────────────────────────────────────────

Result<Portfolio> Portfolio::create(std::span<const Position> positions,
                                    const PortfolioBuildOptions &options) {
  Portfolio pf;
  pf.positions_.assign(positions.begin(), positions.end());
  pf.pos_contract_ix_.resize(positions.size());

  std::unordered_map<ContractKey, std::uint32_t, ContractKeyHash> seen;
  constexpr std::size_t kAutoUniqueReserveCap = 65'536u;
  // A hint above the position count cannot be right -- unique contracts are a
  // subset of positions -- so clamp it. An unclamped hint would let a caller
  // reinstate exactly the multi-million-bucket dedup table this option exists to
  // avoid, and a pathological value would throw length_error out of a factory
  // whose contract is to return a Result.
  const std::size_t expected =
      options.expected_unique_contracts > 0
          ? std::min(options.expected_unique_contracts, positions.size())
          : std::min(positions.size(), kAutoUniqueReserveCap);
  seen.reserve(expected);
  for (std::size_t i = 0; i < positions.size(); ++i) {
    const OptionContract &c = positions[i].contract;
    const ContractKey key = key_of(c);
    auto [it, inserted] = seen.try_emplace(key, static_cast<std::uint32_t>(pf.contracts_.size()));
    if (inserted) {
      pf.contracts_.push_back(c);
    }
    pf.pos_contract_ix_[i] = it->second;
  }

  // Unique, sorted uid set (over the deduped contracts).
  pf.uids_.reserve(pf.contracts_.size());
  for (const OptionContract &c : pf.contracts_) {
    pf.uids_.push_back(c.uid);
  }
  std::sort(pf.uids_.begin(), pf.uids_.end());
  pf.uids_.erase(std::unique(pf.uids_.begin(), pf.uids_.end()), pf.uids_.end());

  return pf;
}

// ── SurfaceSet ────────────────────────────────────────────────────────────

Result<SurfaceSet> SurfaceSet::create(std::span<const PricedSurface *const> surfaces) {
  SurfaceSet ss;
  ss.by_uid_.reserve(surfaces.size());
  for (const PricedSurface *s : surfaces) {
    if (s == nullptr) {
      return Err(ErrorCode::InvalidArgument, "SurfaceSet: null surface pointer");
    }
    ss.by_uid_.emplace_back(s->uid(), s);
  }
  std::sort(ss.by_uid_.begin(), ss.by_uid_.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  for (std::size_t i = 1; i < ss.by_uid_.size(); ++i) {
    if (ss.by_uid_[i].first == ss.by_uid_[i - 1].first) {
      return Err(ErrorCode::InvalidArgument, "SurfaceSet: duplicate uid");
    }
  }
  return ss;
}

const PricedSurface *SurfaceSet::find(std::uint32_t uid) const noexcept {
  auto it = std::lower_bound(by_uid_.begin(), by_uid_.end(), uid,
                             [](const auto &e, std::uint32_t u) { return e.first < u; });
  if (it != by_uid_.end() && it->first == uid) {
    return it->second;
  }
  return nullptr;
}

// ── Pricing ───────────────────────────────────────────────────────────────

namespace {

// Per-unique-contract price result. `fair_value` is the American Andersen-Lake
// mark (the accurate served theo); `g` are the American Greeks (cold finite
// differences on american_price, so g.price == fair_value bit-identical).
struct ContractPx {
  double fair_value{0.0};
  AmericanGreeks g{};
  double iv{0.0};
  PriceStatus status{PriceStatus::ModelUnavailable};
};

[[nodiscard]] bool degenerate(const OptionContract &c) noexcept {
  return !(std::isfinite(c.K) && c.K > 0.0 && std::isfinite(c.T) && c.T > 0.0);
}

} // namespace

Result<PriceFrame> PortfolioPricer::price(const SurfaceSet &surfaces,
                                          const PriceOptions &opts) const {
  const std::span<const OptionContract> contracts = pf_.contracts();
  const std::span<const Position> positions = pf_.positions();

  // 1. Parallel: one Greeks solve per UNIQUE contract into disjoint slots.
  std::vector<ContractPx> px(contracts.size());
  parallel_blocks(contracts.size(), opts.n_threads, [&](std::size_t i) {
    const OptionContract &c = contracts[i];
    ContractPx &out = px[i];
    if (degenerate(c)) {
      out.status = PriceStatus::InvalidContract;
      return;
    }
    const PricedSurface *surf = surfaces.find(c.uid);
    if (surf == nullptr) {
      out.status = PriceStatus::ModelUnavailable;
      return;
    }
    // One fused resolution per unique contract: iv + mark (+ Greeks) off a SINGLE
    // surface resolve, killing the old separate surf->iv() resolution that ran
    // ahead of the fair_value/greeks call at the same (K,T,side).
    using EF = PricedSurface::EvalField;
    if (opts.prices_only) {
      const PricedSurface::FusedResult fr =
          surf->evaluate(c.K, c.T, c.side, EF::Iv | EF::Price, /*analytic=*/false);
      out.iv = fr.iv;
      if (!fr.status.has_value() || !std::isfinite(fr.price)) {
        out.status = PriceStatus::NumericError;
        return;
      }
      out.fair_value = fr.price;
      out.status = PriceStatus::Ok;
      return;
    }
    const PricedSurface::FusedResult fr =
        surf->evaluate(c.K, c.T, c.side, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder,
                       opts.analytic_greeks); // iv + greeks + mark, one resolve
    out.iv = fr.iv;
    if (!fr.status.has_value() || !std::isfinite(fr.greeks.price)) {
      out.status = PriceStatus::NumericError;
      return;
    }
    // greeks().price IS the American fair_value (bit-identical, the P0 cold-FD
    // invariant gated by PnlGreeksConsistency), so the mark comes straight off the
    // fused greeks bundle — no separate fair_value() (a duplicate AL solve) and no
    // separate surf->iv() (a duplicate resolution) at the same (K,T,side).
    out.fair_value = fr.greeks.price;
    out.g = fr.greeks;
    out.status = PriceStatus::Ok;
  });

  // 2. Cache-line-disjoint SoA scatter. A million-position book is memory-bandwidth
  // bound here, so fan it out independently of the much smaller unique-contract
  // solve. Totals are reduced in a second fixed-order pass to remain bit-identical
  // for every thread count.
  PriceFrame f;
  const std::size_t n = positions.size();
  // 14 per-row columns = 101 bytes/position (8+4+8*11+1).
  ATX_VOL_COUNT_N(FrameAllocations, 14);
  ATX_VOL_COUNT_N(FrameBytes, n * 101);
  f.id.resize(n);
  f.uid.resize(n);
  f.pv.resize(n);
  f.price.resize(n);
  f.iv.resize(n);
  f.delta.resize(n);
  f.gamma.resize(n);
  f.vega.resize(n);
  f.theta.resize(n);
  f.rho.resize(n);
  f.vanna.resize(n);
  f.volga.resize(n);
  f.charm.resize(n);
  f.status.resize(n);

  parallel_blocks(n, opts.n_threads, [&](std::size_t i) {
    const Position &p = positions[i];
    const ContractPx &c = px[pf_.contract_ix(i)];
    const double w = p.qty * eff_multiplier(p.multiplier);
    f.id[i] = p.id;
    f.uid[i] = p.contract.uid;
    f.status[i] = c.status;
    f.iv[i] = c.iv;
    if (c.status != PriceStatus::Ok) {
      f.pv[i] = kNaN;
      f.price[i] = kNaN;
      f.delta[i] = f.gamma[i] = f.vega[i] = f.theta[i] = f.rho[i] = kNaN;
      f.vanna[i] = f.volga[i] = f.charm[i] = kNaN;
      return;
    }
    const AmericanGreeks &g = c.g;
    f.price[i] = c.fair_value; // American per-share mark
    f.pv[i] = w * c.fair_value;
    if (opts.prices_only) {
      f.delta[i] = f.gamma[i] = f.vega[i] = f.theta[i] = f.rho[i] = kNaN;
      f.vanna[i] = f.volga[i] = f.charm[i] = kNaN;
    } else {
      f.delta[i] = w * g.delta;
      f.gamma[i] = w * g.gamma;
      f.vega[i] = w * g.vega;
      f.theta[i] = w * g.theta;
      f.rho[i] = w * g.rho;
      f.vanna[i] = w * g.vanna;
      f.volga[i] = w * g.volga;
      f.charm[i] = w * g.charm;
    }
  });
  // prices_only leaves every per-lane Greek column NaN, so the aggregate must be
  // NaN as well. PriceTotals default-initializes its Greeks to 0.0; leaving them
  // there would report a finite, clean zero vega alongside n_ok > 0 -- a book that
  // reads as vega-flat when its Greeks were simply never computed.
  if (opts.prices_only) {
    f.total.delta = f.total.gamma = f.total.vega = f.total.theta = f.total.rho = kNaN;
    f.total.vanna = f.total.volga = f.total.charm = kNaN;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (f.status[i] == PriceStatus::Ok) {
      f.total.pv += f.pv[i];
      if (!opts.prices_only) {
        f.total.delta += f.delta[i];
        f.total.gamma += f.gamma[i];
        f.total.vega += f.vega[i];
        f.total.theta += f.theta[i];
        f.total.rho += f.rho[i];
        f.total.vanna += f.vanna[i];
        f.total.volga += f.volga[i];
        f.total.charm += f.charm[i];
      }
      ++f.total.n_ok;
    }
  }
  return f;
}

// ── PnL explain ───────────────────────────────────────────────────────────

namespace {

struct ContractPnl {
  AmericanGreeks gb{};      // base American (cold-FD) Greeks (the Taylor coefficients)
  double price_base{0.0};   // base American mark (fair_value)
  double price_target{0.0}; // shifted American mark (fair_value)
  double dS{0.0};
  double dvol{0.0};
  double dt{0.0};
  double dr{0.0};
  PriceStatus status{PriceStatus::ModelUnavailable};
};

} // namespace

Result<PnlFrame> PortfolioPricer::pnl_explain(const SurfaceSet &base, const SurfaceSet &shifted,
                                              const PriceOptions &opts) const {
  const std::span<const OptionContract> contracts = pf_.contracts();
  const std::span<const Position> positions = pf_.positions();

  // 1. Parallel: per unique contract, base Greeks + target reprice + state moves.
  std::vector<ContractPnl> pnl(contracts.size());
  parallel_blocks(contracts.size(), opts.n_threads, [&](std::size_t i) {
    const OptionContract &c = contracts[i];
    ContractPnl &out = pnl[i];
    if (degenerate(c)) {
      out.status = PriceStatus::InvalidContract;
      return;
    }
    const PricedSurface *sb = base.find(c.uid);
    const PricedSurface *st = shifted.find(c.uid);
    if (sb == nullptr || st == nullptr) {
      out.status = PriceStatus::ModelUnavailable;
      return;
    }
    const double dt =
        static_cast<double>(st->pricing().now_ts_ns - sb->pricing().now_ts_ns) / kNsPerYear;
    const double T_b = c.T;
    const double T_t = T_b - dt;
    if (!(std::isfinite(T_t) && T_t > 0.0)) {
      out.status = PriceStatus::InvalidContract; // rolled past expiry
      return;
    }
    // One fused base resolution: base Greeks + base IV (sig_b) off a SINGLE sb
    // resolve at (K,T_b), killing the old separate sb->iv() that re-resolved sb at
    // the same (K,T_b) the base greeks already resolved.
    using EF = PricedSurface::EvalField;
    const PricedSurface::FusedResult fr_b =
        sb->evaluate(c.K, T_b, c.side, EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder,
                     opts.analytic_greeks); // base greeks + mark + iv
    auto pt = st->fair_value(c.K, T_t, c.side); // shifted mark at the rolled maturity
    if (!fr_b.status.has_value() || !std::isfinite(fr_b.greeks.price) || !pt.has_value() ||
        !std::isfinite(*pt)) {
      out.status = PriceStatus::NumericError;
      return;
    }
    const double sig_b = fr_b.iv;          // == sb->iv(c.K,T_b), reused from the base resolve
    const double sig_t = st->iv(c.K, T_b); // common maturity: term roll stays in theta
    if (!(std::isfinite(sig_b) && std::isfinite(sig_t))) {
      out.status = PriceStatus::NumericError;
      return;
    }
    out.gb = fr_b.greeks;
    out.price_base = fr_b.greeks.price; // == sb->fair_value(c.K,T_b,side), bit-identical, no dup solve
    out.price_target = *pt;
    out.dS = st->pricing().S - sb->pricing().S;
    out.dvol = sig_t - sig_b;
    out.dt = dt;
    out.dr = st->pricing().r - sb->pricing().r;
    out.status = PriceStatus::Ok;
  });

  // 2. Serial scatter (input order) + fixed-order total reduction.
  PnlFrame f;
  const std::size_t n = positions.size();
  // 19 per-row columns = 141 bytes/position (8+4+8*16+1).
  ATX_VOL_COUNT_N(FrameAllocations, 19);
  ATX_VOL_COUNT_N(FrameBytes, n * 141);
  f.id.resize(n);
  f.uid.resize(n);
  f.pv_base.resize(n);
  f.pv_target.resize(n);
  f.pnl_total.resize(n);
  f.pnl_delta.resize(n);
  f.pnl_gamma.resize(n);
  f.pnl_vega.resize(n);
  f.pnl_volga.resize(n);
  f.pnl_vanna.resize(n);
  f.pnl_theta.resize(n);
  f.pnl_rho.resize(n);
  f.pnl_charm.resize(n);
  f.pnl_unexplained.resize(n);
  f.d_spot.resize(n);
  f.d_vol.resize(n);
  f.d_time.resize(n);
  f.d_rate.resize(n);
  f.status.resize(n);

  for (std::size_t i = 0; i < n; ++i) {
    const Position &p = positions[i];
    const ContractPnl &c = pnl[pf_.contract_ix(i)];
    const double w = p.qty * eff_multiplier(p.multiplier);
    f.id[i] = p.id;
    f.uid[i] = p.contract.uid;
    f.status[i] = c.status;
    if (c.status != PriceStatus::Ok) {
      f.pv_base[i] = f.pv_target[i] = kNaN;
      f.pnl_total[i] = f.pnl_delta[i] = f.pnl_gamma[i] = kNaN;
      f.pnl_vega[i] = f.pnl_volga[i] = f.pnl_vanna[i] = kNaN;
      f.pnl_theta[i] = f.pnl_rho[i] = f.pnl_charm[i] = kNaN;
      f.pnl_unexplained[i] = kNaN;
      f.d_spot[i] = f.d_vol[i] = f.d_time[i] = f.d_rate[i] = kNaN;
      continue;
    }
    const AmericanGreeks &g = c.gb;
    // The full American PnL, decomposed by the base AMERICAN (cold-FD) Greeks. The
    // coefficients now carry the early-exercise premium (delta/gamma finite-
    // differenced through american_price), so `unexpl` is the pure higher-order
    // Taylor tail — small for a small move — not the early-exercise gap the old
    // European Black-76 Greeks left behind.
    const double pnl_total_ps = c.price_target - c.price_base;
    const double pd = g.delta * c.dS;
    const double pg = 0.5 * g.gamma * c.dS * c.dS;
    const double pv = g.vega * c.dvol;
    const double pvol = 0.5 * g.volga * c.dvol * c.dvol;
    const double pvanna = g.vanna * c.dS * c.dvol;
    const double pth = g.theta * c.dt;
    const double prho = g.rho * c.dr;
    const double pcharm = g.charm * c.dS * c.dt;
    const double explained = pd + pg + pv + pvol + pvanna + pth + prho + pcharm;
    const double unexpl = pnl_total_ps - explained;

    f.pv_base[i] = w * c.price_base;
    f.pv_target[i] = w * c.price_target;
    f.pnl_total[i] = w * pnl_total_ps;
    f.pnl_delta[i] = w * pd;
    f.pnl_gamma[i] = w * pg;
    f.pnl_vega[i] = w * pv;
    f.pnl_volga[i] = w * pvol;
    f.pnl_vanna[i] = w * pvanna;
    f.pnl_theta[i] = w * pth;
    f.pnl_rho[i] = w * prho;
    f.pnl_charm[i] = w * pcharm;
    f.pnl_unexplained[i] = w * unexpl;
    f.d_spot[i] = c.dS;
    f.d_vol[i] = c.dvol;
    f.d_time[i] = c.dt;
    f.d_rate[i] = c.dr;

    f.total.pv_base += f.pv_base[i];
    f.total.pv_target += f.pv_target[i];
    f.total.pnl_total += f.pnl_total[i];
    f.total.pnl_delta += f.pnl_delta[i];
    f.total.pnl_gamma += f.pnl_gamma[i];
    f.total.pnl_vega += f.pnl_vega[i];
    f.total.pnl_volga += f.pnl_volga[i];
    f.total.pnl_vanna += f.pnl_vanna[i];
    f.total.pnl_theta += f.pnl_theta[i];
    f.total.pnl_rho += f.pnl_rho[i];
    f.total.pnl_charm += f.pnl_charm[i];
    f.total.pnl_unexplained += f.pnl_unexplained[i];
    ++f.total.n_ok;
  }
  return f;
}

} // namespace atx::vol

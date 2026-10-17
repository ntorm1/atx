// Chain-level batch pricing and Greeks. See api/pricing/batch_chain.hpp for the
// contract; implied vol lives in batch_chain_iv.cpp.

#include "atx/vol/api/pricing/batch_chain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "atx/vol/api/simd/american_boundary_batch.hpp"
#include "atx/vol/api/simd/cpu.hpp"
#include "pricing/batch_chain_detail.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

// ── Workspace ────────────────────────────────────────────────────────────────

ChainBatchWorkspace::ChainBatchWorkspace() : impl_(std::make_unique<Impl>()) {}
ChainBatchWorkspace::~ChainBatchWorkspace() = default;
ChainBatchWorkspace::ChainBatchWorkspace(ChainBatchWorkspace &&) noexcept = default;
ChainBatchWorkspace &ChainBatchWorkspace::operator=(ChainBatchWorkspace &&) noexcept = default;

void ChainBatchWorkspace::reserve(std::size_t max_expiries) {
  Impl &w = *impl_;
  if (w.q_eff.size() < max_expiries) {
    w.q_eff.resize(max_expiries);
    w.df.resize(max_expiries);
    w.fwd.resize(max_expiries);
    w.sqrt_t.resize(max_expiries);
    w.expiry_ok.resize(max_expiries);
  }
}

std::size_t ChainBatchWorkspace::expiry_capacity() const noexcept { return impl_->q_eff.size(); }

// ── Carry ────────────────────────────────────────────────────────────────────

double chain_effective_carry(double spot, double T, double carry, double div_pv) noexcept {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  if (!(spot > 0.0 && spot < kInf) || !(T > 0.0 && T < kInf) || !std::isfinite(carry) ||
      !(div_pv >= 0.0 && div_pv < spot)) {
    return batch_chain_detail::kNaN;
  }
  if (div_pv == 0.0) {
    return carry;
  }
  return carry - std::log1p(-div_pv / spot) / T;
}

// ── Shared internals ─────────────────────────────────────────────────────────

namespace batch_chain_detail {

namespace {

[[nodiscard]] bool bytes_overlap(std::span<const std::byte> a,
                                 std::span<const std::byte> b) noexcept {
  if (a.empty() || b.empty()) {
    return false;
  }
  // std::less supplies the total order built-in pointer comparison lacks across
  // unrelated allocations.
  const auto less = std::less<const std::byte *>{};
  return less(a.data(), b.data() + b.size()) && less(b.data(), a.data() + a.size());
}

} // namespace

bool outputs_alias(std::initializer_list<std::span<const std::byte>> outs,
                   std::initializer_list<std::span<const std::byte>> ins) noexcept {
  for (auto o = outs.begin(); o != outs.end(); ++o) {
    for (const std::span<const std::byte> in : ins) {
      if (bytes_overlap(*o, in)) {
        return true;
      }
    }
    for (auto p = o + 1; p != outs.end(); ++p) {
      if (bytes_overlap(*o, *p)) {
        return true;
      }
    }
  }
  return false;
}

void prepare_expiries(double spot, const ChainExpiryColumns &x, ChainBatchWorkspace &ws) {
  const std::size_t m = x.size();
  ws.reserve(m); // no-op (no allocation) when already large enough
  ChainBatchWorkspace::Impl &w = ws.impl();
  for (std::size_t e = 0; e < m; ++e) {
    const double T = x.T[e];
    const double r = x.rate[e];
    const double D = x.div_pv.empty() ? 0.0 : x.div_pv[e];
    // chain_effective_carry validates T, carry and D; the rate is checked here.
    const double qe = std::isfinite(r) ? chain_effective_carry(spot, T, x.carry[e], D) : kNaN;
    const bool ok = std::isfinite(qe);
    w.q_eff[e] = qe;
    w.df[e] = ok ? std::exp(-r * T) : kNaN;
    w.fwd[e] = ok ? std::exp((r - qe) * T) : kNaN;
    w.sqrt_t[e] = ok ? std::sqrt(T) : kNaN;
    w.expiry_ok[e] = ok ? std::uint8_t{1} : std::uint8_t{0};
  }
}

} // namespace batch_chain_detail

namespace {

namespace bcd = batch_chain_detail;

// One call's read-only context, shared by every chunk.
struct PriceCall {
  double spot;
  const ChainExpiryColumns &expiries;
  const ChainContractColumns &contracts;
  const ChainGreekColumns &out;
  const ChainBatchWorkspace::Impl &w;
  const std::optional<AlOpts> &opts;
  bool price_only;
  bool need_vega;
  bool need_rho;
};

void write_failed(const ChainGreekColumns &out, std::size_t i, ChainRowStatus st) noexcept {
  out.price[i] = bcd::kNaN;
  for (const std::span<double> col : {out.delta, out.gamma, out.vega, out.theta, out.rho}) {
    if (!col.empty()) {
      col[i] = bcd::kNaN;
    }
  }
  out.status[i] = st;
}

// A kernel bundle is served only when every requested column is finite (the
// american_greeks_batch rule); otherwise the row is Unpriceable.
void write_bundle(const ChainGreekColumns &out, std::size_t i, const AmericanGreeks &g) noexcept {
  const auto fine = [](std::span<double> col, double v) noexcept {
    return col.empty() || std::isfinite(v);
  };
  if (!std::isfinite(g.price) || !fine(out.delta, g.delta) || !fine(out.gamma, g.gamma) ||
      !fine(out.vega, g.vega) || !fine(out.theta, g.theta) || !fine(out.rho, g.rho)) {
    write_failed(out, i, ChainRowStatus::Unpriceable);
    return;
  }
  out.price[i] = g.price;
  const auto put = [i](std::span<double> col, double v) noexcept {
    if (!col.empty()) {
      col[i] = v;
    }
  };
  put(out.delta, g.delta);
  put(out.gamma, g.gamma);
  put(out.vega, g.vega);
  put(out.theta, g.theta);
  put(out.rho, g.rho);
  out.status[i] = ChainRowStatus::Ok;
}

// Scalar route: exactly the per-contract loop's call at the hoisted carry.
void price_row_scalar(const PriceCall &c, std::size_t i) noexcept {
  const std::size_t n_exp = c.expiries.size();
  if (!bcd::contract_ok(c.contracts, c.w, n_exp, i)) {
    write_failed(c.out, i, ChainRowStatus::InvalidInput);
    return;
  }
  const std::uint32_t e = c.contracts.expiry[i];
  const double K = c.contracts.strike[i];
  const double T = c.expiries.T[e];
  const double r = c.expiries.rate[e];
  const double q = c.w.q_eff[e];
  const double sig = c.contracts.sigma[i];
  const Side side = c.contracts.side[i];
  // SAFETY: the kernels are not noexcept (their Error messages and pricer state can
  // raise std::bad_alloc). This row function runs on pool workers, so the throw is
  // contained here as the row's own failure; anything else stays loud.
  try {
    if (c.price_only) {
      const Result<double> p = andersen_lake(c.spot, K, T, sig, r, q, side, c.opts);
      if (p && std::isfinite(*p)) {
        c.out.price[i] = *p;
        c.out.status[i] = ChainRowStatus::Ok;
      } else {
        write_failed(c.out, i, ChainRowStatus::Unpriceable);
      }
      return;
    }
    const Result<AmericanGreeks> g = american_greeks_al(c.spot, K, T, sig, r, q, side, c.opts,
                                                        c.need_vega, c.need_rho,
                                                        /*need_charm=*/false);
    if (!g) {
      write_failed(c.out, i, ChainRowStatus::Unpriceable);
      return;
    }
    write_bundle(c.out, i, *g);
  } catch (const std::bad_alloc &) {
    write_failed(c.out, i, ChainRowStatus::Unpriceable);
  }
}

// LanedAvx2 route: the chunk's valid puts, then its valid calls, in input order,
// through the public laned kernels. The pack composition is a function of the
// chunk alone, so it does not move with the thread count.
void price_chunk_laned(const PriceCall &c, std::size_t lo, std::size_t hi) noexcept {
  const std::size_t n_exp = c.expiries.size();
  std::array<double, kChainBatchChunk> S{};
  std::array<double, kChainBatchChunk> K{};
  std::array<double, kChainBatchChunk> T{};
  std::array<double, kChainBatchChunk> sig{};
  std::array<double, kChainBatchChunk> r{};
  std::array<double, kChainBatchChunk> q{};
  std::array<std::size_t, kChainBatchChunk> row{};
  std::array<AmericanGreeks, kChainBatchChunk> g{};
  for (std::size_t i = lo; i < hi; ++i) {
    if (!bcd::contract_ok(c.contracts, c.w, n_exp, i)) {
      write_failed(c.out, i, ChainRowStatus::InvalidInput);
    }
  }
  for (const Side side : {Side::Put, Side::Call}) {
    std::size_t m = 0;
    for (std::size_t i = lo; i < hi; ++i) {
      if (c.contracts.side[i] != side || !bcd::contract_ok(c.contracts, c.w, n_exp, i)) {
        continue;
      }
      const std::uint32_t e = c.contracts.expiry[i];
      S[m] = c.spot;
      K[m] = c.contracts.strike[i];
      T[m] = c.expiries.T[e];
      sig[m] = c.contracts.sigma[i];
      r[m] = c.expiries.rate[e];
      q[m] = c.w.q_eff[e];
      row[m] = i;
      ++m;
    }
    if (m == 0u) {
      continue;
    }
    // The returned route is a dispatch statistic only; the values are what count.
    if (side == Side::Put) {
      (void)simd::american_put_greeks_batch(S.data(), K.data(), T.data(), sig.data(), r.data(),
                                            q.data(), m, c.opts, g.data(),
                                            simd::SimdIsa::ForceAvx2, c.need_vega, c.need_rho,
                                            /*need_charm=*/false);
    } else {
      (void)simd::american_call_greeks_batch(S.data(), K.data(), T.data(), sig.data(), r.data(),
                                             q.data(), m, c.opts, g.data(),
                                             simd::SimdIsa::ForceAvx2, c.need_vega, c.need_rho,
                                             /*need_charm=*/false);
    }
    for (std::size_t j = 0; j < m; ++j) {
      write_bundle(c.out, row[j], g[j]);
    }
  }
}

[[nodiscard]] bool shapes_ok(double spot, const ChainExpiryColumns &x,
                             const ChainContractColumns &c, const ChainGreekColumns &out) noexcept {
  return spot > 0.0 && spot < std::numeric_limits<double>::infinity() && x.consistent() &&
         c.consistent() && out.consistent() && out.size() == c.size();
}

[[nodiscard]] bool aliases(const ChainExpiryColumns &x, const ChainContractColumns &c,
                           const ChainGreekColumns &out) noexcept {
  return bcd::outputs_alias(
      {std::as_bytes(out.price), std::as_bytes(out.delta), std::as_bytes(out.gamma),
       std::as_bytes(out.vega), std::as_bytes(out.theta), std::as_bytes(out.rho),
       std::as_bytes(out.status)},
      {std::as_bytes(x.T), std::as_bytes(x.rate), std::as_bytes(x.carry), std::as_bytes(x.div_pv),
       std::as_bytes(c.strike), std::as_bytes(c.expiry), std::as_bytes(c.side),
       std::as_bytes(c.sigma)});
}

} // namespace

Result<std::size_t> price_chain_batch(double spot, const ChainExpiryColumns &expiries,
                                      const ChainContractColumns &contracts,
                                      const ChainGreekColumns &out,
                                      const ChainBatchOptions &options, ChainBatchWorkspace &ws) {
  if (!shapes_ok(spot, expiries, contracts, out)) {
    return Err(ErrorCode::InvalidArgument,
               "price_chain_batch: inconsistent columns, output length != contract count, or "
               "spot not finite > 0");
  }
  if (aliases(expiries, contracts, out)) {
    return Err(ErrorCode::InvalidArgument,
               "price_chain_batch: an output overlaps an input or another output");
  }
  if (options.route != ChainBatchRoute::Scalar && options.route != ChainBatchRoute::LanedAvx2) {
    return Err(ErrorCode::InvalidArgument, "price_chain_batch: unknown route");
  }
  const std::size_t n = contracts.size();
  if (n == 0u) {
    return Ok(n);
  }
  try {
    batch_chain_detail::prepare_expiries(spot, expiries, ws);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Internal); // no message: building one would allocate again
  }
  const PriceCall call{spot,
                       expiries,
                       contracts,
                       out,
                       ws.impl(),
                       options.al_opts,
                       out.price_only(),
                       !out.vega.empty(),
                       !out.rho.empty()};
  const bool laned = options.route == ChainBatchRoute::LanedAvx2;
  batch_chain_detail::for_each_chunk(
      n, options.executor, options.n_threads,
      [&call, laned](std::size_t lo, std::size_t hi) noexcept {
        if (laned) {
          price_chunk_laned(call, lo, hi);
          return;
        }
        for (std::size_t i = lo; i < hi; ++i) {
          price_row_scalar(call, i);
        }
      });
  return Ok(n);
}

} // namespace atx::vol

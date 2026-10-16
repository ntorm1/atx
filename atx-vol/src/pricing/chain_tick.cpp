// Tick-path chain refresh. See api/pricing/chain_tick.hpp for the contract.

#include "atx/vol/api/pricing/chain_tick.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] bool shapes_ok(double spot, const ChainContractsView &chain,
                             const ChainTickOutputs &out) noexcept {
  return std::isfinite(spot) && spot > 0.0 && chain.consistent() && out.consistent() &&
         out.size() == chain.size();
}

void write_nan(const ChainTickOutputs &out, std::size_t i, TickStatus st) noexcept {
  out.price[i] = kNaN;
  out.delta[i] = kNaN;
  out.gamma[i] = kNaN;
  out.vega[i] = kNaN;
  out.theta[i] = kNaN;
  out.status[i] = st;
}

} // namespace

Status refresh_chain_cached(double spot, const ChainContractsView &chain,
                            const AmericanCorrectionCaches &caches, const ChainTickOutputs &out) {
  if (!shapes_ok(spot, chain, out)) {
    return Err(ErrorCode::InvalidArgument,
               "refresh_chain_cached: column lengths disagree or spot is not finite > 0");
  }
  const std::size_t n = chain.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Side side = chain.side[i];
    const CorrectionCache *cache = caches.for_side(side);
    const bool usable = cache != nullptr && cache->populated() && cache->side() == side;
    const Result<AmericanGreeks> g = american_greeks(spot, chain.strike[i], chain.expiry[i],
                                                     chain.sigma[i], chain.rate[i],
                                                     chain.carry[i], side, cache);
    if (!g) {
      write_nan(out, i, TickStatus::Unpriceable);
      continue;
    }
    out.price[i] = g->price;
    out.delta[i] = g->delta;
    out.gamma[i] = g->gamma;
    out.vega[i] = g->vega;
    out.theta[i] = g->theta;
    out.status[i] = usable ? TickStatus::Ok : TickStatus::Fallback;
  }
  return Ok();
}

// ── ChainRecords ─────────────────────────────────────────────────────────────

Result<ChainRecords> ChainRecords::build(double spot, const ChainContractsView &chain,
                                         const ChainRecordPolicy &policy) {
  if (!std::isfinite(spot) || !(spot > 0.0) || !chain.consistent() ||
      !(policy.spot_move >= 0.0) || !(policy.sigma_lo_mult > 0.0) ||
      !(policy.sigma_hi_mult >= policy.sigma_lo_mult)) {
    return Err(ErrorCode::InvalidArgument,
               "ChainRecords::build: bad spot, column lengths, or policy");
  }
  const std::size_t n = chain.size();
  // Group contracts by exact (T, r, q, side); groups keep first-seen order.
  struct Group {
    double T, r, q;
    Side side;
    double k_lo, k_hi, s_lo, s_hi;
  };
  std::vector<Group> groups;
  std::vector<std::uint32_t> group_of(n, 0u);
  for (std::size_t i = 0; i < n; ++i) {
    const double T = chain.expiry[i];
    const double r = chain.rate[i];
    const double q = chain.carry[i];
    const Side sd = chain.side[i];
    const double K = chain.strike[i];
    const double sig = chain.sigma[i];
    if (!(K > 0.0) || !(sig > 0.0) || !(T > 0.0) || !std::isfinite(r) || !std::isfinite(q)) {
      return Err(ErrorCode::InvalidArgument,
                 "ChainRecords::build: every contract needs K, sigma, T > 0 and finite carry");
    }
    const double k = std::log(K / (spot * std::exp((r - q) * T)));
    std::size_t g = 0;
    while (g < groups.size() && !(groups[g].T == T && groups[g].r == r && groups[g].q == q &&
                                  groups[g].side == sd)) {
      ++g;
    }
    if (g == groups.size()) {
      groups.push_back(Group{T, r, q, sd, k, k, sig, sig});
    }
    Group &G = groups[g];
    G.k_lo = std::min(G.k_lo, k);
    G.k_hi = std::max(G.k_hi, k);
    G.s_lo = std::min(G.s_lo, sig);
    G.s_hi = std::max(G.s_hi, sig);
    group_of[i] = static_cast<std::uint32_t>(g);
  }

  ChainRecords out;
  out.bank_ = ExpiryRecordBank(groups.size());
  out.worst_cert_ = 0.0;
  for (const Group &G : groups) {
    ExpiryRecordSpec spec;
    spec.expiry = G.T;
    spec.carry = ExpiryCarry{G.r, G.q, {}};
    spec.side = G.side;
    // A one-strike group still needs a box: pad by the spot move or a small floor
    // so k_min < k_max holds.
    const double pad = std::max(policy.spot_move, 1.0e-3);
    spec.k_min = G.k_lo - pad;
    spec.k_max = G.k_hi + pad;
    spec.sigma_min = policy.sigma_lo_mult * G.s_lo;
    spec.sigma_max = policy.sigma_hi_mult * G.s_hi;
    if (!(spec.sigma_max > spec.sigma_min)) {
      spec.sigma_max = spec.sigma_min * 1.25;
    }
    spec.sample_opts = policy.sample_opts;
    spec.certify = policy.certify;
    Result<ExpiryRecord> rec = ExpiryRecord::build(spec);
    if (!rec) {
      return Err(rec.error());
    }
    const double ce = rec->certified_max_abs_error_fwd();
    out.worst_cert_ = std::isnan(ce) ? ce : std::max(out.worst_cert_, ce);
    const Result<std::size_t> idx = out.bank_.insert(*rec);
    if (!idx) {
      return Err(idx.error());
    }
  }
  out.record_of_ = std::move(group_of);
  return Ok(std::move(out));
}

Status refresh_chain_records(double spot, const ChainContractsView &chain,
                             const ChainRecords &records, const ChainTickOutputs &out) {
  if (!shapes_ok(spot, chain, out) || records.record_of_contract().size() != chain.size()) {
    return Err(ErrorCode::InvalidArgument,
               "refresh_chain_records: column lengths disagree, spot is not finite > 0, or the "
               "records were built for another chain");
  }
  const std::span<const std::uint32_t> rec_of = records.record_of_contract();
  const ExpiryRecordBank &bank = records.bank();
  const std::size_t n = chain.size();
  for (std::size_t i = 0; i < n; ++i) {
    const ExpiryRecord *rec = bank.at(rec_of[i]);
    const double K = chain.strike[i];
    const double sig = chain.sigma[i];
    if (rec == nullptr || !(K > 0.0) || !(sig > 0.0)) {
      write_nan(out, i, TickStatus::Unpriceable);
      continue;
    }
    const RecordGreeks g = rec->greeks(spot, K, sig);
    out.price[i] = g.price;
    out.delta[i] = g.delta;
    out.gamma[i] = g.gamma;
    out.vega[i] = g.vega;
    out.theta[i] = g.theta;
    out.status[i] = g.exercised ? TickStatus::Exercised
                                : (g.in_box ? TickStatus::Ok : TickStatus::Fallback);
  }
  return Ok();
}

Status implied_vols_from_records(double spot, const ChainContractsView &chain,
                                 const ChainRecords &records, std::span<const double> target_price,
                                 std::span<double> sigma_out,
                                 std::span<RecordIvStatus> status_out) {
  const std::size_t n = chain.size();
  if (!std::isfinite(spot) || !(spot > 0.0) || !chain.consistent() ||
      records.record_of_contract().size() != n || target_price.size() != n ||
      sigma_out.size() != n || status_out.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "implied_vols_from_records: span lengths disagree or spot is not finite > 0");
  }
  const std::span<const std::uint32_t> rec_of = records.record_of_contract();
  const ExpiryRecordBank &bank = records.bank();
  for (std::size_t i = 0; i < n; ++i) {
    const ExpiryRecord *rec = bank.at(rec_of[i]);
    if (rec == nullptr) {
      sigma_out[i] = kNaN;
      status_out[i] = RecordIvStatus::Invalid;
      continue;
    }
    const RecordIv iv = rec->implied_vol(spot, chain.strike[i], target_price[i], chain.sigma[i]);
    sigma_out[i] = iv.sigma;
    status_out[i] = iv.status;
  }
  return Ok();
}

} // namespace atx::vol

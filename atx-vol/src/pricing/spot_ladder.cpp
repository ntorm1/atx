// Per-contract Chebyshev spot ladders — the tick path of api/pricing/chain_tick.hpp
// (pricing hot-path audit 2026-10-16, items 1 and 4). See the ChainSpotLadder
// section of that header for the representation, certification and window
// policy; this file is the implementation.

#include "atx/vol/api/pricing/chain_tick.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "fitting/correction_detail.hpp" // cheb_node / cheb_dct2 / unit maps

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr std::size_t kL = kSpotLadderLanes;
constexpr std::uint16_t kN = kSpotLadderNodes;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// Window halvings tried when a contract's certificate fails (build or rebuild).
constexpr int kMaxShrink = 6;
// Ulp-scale floor on every dollar certificate (8 ulps of max(F_max, K)): the
// record certificate is a forward-unit grid bound, and a few-hundred-dollar price
// carries rounding it cannot see (tick-path review v2: one 5.0e-13 overshoot at
// S ~ 631).
constexpr double kCertUlps = 8.0;

// Status codes the block kernel emits; OutOfWindow lanes are resolved afterwards
// by the rebuild / record-fallback pass.
constexpr std::uint8_t kLaneOk = static_cast<std::uint8_t>(TickStatus::Ok);
constexpr std::uint8_t kLaneExercised = static_cast<std::uint8_t>(TickStatus::Exercised);
constexpr std::uint8_t kLaneOut = static_cast<std::uint8_t>(TickStatus::OutOfWindow);

// kL consecutive contracts. Coefficients are lane-interleaved so each Clenshaw
// step reads one contiguous kL-double row for all lanes.
struct alignas(64) Block {
  double pc[kN][kL]; // price series in x
  double vc[kN][kL]; // vega series in x
  double alpha[kL];  // x = alpha * S + beta maps the continuation domain to [-1, 1]
  double beta[kL];
  double lo[kL]; // window [lo, hi]; a refused contract has lo = +inf, hi = -inf
  double hi[kL];
  double s_ex[kL];   // exercise split: put exercised iff S <= s_ex, call iff S >= s_ex
  double strike[kL];
  double put[kL];  // 1.0 for a put, 0.0 for a call
  double r[kL];    // theta = r V - rq S delta - hs2 S^2 gamma
  double rq[kL];
  double hs2[kL];
};

// One tick of one block: every lane's outputs plus a lane status. Branch-free
// so the lane loops vectorize; lanes outside their window still compute (finite
// garbage) and are flagged kLaneOut for the caller to resolve.
struct LaneOut {
  double price[kL], delta[kL], gamma[kL], vega[kL], theta[kL];
  std::uint8_t status[kL];
};

inline void eval_block(const Block &B, double S, LaneOut &o) noexcept {
  double x[kL], tx[kL];
  double b1[kL], b2[kL], d1[kL], d2[kL], e1[kL], e2[kL], v1[kL], v2[kL];
  for (std::size_t l = 0; l < kL; ++l) {
    x[l] = B.alpha[l] * S + B.beta[l];
    tx[l] = 2.0 * x[l];
    b1[l] = b2[l] = d1[l] = d2[l] = e1[l] = e2[l] = v1[l] = v2[l] = 0.0;
  }
  // Clenshaw for p(x), p'(x), p''(x) (differentiated recurrences) and the vega
  // series, all four sharing the step. Same full-c0 form as the record.
  for (int k = static_cast<int>(kN) - 1; k >= 1; --k) {
    const double *pk = B.pc[k];
    const double *vk = B.vc[k];
    for (std::size_t l = 0; l < kL; ++l) {
      const double b = pk[l] + tx[l] * b1[l] - b2[l];
      const double d = 2.0 * b1[l] + tx[l] * d1[l] - d2[l];
      const double e = 4.0 * d1[l] + tx[l] * e1[l] - e2[l];
      const double v = vk[l] + tx[l] * v1[l] - v2[l];
      b2[l] = b1[l];
      b1[l] = b;
      d2[l] = d1[l];
      d1[l] = d;
      e2[l] = e1[l];
      e1[l] = e;
      v2[l] = v1[l];
      v1[l] = v;
    }
  }
  for (std::size_t l = 0; l < kL; ++l) {
    const double P = B.pc[0][l] + x[l] * b1[l] - b2[l];
    const double Px = b1[l] + x[l] * d1[l] - d2[l];
    const double Pxx = 2.0 * d1[l] + x[l] * e1[l] - e2[l];
    const double V = B.vc[0][l] + x[l] * v1[l] - v2[l];
    const bool is_put = B.put[l] != 0.0;
    const bool in_win = (S >= B.lo[l]) && (S <= B.hi[l]);
    const bool ex = is_put ? (S <= B.s_ex[l]) : (S >= B.s_ex[l]);
    const double intr_raw = is_put ? (B.strike[l] - S) : (S - B.strike[l]);
    const double intr = intr_raw > 0.0 ? intr_raw : 0.0;
    // The served price is max(series, intrinsic). Where the floor wins, the lane
    // is served the floor's own Greeks — the exercise convention for intrinsic,
    // zero for a worthless floor — so the Greeks always belong to the served
    // price (review dcb6e3f1 nit 12). An intrinsic floor reports Exercised.
    const bool floored = !(P > intr);
    const bool at_floor = ex || floored;
    const double delta = B.alpha[l] * Px;
    const double gamma = B.alpha[l] * B.alpha[l] * Pxx;
    const double floor_delta = (ex || intr > 0.0) ? (is_put ? -1.0 : 1.0) : 0.0;
    o.price[l] = at_floor ? intr : P;
    o.delta[l] = at_floor ? floor_delta : delta;
    o.gamma[l] = at_floor ? 0.0 : gamma;
    o.vega[l] = at_floor ? 0.0 : V;
    o.theta[l] = at_floor ? 0.0 : (B.r[l] * P - B.rq[l] * S * delta - B.hs2[l] * S * S * gamma);
    const bool exercised = ex || (floored && intr > 0.0);
    o.status[l] = !in_win ? kLaneOut : (exercised ? kLaneExercised : kLaneOk);
  }
}

// Price of one lane's series at S (no exercise / window logic): certification.
[[nodiscard]] double lane_series_price(const Block &B, std::size_t l, double S) noexcept {
  const double x = B.alpha[l] * S + B.beta[l];
  const double two_x = 2.0 * x;
  double b1 = 0.0, b2 = 0.0;
  for (int k = static_cast<int>(kN) - 1; k >= 1; --k) {
    const double b = B.pc[k][l] + two_x * b1 - b2;
    b2 = b1;
    b1 = b;
  }
  return B.pc[0][l] + x * b1 - b2;
}

} // namespace

struct ChainSpotLadder::Impl {
  std::size_t n = 0;
  std::vector<Block> blocks;
  std::vector<double> sigma;
  std::vector<double> strike;
  std::vector<Side> side;
  std::vector<std::uint32_t> rec_of;
  std::vector<double> half_width; // current ln-spot window half-width per contract
  // Spot range [box_lo, box_hi] that keeps the contract's (k, sigma) inside its
  // record's certified box: k = ln(K / (S g)) in [k_min, k_max] <=> S in
  // [K e^{-k_max} / g, K e^{-k_min} / g]. Empty (+inf, -inf) when sigma itself is
  // outside the record's sigma box. Every window is clipped to it, so a lane the
  // kernel reports Ok or Exercised is always inside the box the record's
  // certificate covers; a spot outside it is served from the record (Fallback).
  std::vector<double> box_lo;
  std::vector<double> box_hi;
  std::vector<std::uint8_t> refused;
  std::vector<std::uint8_t> sigma_out; // sigma outside the record's sigma box
  std::vector<double> cert_usd;
  std::vector<std::uint32_t> pending; // out-of-window scratch, sized n at build
  // Rebuild priority: |ln K - ln g - ln S| x inv_sd = |ln(K / F)| / (sigma sqrt T).
  std::vector<double> ln_k_over_g;
  std::vector<double> inv_sd;
  // In-box rebuild candidates of one tick and their (key, contract) ranks:
  // scratch sized n at build, so the tick's partial sort allocates nothing.
  std::vector<std::uint32_t> candidates;
  std::vector<std::pair<double, std::uint32_t>> ranked;
  ChainRecords records;
  SpotLadderPolicy policy;
  SpotLadderCounters counters;
  std::size_t refused_count = 0;
  std::size_t sigma_out_count = 0;

  [[nodiscard]] const ExpiryRecord &record(std::size_t c) const noexcept {
    return *records.bank().at(rec_of[c]);
  }
  [[nodiscard]] bool spot_in_box(std::size_t c, double S) const noexcept {
    return S >= box_lo[c] && S <= box_hi[c];
  }

  // (Re)build contract c's ladder centred on `center`. Returns true when the
  // contract has a certified window containing `center`; false when it is
  // refused (certificate over policy, or an uncertified record) or `center` is
  // outside its record box (no window until the spot comes back).
  bool build_one(std::size_t c, double center) noexcept;
  void refuse(std::size_t c) noexcept;
  void clear_window(std::size_t c) noexcept;
  // Serve contract c from its record (Fallback) and count it in `bucket`.
  void serve_from_record(std::size_t c, double S, const ChainTickOutputs &out,
                         std::uint64_t &bucket) noexcept;
  // Certificate acceptance threshold at forward F_max (SpotLadderPolicy).
  [[nodiscard]] double cert_limit(double f_max) const noexcept {
    return std::max(policy.max_cert_usd, policy.max_cert_rel * f_max);
  }
};

void ChainSpotLadder::Impl::clear_window(std::size_t c) noexcept {
  Block &B = blocks[c / kL];
  const std::size_t l = c % kL;
  B.lo[l] = kInf; // never inside: every tick routes it to the pending pass
  B.hi[l] = -kInf;
}

void ChainSpotLadder::Impl::refuse(std::size_t c) noexcept {
  clear_window(c);
  if (refused[c] == 0u) {
    refused[c] = 1u;
    ++refused_count;
  }
}

bool ChainSpotLadder::Impl::build_one(std::size_t c, double center) noexcept {
  const ExpiryRecord &rec = record(c);
  const double K = strike[c];
  const double sig = sigma[c];
  const bool is_put = (side[c] == Side::Put);
  const double g = rec.forward_factor();
  Block &B = blocks[c / kL];
  const std::size_t l = c % kL;

  // Spot of the exercise split: k = ln(K / (S g)) crosses the record's edge.
  double s_ex = is_put ? -kInf : kInf;
  if (rec.has_exercise_edge()) {
    const double e = rec.exercise_edge(sig);
    if (std::isfinite(e)) {
      s_ex = K * std::exp(-e) / g;
    }
  }
  // The ladder's certificate stands on the record's: an uncertified record
  // (certify = false, NaN) gives nothing to stand on, so the contract is refused
  // and served from its record, flagged Fallback.
  const double rec_cert = rec.certified_max_abs_error_fwd();
  if (!std::isfinite(rec_cert)) {
    cert_usd[c] = kNaN;
    refuse(c);
    return false;
  }
  // The record certificate covers its (k, sigma) box only. A centre outside the
  // box has no certifiable window: serve the record until the spot comes back.
  if (!spot_in_box(c, center)) {
    cert_usd[c] = kNaN;
    clear_window(c);
    return false;
  }

  double delta = half_width[c];
  double cert = kInf;
  double limit = policy.max_cert_usd;
  double prev_lo = kNaN;
  double prev_hi = kNaN;
  for (int attempt = 0; attempt <= kMaxShrink; ++attempt) {
    // Window clipped to the box, so every Ok / Exercised lane is in the box.
    const double lo = std::max(center * std::exp(-delta), box_lo[c]);
    const double hi = std::min(center * std::exp(delta), box_hi[c]);
    if (lo == prev_lo && hi == prev_hi) {
      // The box clips both sides: halving delta no longer moves the window, so
      // another attempt would recompute the same certificate (review v2 nit 5).
      break;
    }
    prev_lo = lo;
    prev_hi = hi;
    // Record error in dollars = F x the forward-unit certificate; F is largest
    // at the window's top. Plus the ulp floor at the largest price scale.
    const double f_max = hi * g;
    const double ulp_floor =
        kCertUlps * std::numeric_limits<double>::epsilon() * std::max(f_max, K);
    const double rec_cert_usd = rec_cert * f_max + ulp_floor;
    limit = cert_limit(f_max);
    const double a = is_put ? std::max(lo, s_ex) : lo;
    const double b = is_put ? hi : std::min(hi, s_ex);
    B.lo[l] = lo;
    B.hi[l] = hi;
    B.s_ex[l] = s_ex;
    if (!(b > a)) {
      // The whole window is on the exercise side: intrinsic everywhere.
      for (std::uint16_t k = 0; k < kN; ++k) {
        B.pc[k][l] = 0.0;
        B.vc[k][l] = 0.0;
      }
      B.alpha[l] = 0.0;
      B.beta[l] = 0.0;
      // Intrinsic on the exercise side of the record's edge: exactly the
      // record's value there, which its certificate covers (it probes the
      // exercise side too).
      cert = rec_cert_usd;
      break;
    }
    std::array<double, kN> pv{};
    std::array<double, kN> vv{};
    std::array<double, kN> pcoef{};
    std::array<double, kN> vcoef{};
    for (std::uint16_t j = 0; j < kN; ++j) {
      const double Sj = detail::cheb_from_unit(detail::cheb_node(j, kN), a, b);
      double vega = 0.0;
      pv[j] = rec.price_and_vega(Sj, K, sig, &vega);
      vv[j] = vega;
    }
    detail::cheb_dct2(pv.data(), pcoef.data(), kN);
    detail::cheb_dct2(vv.data(), vcoef.data(), kN);
    for (std::uint16_t k = 0; k < kN; ++k) {
      B.pc[k][l] = pcoef[k];
      B.vc[k][l] = vcoef[k];
    }
    B.alpha[l] = 2.0 / (b - a);
    B.beta[l] = -(a + b) / (b - a);
    // Certificate: series vs record at every midpoint between adjacent nodes
    // (sorted ascending in x) and at both domain ends.
    std::array<double, kN> xs{};
    for (std::uint16_t j = 0; j < kN; ++j) {
      xs[j] = detail::cheb_node(j, kN);
    }
    std::sort(xs.begin(), xs.end());
    double worst = 0.0;
    const auto probe = [&](double x) {
      const double S = detail::cheb_from_unit(x, a, b);
      const double err = std::abs(lane_series_price(B, l, S) - rec.price(S, K, sig));
      worst = std::max(worst, std::isfinite(err) ? err : kInf);
    };
    for (std::uint16_t j = 0; j + 1 < kN; ++j) {
      probe(0.5 * (xs[j] + xs[j + 1]));
    }
    probe(-1.0);
    probe(1.0);
    cert = worst + rec_cert_usd;
    if (cert <= limit || delta <= policy.window_min) {
      break;
    }
    delta = std::max(0.5 * delta, policy.window_min);
  }
  half_width[c] = delta;
  cert_usd[c] = cert;
  if (!(cert <= limit)) {
    refuse(c);
    return false;
  }
  return true;
}

void ChainSpotLadder::Impl::serve_from_record(std::size_t c, double S, const ChainTickOutputs &out,
                                              std::uint64_t &bucket) noexcept {
  const RecordGreeks g = record(c).greeks(S, strike[c], sigma[c]);
  out.price[c] = g.price;
  out.delta[c] = g.delta;
  out.gamma[c] = g.gamma;
  out.vega[c] = g.vega;
  out.theta[c] = g.theta;
  out.status[c] = std::isfinite(g.price) ? TickStatus::Fallback : TickStatus::Unpriceable;
  ++bucket;
}

ChainSpotLadder::ChainSpotLadder() = default;
ChainSpotLadder::~ChainSpotLadder() = default;
ChainSpotLadder::ChainSpotLadder(ChainSpotLadder &&) noexcept = default;
ChainSpotLadder &ChainSpotLadder::operator=(ChainSpotLadder &&) noexcept = default;
ChainSpotLadder::ChainSpotLadder(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Result<ChainSpotLadder> ChainSpotLadder::build(double spot, const ChainContractsView &chain,
                                               const ChainRecords &records,
                                               const SpotLadderPolicy &policy) {
  const std::size_t n = chain.size();
  if (!std::isfinite(spot) || !(spot > 0.0) || !chain.consistent() ||
      records.record_of_contract().size() != n || !(policy.window_min > 0.0) ||
      !(policy.window_max >= policy.window_min) || !(policy.window_sigmas > 0.0) ||
      !(policy.max_cert_usd > 0.0) || !(policy.max_cert_rel >= 0.0) ||
      !std::isfinite(policy.max_cert_rel) ||
      (policy.rebuild_order != SpotLadderRebuildOrder::ContractIndex &&
       policy.rebuild_order != SpotLadderRebuildOrder::NearestAtmFirst)) {
    return Err(ErrorCode::InvalidArgument,
               "ChainSpotLadder::build: bad spot, column lengths, records or policy");
  }
  auto impl = std::make_unique<Impl>();
  impl->n = n;
  impl->policy = policy;
  impl->records = records;
  impl->blocks.assign((n + kL - 1) / kL, Block{});
  impl->sigma.assign(chain.sigma.begin(), chain.sigma.end());
  impl->strike.assign(chain.strike.begin(), chain.strike.end());
  impl->side.assign(chain.side.begin(), chain.side.end());
  const std::span<const std::uint32_t> rec_of = records.record_of_contract();
  impl->rec_of.assign(rec_of.begin(), rec_of.end());
  impl->half_width.assign(n, policy.window_max);
  impl->box_lo.assign(n, kInf);
  impl->box_hi.assign(n, -kInf);
  impl->refused.assign(n, 0u);
  impl->sigma_out.assign(n, 0u);
  impl->cert_usd.assign(n, kNaN);
  impl->pending.assign(n, 0u);
  impl->ln_k_over_g.assign(n, 0.0);
  impl->inv_sd.assign(n, 0.0);
  impl->candidates.assign(n, 0u);
  impl->ranked.assign(n, std::pair<double, std::uint32_t>{0.0, 0u});

  // Padding lanes of the last block mirror a refused contract: never in window,
  // never written (the tick only writes lanes < n).
  for (Block &B : impl->blocks) {
    for (std::size_t l = 0; l < kL; ++l) {
      B.lo[l] = kInf;
      B.hi[l] = -kInf;
      B.put[l] = 1.0;
      B.s_ex[l] = -kInf;
    }
  }
  for (std::size_t c = 0; c < n; ++c) {
    const ExpiryRecord *rec = records.bank().at(rec_of[c]);
    const double K = chain.strike[c];
    const double sig = chain.sigma[c];
    const double T = chain.expiry[c];
    if (rec == nullptr || !(K > 0.0) || !(sig > 0.0) || !(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "ChainSpotLadder::build: every contract needs a record, K, sigma, T > 0");
    }
    Block &B = impl->blocks[c / kL];
    const std::size_t l = c % kL;
    B.strike[l] = K;
    B.put[l] = (chain.side[c] == Side::Put) ? 1.0 : 0.0;
    B.r[l] = rec->rate();
    B.rq[l] = rec->rate() - rec->carry();
    B.hs2[l] = 0.5 * sig * sig;
    impl->half_width[c] =
        std::clamp(policy.window_sigmas * sig * std::sqrt(T), policy.window_min, policy.window_max);
    impl->ln_k_over_g[c] = std::log(K) - std::log(rec->forward_factor());
    impl->inv_sd[c] = 1.0 / (sig * std::sqrt(T));
    if (sig >= rec->sigma_min() && sig <= rec->sigma_max()) {
      // Pulled in by a relative 1e-12 so that rounding in ln(K / (S g)) at the
      // very edge can never land a served lane a few ulps outside the box.
      const double g = rec->forward_factor();
      impl->box_lo[c] = K * std::exp(-rec->k_max()) / g * (1.0 + 1.0e-12);
      impl->box_hi[c] = K * std::exp(-rec->k_min()) / g * (1.0 - 1.0e-12);
    } else {
      impl->sigma_out[c] = 1u; // no window ever; served from the record until a refit
      ++impl->sigma_out_count;
    }
    (void)impl->build_one(c, spot);
  }
  return Ok(ChainSpotLadder(std::move(impl)));
}

Status ChainSpotLadder::refresh(double spot, const ChainTickOutputs &out) {
  if (!impl_ || !std::isfinite(spot) || !(spot > 0.0) || !out.consistent() ||
      out.size() != impl_->n) {
    return Err(ErrorCode::InvalidArgument,
               "ChainSpotLadder::refresh: output length mismatch or spot not finite > 0");
  }
  Impl &m = *impl_;
  const std::size_t n = m.n;
  std::size_t n_pending = 0;
  LaneOut o;
  for (std::size_t b = 0; b < m.blocks.size(); ++b) {
    eval_block(m.blocks[b], spot, o);
    const std::size_t base = b * kL;
    const std::size_t lanes = std::min(kL, n - base);
    for (std::size_t l = 0; l < lanes; ++l) {
      out.price[base + l] = o.price[l];
      out.delta[base + l] = o.delta[l];
      out.gamma[base + l] = o.gamma[l];
      out.vega[base + l] = o.vega[l];
      out.theta[base + l] = o.theta[l];
      out.status[base + l] = static_cast<TickStatus>(o.status[l]);
      if (o.status[l] == kLaneOut) {
        m.pending[n_pending++] = static_cast<std::uint32_t>(base + l);
      }
    }
  }
  // Resolve window exits: rebuild within budget, otherwise serve the record. A
  // spot outside the contract's record box cannot be certified at all, so it is
  // served from the record without spending the rebuild budget. Each fallback is
  // counted in the bucket that says why (SpotLadderCounters).
  SpotLadderCounters &k = m.counters;
  std::size_t n_cand = 0;
  for (std::size_t p = 0; p < n_pending; ++p) {
    const std::size_t c = m.pending[p];
    if (m.refused[c] != 0u) {
      m.serve_from_record(c, spot, out, k.refused_fallbacks);
      continue;
    }
    if (m.sigma_out[c] != 0u) {
      m.serve_from_record(c, spot, out, k.sigma_out_of_box_fallbacks);
      continue;
    }
    if (!m.spot_in_box(c, spot)) {
      m.serve_from_record(c, spot, out, k.out_of_box_fallbacks);
      continue;
    }
    m.candidates[n_cand++] = static_cast<std::uint32_t>(c);
  }
  const std::size_t budget =
      m.policy.rebuild_on_exit ? std::min<std::size_t>(m.policy.max_rebuilds_per_tick, n_cand) : 0u;
  if (budget < n_cand && budget > 0u &&
      m.policy.rebuild_order == SpotLadderRebuildOrder::NearestAtmFirst) {
    // More candidates than budget (opt-in order): move the `budget` nearest the
    // money (in standard deviations at this spot) to the front. The (key, index)
    // pairs are distinct, so ties break on the contract index and the selected
    // set is deterministic; the order inside it does not matter, because each
    // rebuild writes only its own lane. Keys are finite: K, g, sigma, T > 0 were
    // checked at build and the spot is finite > 0.
    const double ln_s = std::log(spot);
    for (std::size_t i = 0; i < n_cand; ++i) {
      const std::uint32_t c = m.candidates[i];
      m.ranked[i] = {std::abs(m.ln_k_over_g[c] - ln_s) * m.inv_sd[c], c};
    }
    const auto first = m.ranked.begin();
    std::nth_element(first, first + static_cast<std::ptrdiff_t>(budget),
                     first + static_cast<std::ptrdiff_t>(n_cand));
    for (std::size_t i = 0; i < n_cand; ++i) {
      m.candidates[i] = m.ranked[i].second;
    }
  }
  for (std::size_t i = 0; i < n_cand; ++i) {
    const std::size_t c = m.candidates[i];
    if (i >= budget) {
      m.serve_from_record(c, spot, out, k.budget_fallbacks);
      continue;
    }
    ++k.rebuilds;
    if (m.build_one(c, spot)) {
      eval_block(m.blocks[c / kL], spot, o);
      const std::size_t l = c % kL;
      out.price[c] = o.price[l];
      out.delta[c] = o.delta[l];
      out.gamma[c] = o.gamma[l];
      out.vega[c] = o.vega[l];
      out.theta[c] = o.theta[l];
      out.status[c] = static_cast<TickStatus>(o.status[l]);
      continue;
    }
    // The rebuild could not certify a window: build_one refused the contract.
    m.serve_from_record(c, spot, out, k.refused_fallbacks);
  }
  return Ok();
}

std::size_t ChainSpotLadder::size() const noexcept { return impl_ ? impl_->n : 0u; }
std::uint64_t ChainSpotLadder::rebuilds() const noexcept {
  return impl_ ? impl_->counters.rebuilds : 0u;
}
std::uint64_t ChainSpotLadder::fallbacks() const noexcept {
  if (!impl_) {
    return 0u;
  }
  const SpotLadderCounters &k = impl_->counters;
  return k.budget_fallbacks + k.out_of_box_fallbacks + k.sigma_out_of_box_fallbacks +
         k.refused_fallbacks;
}
SpotLadderCounters ChainSpotLadder::counters() const noexcept {
  return impl_ ? impl_->counters : SpotLadderCounters{};
}
std::size_t ChainSpotLadder::sigma_out_of_box_contracts() const noexcept {
  return impl_ ? impl_->sigma_out_count : 0u;
}
std::span<const double> ChainSpotLadder::certified_error_usd() const noexcept {
  return impl_ ? std::span<const double>(impl_->cert_usd) : std::span<const double>{};
}
std::size_t ChainSpotLadder::refused_contracts() const noexcept {
  return impl_ ? impl_->refused_count : 0u;
}
std::size_t ChainSpotLadder::tick_bytes() const noexcept {
  return impl_ ? impl_->blocks.size() * sizeof(Block) : 0u;
}

} // namespace atx::vol

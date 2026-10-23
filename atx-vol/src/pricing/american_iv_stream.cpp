// K2 (r5, 2026-10-22): the recorded Premium inverse request stream and its replay
// (contract in american_iv_stream.hpp).

#include "pricing/american_iv_stream.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <tuple>
#include <vector>

#include "atx/vol/api/pricing/implied_vol.hpp"

namespace atx::vol::iv_batch {

namespace {

namespace sq = sigma_q_family_detail;

std::atomic<InverseStream *> g_stream{nullptr};

[[nodiscard]] InverseStreamFamilyKey key_of(const AlSigmaQFamily &f) noexcept {
  InverseStreamFamilyKey k;
  k.state = sq::QFamilyAccess::state(f);
  k.sigma_lo = f.sigma_lo();
  k.sigma_hi = f.sigma_hi();
  k.q_lo = f.q_lo();
  k.q_hi = f.q_hi();
  k.anchor = f.anchor();
  k.n_sigma = f.n_sigma();
  k.n_q = f.n_q();
  k.side = f.side();
  return k;
}

[[nodiscard]] bool bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] bool same_key(const InverseStreamFamilyKey &a,
                            const InverseStreamFamilyKey &b) noexcept {
  return a.state == b.state && bits(a.sigma_lo, b.sigma_lo) && bits(a.sigma_hi, b.sigma_hi) &&
         bits(a.q_lo, b.q_lo) && bits(a.q_hi, b.q_hi) && bits(a.anchor, b.anchor) &&
         a.n_sigma == b.n_sigma && a.n_q == b.n_q && a.side == b.side;
}

// The family's table index, snapshotting it on first sight (-1: the snapshot failed).
[[nodiscard]] std::int32_t family_index(InverseStream &s, const AlSigmaQFamily &f) {
  const InverseStreamFamilyKey k = key_of(f);
  for (std::size_t i = 0; i < s.keys.size(); ++i) {
    if (same_key(s.keys[i], k)) {
      return s.tables[i] ? static_cast<std::int32_t>(i) : -1;
    }
  }
  auto table = std::make_unique<sq::PremiumTable>();
  if (sq::build_premium_table(f, *table) != sq::FusedStatus::Ok) {
    ++s.table_failures;
    table.reset();
  }
  s.keys.push_back(k);
  s.tables.push_back(std::move(table)); // the unique_ptr moves; the table never does
  const std::size_t i = s.keys.size() - 1;
  return s.tables[i] ? static_cast<std::int32_t>(i) : -1;
}

[[nodiscard]] double seconds_since(std::chrono::steady_clock::time_point t0) noexcept {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// One invert_many call's lanes (built before the timed region).
struct ReplayGroup {
  std::int32_t family = -1;
  double S = kNaN;
  double tol = kNaN;
  std::vector<std::uint32_t> req; // stream ids, lane order
  std::vector<double> K, target, q, seed, T, r;
  std::vector<std::uint8_t> is_call;
  std::vector<std::int32_t> anchor;
  std::vector<double> sigma, lo, hi, plo, phi, res, vega, last;
  std::vector<std::uint16_t> evals;
  std::vector<InverseStatus> status;
  std::vector<std::uint8_t> flags;
  std::vector<LaneAcceptance> acc;
};

} // namespace

void inverse_stream_arm(InverseStream *stream) noexcept {
  g_stream.store(stream, std::memory_order_release);
}

InverseStream *inverse_stream_armed() noexcept {
  return g_stream.load(std::memory_order_acquire);
}

std::uint32_t inverse_stream_record(const InverseStreamRequest &request,
                                    const AlSigmaQFamily *native_family,
                                    std::uint32_t carried_from) noexcept {
  InverseStream *s = inverse_stream_armed();
  if (s == nullptr) {
    return kNoStreamId;
  }
  const std::lock_guard<std::mutex> lock(s->mutex);
  try {
    InverseStreamRequest r = request;
    r.family = native_family != nullptr ? family_index(*s, *native_family) : -1;
    r.anchor = (carried_from != kNoStreamId && carried_from < s->requests.size())
                   ? static_cast<std::int32_t>(carried_from)
                   : -1;
    s->requests.push_back(r);
    return static_cast<std::uint32_t>(s->requests.size() - 1);
  } catch (const std::bad_alloc &) {
    ++s->record_failures;
    return kNoStreamId;
  }
}

void inverse_stream_annotate(std::uint32_t id, InverseStatus status, std::uint16_t evaluations,
                             double sigma) noexcept {
  InverseStream *s = inverse_stream_armed();
  if (s == nullptr || id == kNoStreamId) {
    return;
  }
  const std::lock_guard<std::mutex> lock(s->mutex);
  if (id < s->requests.size()) {
    InverseStreamRequest &r = s->requests[id];
    r.fit_status = status;
    r.fit_evaluations = evaluations;
    r.fit_sigma = sigma;
  }
}

InverseReplay replay_inverse_stream(const InverseStream &stream, InverseIsa isa) {
  InverseReplay out;
  const std::vector<InverseStreamRequest> &reqs = stream.requests;
  out.requests = reqs.size();
  // Untimed: the groups (family, S, tolerance) in arrival order, and the anchors flattened
  // onto anchor-free lanes of the same group.
  std::map<std::tuple<std::int32_t, std::uint64_t, std::uint64_t>, std::size_t> index;
  std::vector<ReplayGroup> groups;
  std::vector<std::int64_t> lane_of(reqs.size(), -1);
  std::vector<std::size_t> group_of(reqs.size(), 0);
  for (std::size_t i = 0; i < reqs.size(); ++i) {
    const InverseStreamRequest &r = reqs[i];
    if (r.family < 0) {
      ++out.without_family;
      continue;
    }
    ++out.with_family;
    const auto key = std::make_tuple(r.family, std::bit_cast<std::uint64_t>(r.S),
                                     std::bit_cast<std::uint64_t>(r.tol));
    auto [it, fresh] = index.try_emplace(key, groups.size());
    if (fresh) {
      ReplayGroup g;
      g.family = r.family;
      g.S = r.S;
      g.tol = r.tol;
      groups.push_back(std::move(g));
    }
    ReplayGroup &g = groups[it->second];
    std::int32_t a = -1;
    if (r.anchor >= 0) {
      std::int32_t root = r.anchor;
      const auto ra = static_cast<std::size_t>(root);
      if (reqs[ra].anchor >= 0) {
        root = reqs[ra].anchor; // a chain of carries: the batch anchors to the first
      }
      const auto rr = static_cast<std::size_t>(root);
      if (lane_of[rr] >= 0 && group_of[rr] == it->second && reqs[rr].anchor < 0 &&
          bits(reqs[rr].K, r.K) && bits(reqs[rr].q, r.q)) {
        a = static_cast<std::int32_t>(lane_of[rr]);
      }
    }
    lane_of[i] = static_cast<std::int64_t>(g.req.size());
    group_of[i] = it->second;
    g.req.push_back(static_cast<std::uint32_t>(i));
    g.K.push_back(r.K);
    g.target.push_back(r.price);
    g.q.push_back(r.q);
    g.T.push_back(r.T);
    g.r.push_back(r.r);
    g.is_call.push_back(r.side == Side::Call ? 1u : 0u);
    g.anchor.push_back(a);
    g.seed.push_back(kNaN);
    if (a >= 0) ++out.anchored;
  }
  std::size_t largest = 0;
  for (ReplayGroup &g : groups) {
    const std::size_t n = g.req.size();
    largest = std::max(largest, n);
    g.sigma.assign(n, kNaN);
    g.lo.assign(n, kNaN);
    g.hi.assign(n, kNaN);
    g.plo.assign(n, kNaN);
    g.phi.assign(n, kNaN);
    g.res.assign(n, kNaN);
    g.vega.assign(n, kNaN);
    g.last.assign(n, kNaN);
    g.evals.assign(n, 0);
    g.status.assign(n, InverseStatus::Inactive);
    g.flags.assign(n, 0);
    g.acc.assign(n, LaneAcceptance{});
  }
  out.groups = groups.size();
  out.max_group = largest;
  InverseScratch scratch;
  scratch.reserve(largest);
  // Timed: the fit's Black-76 seeds of the unanchored lanes and the invert_many calls.
  const auto t0 = std::chrono::steady_clock::now();
  for (ReplayGroup &g : groups) {
    const auto ts = std::chrono::steady_clock::now();
    const std::size_t n = g.req.size();
    for (std::size_t i = 0; i < n; ++i) {
      if (g.anchor[i] >= 0) {
        continue;
      }
      const double F = g.S * std::exp((g.r[i] - g.q[i]) * g.T[i]);
      const Result<double> eu =
          implied_vol(g.target[i], F, g.K[i], g.T[i], std::exp(-g.r[i] * g.T[i]),
                      g.is_call[i] != 0u ? Side::Call : Side::Put);
      ++out.seeds;
      if (eu && std::isfinite(*eu)) {
        g.seed[i] = *eu;
      } else {
        ++out.seed_failures;
      }
    }
    out.seed_seconds += seconds_since(ts);
    InverseBatchIn in;
    in.table = stream.tables[static_cast<std::size_t>(g.family)].get();
    in.S = g.S;
    in.tol = g.tol;
    in.isa = isa;
    in.K = g.K;
    in.target = g.target;
    in.q = g.q;
    in.is_call = g.is_call;
    in.seed_sigma = g.seed;
    in.anchor = g.anchor;
    const InverseBatchOut o{g.sigma, g.lo,   g.hi,     g.plo,    g.phi,   g.res,
                            g.vega,  g.last, g.evals, g.status, g.flags, g.acc};
    const auto tb = std::chrono::steady_clock::now();
    static_cast<void>(invert_many(in, o, scratch, &out.work));
    out.batch_seconds += seconds_since(tb);
  }
  out.total_seconds = seconds_since(t0);
  // Untimed: agreement with the fit's own native attempts.
  for (const ReplayGroup &g : groups) {
    for (std::size_t i = 0; i < g.req.size(); ++i) {
      const InverseStreamRequest &r = reqs[g.req[i]];
      ++out.fit_by_status[static_cast<std::size_t>(r.fit_status)];
      if (r.fit_status == InverseStatus::Inactive) {
        continue;
      }
      ++out.fit_compared;
      if (r.fit_status == g.status[i]) ++out.fit_status_equal;
      if (r.fit_status == InverseStatus::Converged && g.status[i] == InverseStatus::Converged) {
        ++out.fit_both_converged;
        out.fit_max_dsigma = std::max(out.fit_max_dsigma, std::fabs(g.sigma[i] - r.fit_sigma));
      }
    }
  }
  return out;
}

} // namespace atx::vol::iv_batch

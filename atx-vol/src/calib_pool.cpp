// Multi-underlier calibration pool + refit-cadence scheduler.
//
// Port of ats_calibrate_pool.c. See calib_pool.hpp for the design + PORT NOTES.

#include "atx/vol/calib_pool.hpp"

#include <algorithm>  // std::sort, std::min, std::max
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <thread>     // std::jthread, std::thread::hardware_concurrency
#include <utility>    // std::move, std::swap

#include "atx/vol/essvi_calib.hpp"  // essvi_calib_surface
#include "atx/vol/profile.hpp"      // classify_underlier, profile_lookup
#include "atx/vol/svi_calib.hpp"    // svi_calib_surface, svi_mm_calib_surface

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

// ── CadenceQueue: binary min-heap on (due, tier, uid) ─────────────────────

bool CadenceQueue::entry_before(const Entry& a, const Entry& b) noexcept {
  if (a.next_refit_ts_ns != b.next_refit_ts_ns) {
    return a.next_refit_ts_ns < b.next_refit_ts_ns;
  }
  if (a.tier_priority != b.tier_priority) {
    return a.tier_priority < b.tier_priority;
  }
  return a.uid < b.uid;
}

void CadenceQueue::sift_up(std::size_t i) noexcept {
  while (i > 0u) {
    const std::size_t parent = (i - 1u) / 2u;
    if (!entry_before(entries_[i], entries_[parent])) {
      break;
    }
    std::swap(entries_[i], entries_[parent]);
    i = parent;
  }
}

void CadenceQueue::sift_down(std::size_t i) noexcept {
  const std::size_t n = entries_.size();
  for (;;) {
    const std::size_t l = 2u * i + 1u;
    const std::size_t r = 2u * i + 2u;
    std::size_t best = i;
    if (l < n && entry_before(entries_[l], entries_[best])) {
      best = l;
    }
    if (r < n && entry_before(entries_[r], entries_[best])) {
      best = r;
    }
    if (best == i) {
      break;
    }
    std::swap(entries_[i], entries_[best]);
    i = best;
  }
}

void CadenceQueue::push(Uid uid, std::uint16_t tier_priority,
                        std::int64_t initial_due_ns) {
  Entry e{};
  e.uid = uid;
  e.tier_priority = tier_priority;
  e.next_refit_ts_ns = initial_due_ns;
  e.last_refit_ts_ns = 0;
  entries_.push_back(e);
  sift_up(entries_.size() - 1u);
}

const CadenceQueue::Entry& CadenceQueue::peek() const noexcept {
  assert(!entries_.empty());
  return entries_.front();
}

CadenceQueue::Entry CadenceQueue::pop() noexcept {
  assert(!entries_.empty());
  const Entry top = entries_.front();
  entries_.front() = entries_.back();
  entries_.pop_back();
  if (!entries_.empty()) {
    sift_down(0u);
  }
  return top;
}

void CadenceQueue::reschedule_top(std::int64_t last_refit_ts_ns,
                                  std::int64_t next_refit_ts_ns) noexcept {
  assert(!entries_.empty());
  entries_.front().last_refit_ts_ns = last_refit_ts_ns;
  entries_.front().next_refit_ts_ns = next_refit_ts_ns;
  sift_down(0u);
}

std::size_t CadenceQueue::pop_due(std::int64_t now_ns, std::span<Uid> out_uids,
                                  std::span<const std::int64_t> uid_cadence_ns) noexcept {
  const std::size_t cap_out = out_uids.size();
  std::size_t n_out = 0u;
  // Bounded by cap_out (JPL Rule 2): each iteration writes one output slot.
  while (!entries_.empty() && n_out < cap_out) {
    const Entry& top = entries_.front();
    if (top.next_refit_ts_ns > now_ns) {
      break;  // earliest-due entry is in the future => nothing due
    }
    const Uid uid = top.uid;
    out_uids[n_out] = uid;
    ++n_out;

    const std::size_t cad_idx = static_cast<std::size_t>(uid);
    const std::int64_t cadence =
        (!uid_cadence_ns.empty() && cad_idx < uid_cadence_ns.size())
            ? uid_cadence_ns[cad_idx]
            : kCadenceDefaultNs;
    reschedule_top(now_ns, now_ns + cadence);
  }
  return n_out;
}

// ── Fan-out rebuild driver ────────────────────────────────────────────────

namespace {

// (tier_priority << 48) | hash(uid): the C `pack_priority_uid` sharding key.
// The hash spreads uids within a tier across worker chunks for tail balance;
// it does NOT affect the (uid-sorted) result, only the dispatch order.
[[nodiscard]] std::uint32_t mix_uid(Uid uid) noexcept {
  std::uint32_t h = uid;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

[[nodiscard]] std::uint64_t pack_priority_uid(std::uint16_t tier, Uid uid) noexcept {
  return (static_cast<std::uint64_t>(tier) << 48) |
         static_cast<std::uint64_t>(mix_uid(uid));
}

// Resolved, thread-safe-to-read plan for one uid (built single-threaded).
struct UidPlan {
  Uid uid{kInvalidUid};
  const Underlying* under{nullptr};  // stable deque pointer (read-only)
  const CurveSet* curves{nullptr};   // from the provider (read-only)
  ProfileKind kind{ProfileKind::OrdinarySingleName};
  std::uint8_t tier{3u};
  Parametrization param{Parametrization::Essvi};
  bool fittable{false};
  std::uint64_t sort_key{0u};
};

[[nodiscard]] bool param_supported(Parametrization p) noexcept {
  return p == Parametrization::Essvi || p == Parametrization::Svi ||
         p == Parametrization::SviMm;
}

// Dispatch to the calibrator selected by the profile's base surface.
[[nodiscard]] Status dispatch_calib(Parametrization param, VolSurface& surface,
                                    const Underlying& under, const CurveSet& curves,
                                    const CalibOpts& opts, FitDiag* diag) {
  switch (param) {
    case Parametrization::Essvi:
      return essvi_calib_surface(surface, under, curves, opts, diag);
    case Parametrization::Svi:
      return svi_calib_surface(surface, under, curves, opts, diag);
    case Parametrization::SviMm:
      return svi_mm_calib_surface(surface, under, curves, opts, diag);
    case Parametrization::Wing:
    case Parametrization::C8:
    case Parametrization::CStar16M:
      // Filtered out upstream (param_supported); kept for switch exhaustiveness.
      return Err(ErrorCode::NotImplemented,
                 "calibrate_pool: no calibrator for this parametrization");
  }
  return Err(ErrorCode::Internal, "calibrate_pool: unreachable parametrization");
}

// Fit one underlier. Pure w.r.t. shared state: reads `p` (const) + `opts`
// (const), constructs its own `VolSurface`/`FitDiag`. Safe to run on any
// worker thread concurrently with other calls on disjoint plans.
[[nodiscard]] PoolEntry fit_one(const UidPlan& p, const CalibOpts& opts) {
  PoolEntry e{};
  e.uid = p.uid;
  e.profile_kind = p.kind;
  e.tier_priority = p.tier;
  e.param = p.param;

  if (!p.fittable) {
    e.status = FitStatus::Skipped;
    return e;
  }

  // fittable guarantees non-null pointers and a supported parametrization.
  auto surf_res = VolSurface::create(p.uid, p.param, p.under->chains.size());
  if (!surf_res) {
    e.status = FitStatus::Failed;
    e.error_code = surf_res.error().code();
    return e;
  }
  VolSurface surface = std::move(*surf_res);

  FitDiag diag{};
  try {
    const Status st = dispatch_calib(p.param, surface, *p.under, *p.curves, opts, &diag);
    if (st) {
      e.status = FitStatus::Ok;
      e.diag = diag;
      e.surface = std::move(surface);
    } else {
      e.status = FitStatus::Failed;
      e.error_code = st.error().code();
    }
  } catch (...) {
    // SAFETY: a std::jthread worker must not let an exception escape (e.g.
    // std::bad_alloc from a calibrator's scratch vectors) — that would
    // std::terminate the process. Record it as a Failed fit instead.
    e.status = FitStatus::Failed;
    e.error_code = ErrorCode::Internal;
  }
  return e;
}

}  // namespace

Result<PoolResult> calibrate_pool(Universe& universe, const CurveProvider& curves_for,
                                  const CalibOpts& opts, unsigned n_threads) {
  if (!curves_for) {
    return Err(ErrorCode::InvalidArgument, "calibrate_pool: null curve provider");
  }

  // ── Single-threaded pre-pass: classify + resolve pointers + sharding key ──
  const Universe& cu = universe;  // force the const (read-only) get_underlying
  const std::uint32_t n_uids = universe.n_underlyings();

  std::vector<UidPlan> plans;
  plans.reserve(n_uids);
  for (std::uint32_t uid = 1u; uid <= n_uids; ++uid) {
    UidPlan p{};
    p.uid = uid;

    auto under_res = cu.get_underlying(uid);
    if (under_res) {
      p.under = *under_res;
      const ProfileVerdict verdict = classify_underlier(*p.under);
      p.kind = verdict.kind;
      p.tier = profile_tier_priority(verdict.kind);
      if (auto prof = profile_lookup(verdict.kind); prof) {
        p.param = (*prof)->base_surface;
      }
      p.curves = curves_for(uid);
      p.fittable = !p.under->chains.empty() && p.curves != nullptr &&
                   param_supported(p.param);
    }

    p.sort_key = pack_priority_uid(static_cast<std::uint16_t>(p.tier), uid);
    plans.push_back(p);
  }

  PoolResult out{};
  const std::size_t n = plans.size();
  if (n == 0u) {
    return Ok(std::move(out));  // empty universe: zeroed result
  }

  // Tier-priority shard: ULTRA_LIQUID (tier 0) dispatched before ILLIQUID
  // (tier 3); hash spreads a tier across chunks; uid breaks any residual tie.
  std::sort(plans.begin(), plans.end(),
            [](const UidPlan& a, const UidPlan& b) noexcept {
              if (a.sort_key != b.sort_key) {
                return a.sort_key < b.sort_key;
              }
              return a.uid < b.uid;
            });

  // ── Fan out across workers; each writes its own disjoint slots ────────────
  std::size_t n_workers = (n_threads != 0u)
                              ? static_cast<std::size_t>(n_threads)
                              : std::max<std::size_t>(
                                    1u, std::thread::hardware_concurrency());
  n_workers = std::min(n_workers, n);  // n >= 1 here

  std::vector<PoolEntry> slots(n);
  const auto run_range = [&plans, &slots, &opts](std::size_t start,
                                                 std::size_t end) {
    for (std::size_t i = start; i < end; ++i) {
      slots[i] = fit_one(plans[i], opts);
    }
  };

  {
    std::vector<std::jthread> workers;
    workers.reserve(n_workers - 1u);
    const std::size_t base = n / n_workers;
    const std::size_t rem = n % n_workers;
    std::size_t pos = 0u;
    for (std::size_t w = 0u; w < n_workers; ++w) {
      const std::size_t sz = base + (w < rem ? std::size_t{1u} : std::size_t{0u});
      const std::size_t start = pos;
      const std::size_t end = pos + sz;
      pos = end;
      if (w + 1u < n_workers) {
        workers.emplace_back([&run_range, start, end] { run_range(start, end); });
      } else {
        run_range(start, end);  // last chunk on the calling thread
      }
    }
    // workers join here (jthread RAII) before we read `slots`.
  }

  // ── Aggregate deterministically (sorted by uid) ───────────────────────────
  std::sort(slots.begin(), slots.end(),
            [](const PoolEntry& a, const PoolEntry& b) noexcept {
              return a.uid < b.uid;
            });
  for (const PoolEntry& e : slots) {
    switch (e.status) {
      case FitStatus::Ok:
        ++out.n_fit_ok;
        break;
      case FitStatus::Failed:
        ++out.n_fit_failed;
        break;
      case FitStatus::Skipped:
        ++out.n_skipped;
        break;
    }
  }
  out.n_attempted = static_cast<std::uint32_t>(slots.size());
  out.entries = std::move(slots);
  return Ok(std::move(out));
}

Result<PoolResult> calibrate_pool(Universe& universe, const CurveSet& shared_curves,
                                  const CalibOpts& opts, unsigned n_threads) {
  const CurveProvider provider = [&shared_curves](Uid) noexcept {
    return &shared_curves;
  };
  return calibrate_pool(universe, provider, opts, n_threads);
}

}  // namespace atx::vol

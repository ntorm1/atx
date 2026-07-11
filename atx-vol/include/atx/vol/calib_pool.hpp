#pragma once

// Multi-underlier calibration pool + refit-cadence scheduler — the parallel
// universe-rebuild driver for atx-vol.
//
// Ported from the C `ats-vol` library (ats_calibrate_pool.h / .c):
//   - `ats_vol_universe_rebuild`  -> `calibrate_pool` (the profile-tier-sharded
//     fan-out driver that fits every fittable underlier in the universe);
//   - `AtsVolCadenceQueue` + `ats_vol_cadence_queue_{init,register,pop_due}`
//     -> `class CadenceQueue` (the steady-state refit scheduler — a min-heap
//     keyed on the next-due timestamp that drives many rebuild waves across
//     time, each wave honouring the per-profile cadence).
//
// The refactor to the atx house style (.agents/cpp/agent.md) drops the C's
// arena + hand-rolled worker-slot pool + negative-integer `AtsVolStatus`
// channel and replaces them with `std::vector`/`std::jthread`, per-thread
// locals, and `Result<T>` / `Status`.
//
// ## What is reused (nothing here is redefined)
//   - Calibrators : `essvi_calib_surface` / `svi_calib_surface` /
//                   `svi_mm_calib_surface`         (essvi_calib.hpp, svi_calib.hpp)
//   - Classifier  : `classify_underlier` + `profile_lookup` +
//                   `profile_tier_priority`        (profile.hpp)
//   - Surface     : `VolSurface` / `Parametrization` (vol_surface.hpp)
//   - Universe    : `Universe` / `Underlying`      (universe.hpp)
//   - Curves      : `CurveSet`                     (curve.hpp)
//   - Options     : `CalibOpts` / `FitDiag`        (calib.hpp)
//   - Errors      : `Result` / `Status` / `ErrorCode` (atx-core, via types.hpp)
//
// ## Concurrency
//
// atx-core/concurrent ships only lock-free queues (no thread pool / parallel-
// for), so the fan-out uses `std::jthread`. The driver is **deterministic by
// construction**, independent of the thread count:
//   - all Universe / curve-provider access happens in a single-threaded
//     pre-pass that resolves a stable `const Underlying*` + `const CurveSet*`
//     per uid (deque storage keeps those pointers valid for the call);
//   - each worker owns its own scratch (`VolSurface`, `FitDiag`) — no shared
//     mutable calibrator state, mirroring the C's per-worker ctx;
//   - workers write disjoint, pre-sized result slots (never the same index),
//     so there is no data race on the output;
//   - the calibrators + classifier are pure reads of their (const) inputs;
//   - results are aggregated sorted by uid, so the observable output is
//     identical across runs and across worker counts.
//
// ── PORT NOTES ─────────────────────────────────────────────────────────────
//  - The C `AtsVolCalibPool` worker-slot pool (arena-backed `AtsVolCalibCtx`
//    per worker) is NOT ported: atx-vol's calibrators are stateless free
//    functions, so each `std::jthread` simply constructs its own locals. No
//    create/destroy handle is needed.
//  - Per-uid curves: the C read `under->curves`; atx-vol's `Underlying`
//    deliberately omits the curve link (universe.hpp), so `calibrate_pool`
//    takes a `CurveProvider` (or one shared `CurveSet`). The provider is only
//    ever invoked in the single-threaded pre-pass.
//  - Fitted surfaces: the C wrote `under->surface`; atx-vol's `Underlying` has
//    no surface field, so each fit's `VolSurface` is returned inside its
//    `PoolEntry`.
//  - Calibration options are applied uniformly to every underlier (the C's
//    `opts_override` branch). The per-profile `prof->calib` path is reachable
//    via `profile_lookup` if a caller wants it, but is not the pool's default.
//  - Wall-clock timing (`wall_ns` / `wall_ns_per_tier`) is dropped — it is
//    non-deterministic and the driver's contract here is determinism.
//  - Cadence ordering: the C `AtsVolCadenceQueue` heap is keyed solely on
//    `next_refit_ts_ns`; `tier_priority` is stored per entry but not used in
//    the comparator. `CadenceQueue` keeps due-time as the PRIMARY key (the C
//    behaviour the cadence tests depend on — `pop_due` stops at the first
//    not-yet-due entry, which requires the earliest-due entry on top) and adds
//    `tier_priority` then `uid` as deterministic tie-breakers among equal due
//    times. Ordering by tier first would break the C "pop-all-due" contract.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/calib.hpp"         // CalibOpts, FitDiag
#include "atx/vol/curve.hpp"         // CurveSet
#include "atx/vol/profile.hpp"       // ProfileKind, profile_tier_priority
#include "atx/vol/types.hpp"         // Result, Status, ErrorCode
#include "atx/vol/universe.hpp"      // Universe, Underlying, Uid
#include "atx/vol/vol_surface.hpp"   // VolSurface, Parametrization

namespace atx::vol {

// De-Americanization options bundle (defined in deamer.hpp). Forward-declared
// so the pool driver can take an opt-in `const DeAmOptions*` without pulling the
// de-Am pipeline header into every consumer of this one.
struct DeAmOptions;

// ── Profile-cadence scheduler ────────────────────────────────────────────
//
// Steady-state refit primitive (ports `AtsVolCadenceQueue`). A binary min-heap
// over `Entry`, keyed on `next_refit_ts_ns` (then `tier_priority`, then `uid`
// as tie-breakers — see the PORT NOTE above). `pop_due` pulls every entry due
// at/ before `now_ns`, advancing each by its per-uid cadence so a hot profile
// (e.g. an ULTRA_LIQUID name at 250 ms) never starves under a backlog of cold
// names (e.g. ILLIQUID at 5 s).
//
// Thread-safety: NOT internally synchronized — a single owner drives it (the
// scheduler thread). Concurrent mutation is a data race; fence externally.
class CadenceQueue {
 public:
  // Per-uid cadence state (ports `AtsVolCadenceEntry`). Trivially copyable;
  // every member initialized.
  struct Entry {
    Uid uid{kInvalidUid};
    std::uint16_t tier_priority{0u};    // lower = higher priority (tie-break)
    std::int64_t next_refit_ts_ns{0};   // heap key: earliest-due wins
    std::int64_t last_refit_ts_ns{0};   // stamped on each pop
  };

  // Default per-uid cadence used by `pop_due` when no cadence is supplied for a
  // uid (ports the C's 1 s NULL-array fallback).
  static constexpr std::int64_t kCadenceDefaultNs = 1'000'000'000;

  CadenceQueue() = default;

  // Reserve capacity for `cap_hint` entries (the C's fixed `cap`; here it is a
  // hint only — the heap grows on demand, so `push` never fails on a full
  // queue). Rule of Zero otherwise.
  explicit CadenceQueue(std::size_t cap_hint) { entries_.reserve(cap_hint); }

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  // Register `uid` due at `initial_due_ns` with tier `tier_priority` (ports
  // `ats_vol_cadence_queue_register`; `last_refit_ts_ns` seeds to 0).
  void push(Uid uid, std::uint16_t tier_priority, std::int64_t initial_due_ns);

  // The earliest-due entry (heap root). Precondition: `!empty()`.
  [[nodiscard]] const Entry& peek() const noexcept;

  // Remove and return the earliest-due entry. Precondition: `!empty()`.
  [[nodiscard]] Entry pop() noexcept;

  // Advance the current root's schedule in place and re-heapify (ports the
  // in-heap reschedule inside the C `pop_due`). Precondition: `!empty()`.
  void reschedule_top(std::int64_t last_refit_ts_ns,
                      std::int64_t next_refit_ts_ns) noexcept;

  // Pop every entry whose `next_refit_ts_ns <= now_ns` into `out_uids`, each
  // rescheduled to `now_ns + cadence` where cadence is `uid_cadence_ns[uid]`
  // (or `kCadenceDefaultNs` when the span is empty or `uid` is out of range).
  // Capacity-bounded to `out_uids.size()` per call; remaining due work stays on
  // the heap for the next call. Ports `ats_vol_cadence_queue_pop_due`.
  //
  // @return the number of uids written to `out_uids`.
  [[nodiscard]] std::size_t pop_due(std::int64_t now_ns, std::span<Uid> out_uids,
                                    std::span<const std::int64_t> uid_cadence_ns) noexcept;

 private:
  // Strict-weak "a should sit closer to the root than b" (min on the heap key).
  [[nodiscard]] static bool entry_before(const Entry& a, const Entry& b) noexcept;
  void sift_up(std::size_t i) noexcept;
  void sift_down(std::size_t i) noexcept;

  std::vector<Entry> entries_{};  // binary min-heap on the (due, tier, uid) key
};

// ── Fan-out rebuild driver ───────────────────────────────────────────────

// Outcome of one underlier's fit within a pool rebuild.
enum class FitStatus : std::uint8_t {
  Ok = 0,       // the calibrator returned a fitted surface
  Failed = 1,   // the calibrator returned an error (see `error_code`)
  Skipped = 2,  // no chains / no curves / no applicable calibrator
};

// Per-underlier rebuild record (ports the meaningful fields of the C's
// `AtsVolCalibResult` slot, plus the classification the C left implicit).
// Aggregate value type; Rule of Zero.
struct PoolEntry {
  Uid uid{kInvalidUid};
  ProfileKind profile_kind{ProfileKind::OrdinarySingleName};
  std::uint8_t tier_priority{3u};                    // profile_tier_priority(kind)
  Parametrization param{Parametrization::Essvi};     // profile.base_surface
  FitStatus status{FitStatus::Skipped};
  ErrorCode error_code{ErrorCode::Unknown};          // meaningful iff status==Failed
  FitDiag diag{};                                    // meaningful iff status==Ok
  std::optional<VolSurface> surface{};               // present iff status==Ok
};

// Aggregate result of a pool rebuild (ports `AtsVolUniverseRebuildStats` +
// the per-uid `out_results`). `entries` is sorted ascending by `uid`, so the
// output is deterministic across runs and worker counts.
struct PoolResult {
  std::vector<PoolEntry> entries{};
  std::uint32_t n_attempted{0u};
  std::uint32_t n_fit_ok{0u};
  std::uint32_t n_fit_failed{0u};
  std::uint32_t n_skipped{0u};
};

// Per-uid curve resolver: returns the `CurveSet*` to price `uid` against, or
// nullptr if the uid has no curves (that uid is then Skipped). Invoked only in
// the single-threaded pre-pass, so it need not be thread-safe.
using CurveProvider = std::function<const CurveSet*(Uid)>;

// Fit every registered underlier (uids 1..n_underlyings) of `universe`:
// classify each -> pick the calibrator by the profile's `base_surface`
// (Essvi/Svi/SviMm) -> fit a fresh `VolSurface` -> collect the per-uid status +
// diagnostics + surface. Underliers are tier-priority-sharded (ULTRA_LIQUID
// dispatched before ILLIQUID) then fanned out across `n_threads` workers; the
// aggregated result is order-independent (sorted by uid). Ports
// `ats_vol_universe_rebuild`.
//
// @param universe   the registry to rebuild (read-only during the call).
// @param curves_for per-uid curve resolver (see `CurveProvider`).
// @param opts       calibration options applied to every underlier.
// @param n_threads  worker count; 0 => `std::thread::hardware_concurrency()`
//                   (>= 1). Clamped to the underlier count.
// @param deam  OPT-IN de-Americanization route, forwarded to the eSSVI
//              calibrator ONLY (see `essvi_calib_surface`'s `deam`): null
//              (default) is today's raw Black-76 path, byte-identical. When
//              non-null, every eSSVI-parametrized name de-Americanizes its mids
//              before fitting, fixing the silent American-IV bias. The SVI /
//              SviMm calibrators do not yet honor it (follow-up); a name routed
//              to them fits raw regardless of `deam`.
// @return InvalidArgument if `curves_for` is empty; otherwise Ok(result). An
//         empty universe yields Ok with a zeroed result.
[[nodiscard]] Result<PoolResult> calibrate_pool(Universe& universe,
                                                const CurveProvider& curves_for,
                                                const CalibOpts& opts,
                                                unsigned n_threads = 0u,
                                                const DeAmOptions* deam = nullptr);

// Convenience overload: price every underlier against one shared `CurveSet`
// (its lifetime must enclose the call).
[[nodiscard]] Result<PoolResult> calibrate_pool(Universe& universe,
                                                const CurveSet& shared_curves,
                                                const CalibOpts& opts,
                                                unsigned n_threads = 0u,
                                                const DeAmOptions* deam = nullptr);

}  // namespace atx::vol

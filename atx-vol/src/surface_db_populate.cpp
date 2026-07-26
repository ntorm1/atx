#include "atx/vol/surface_db_populate.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator> // make_move_iterator (FIX-E disabled-carry append)
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/detail/archive_util.hpp"  // canonicalize_symbol (carry-over key match)
#include "atx/vol/detail/fit_scheduler.hpp" // run_bounded_fit_tasks
#include "atx/vol/dispersion.hpp"           // with_uid
#include "atx/vol/pricer_fitter.hpp"        // PricerConfig
#include "atx/vol/session.hpp"              // SessionInputs
#include "atx/vol/universe.hpp"             // uid_for_symbol
#include "corpus_board_fit.hpp"             // FitSlot, fit_board (shared blessed fit path)
#include "surface_db_seed.hpp"              // seed_symbol_config (shared index/preset seed recipe)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Translate the PricerConfig-representable subset of a SymbolFitConfig
// (product policy, preset, curve-when-pinned, and the four optional<bool>
// knobs) so the
// fallback-ladder's "a caller-pinned curve is never silently substituted"
// invariant (pricer_fitter.cpp) holds for a symbol-config pin exactly the
// way it already holds for CorpusBoard::curve. The fields PricerConfig
// cannot carry (band_k, al_override/al, calendar_repair, and calib-mirror-
// when-pinned) are layered separately via `fit_board`'s `session_overlay`
// hook, which calls `apply_symbol_config` on the fitter's actual
// SessionInputs — see corpus_board_fit.hpp.
[[nodiscard]] PricerConfig pricer_config_for_symbol(const SymbolFitConfig &cfg) {
  PricerConfig out;
  out.preset = cfg.preset;
  out.quality_mode = cfg.surface_policy.quality_mode;
  out.outputs = cfg.surface_policy.outputs;
  out.risk_admission = cfg.surface_policy.risk_admission;
  out.fallback = cfg.surface_policy.fallback;
  if (cfg.pin_curve) {
    out.curve = cfg.curve;
  }
  out.use_correction_cache = cfg.use_correction_cache;
  out.score_parity = cfg.score_parity;
  out.enforce_calendar_floor = cfg.enforce_calendar_floor;
  out.use_deam_cache_for_fit = cfg.use_deam_cache_for_fit;
  return out;
}

// The ONE place a board's effective fit config is resolved: the symbol's
// manifest entry when present, else the caller's fallback. Both the population
// loop's disabled-skip and the cell-aware resume filter's disabled-exclusion go
// through this, so the two cannot drift out of agreement about which cells will
// actually be written.
[[nodiscard]] SymbolFitConfig resolve_symbol_config(SurfaceDb &db, const std::string &symbol,
                                                    const SymbolFitConfig &fallback) {
  const Result<SymbolFitConfig> found = db.symbol_config(symbol);
  return found.has_value() ? *found : fallback;
}

struct SymbolAccum {
  PopulateSymbolStats stats;
  double oos_sum{0.0};
  std::uint32_t oos_count{0};
};

struct DateRange {
  std::size_t begin{0u};
  std::size_t end{0u};
  bool skip{false};
};

// ── R1-a (review C-06): the drain's "the scheduler is gone" sentinel ─────────
//
// `remaining[r]` normally holds the number of enabled fits still in flight for
// date-range r, and 0 means "this date is complete, drain it". This third value
// means "the fit scheduler TERMINATED while this date still had fits outstanding
// — they will never run and nothing will ever decrement this counter again".
//
// It is a value of `remaining[r]` itself, not a separate flag, and that is the
// whole point. The drain sleeps in `remaining[r].wait(left)`, which wakes on a
// change to THAT object; a separate `std::atomic<bool> scheduler_finished` could
// be set and notified in the window between the drain's last check of it and its
// entry into `wait`, and the drain would then sleep forever on a counter nobody
// will touch again — the classic lost wakeup, reintroduced by the fix. Folding
// termination into the waited-on object closes that window by construction: see
// the drain loop for the full argument.
//
// SIZE_MAX is safe as the sentinel because the counter counts boards, which are
// bounded by `boards.size()`, and because it is only ever STORED after
// `run_bounded_fit_tasks` has returned — which joins every worker it started, so
// no `MarkDone` destructor can be racing a `fetch_sub` against it.
constexpr std::size_t kSchedulerAborted = std::numeric_limits<std::size_t>::max();

// ── Stats CSV formatting (mirrors run_report.cpp's meta+header+rows shape;
// that helper is TU-private there, so this is an independent small twin, not
// a shared function) ─────────────────────────────────────────────────────

[[nodiscard]] std::string fmt10(double v) {
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.10g", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

[[nodiscard]] std::string fmt_nan_aware10(double v) {
  return std::isnan(v) ? std::string("nan") : fmt10(v);
}

[[nodiscard]] std::string fmt_u32(std::uint32_t v) {
  char buf[16];
  const int len = std::snprintf(buf, sizeof buf, "%u", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

[[nodiscard]] Status write_meta_body(const MetaKv &meta, const std::string &body,
                                     std::string_view path, const char *who) {
  std::ofstream os(std::string(path), std::ios::binary | std::ios::trunc);
  if (!os) {
    return Err(ErrorCode::IoError, std::string(who) + ": cannot open file");
  }
  std::string out;
  out.reserve(body.size() + meta.size() * 32 + 16);
  for (const auto &[k, v] : meta) {
    out += "# ";
    out += k;
    out += '=';
    out += v;
    out += '\n';
  }
  out += body;
  os.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!os) {
    return Err(ErrorCode::IoError, std::string(who) + ": write failed");
  }
  return Ok();
}

} // namespace

Result<SurfaceDbPopulateStats> populate_surface_db(SurfaceDb &db,
                                                   std::span<const CorpusBoard> boards,
                                                   const SurfaceDbPopulateConfig &cfg,
                                                   const PopulateTestHooks *test_hooks) {
  if (boards.empty()) {
    return Err(ErrorCode::InvalidArgument, "populate_surface_db: empty boards");
  }
  const std::size_t n = boards.size();

  // Deterministic order: date asc, symbol asc, then stable original index.
  std::vector<std::size_t> order(n);
  for (std::size_t idx = 0; idx < n; ++idx) {
    order[idx] = idx;
  }
  std::sort(order.begin(), order.end(), [&boards](std::size_t a, std::size_t b) noexcept {
    if (boards[a].date != boards[b].date) {
      return boards[a].date < boards[b].date;
    }
    if (boards[a].symbol != boards[b].symbol) {
      return boards[a].symbol < boards[b].symbol;
    }
    return a < b;
  });

  SurfaceDbPopulateStats stats;
  stats.n_boards = static_cast<std::uint32_t>(n);
  std::map<std::string, SymbolAccum> per_symbol; // ordered by symbol -> free sort

  // Resolve resumability before launching any fit. Writes remain date-ordered
  // and serial, while every non-skipped board shares one dynamic queue.
  std::vector<DateRange> date_ranges;
  date_ranges.reserve(n);
  std::size_t i = 0u;
  while (i < n) {
    const std::string &date = boards[order[i]].date;
    std::size_t j = i + 1u;
    while (j < n && boards[order[j]].date == date) {
      ++j;
    }

    bool skip = false;
    if (cfg.skip_existing) {
      const Result<SurfaceArchiveV2> existing = db.open_partition(date);
      if (existing.has_value()) {
        ++stats.n_dates_skipped_existing;
        skip = true;
      } else if (existing.error().code() != ErrorCode::NotFound) {
        return Err(existing.error());
      }
    }
    date_ranges.push_back(DateRange{i, j, skip});
    i = j;
  }

  std::vector<SymbolFitConfig> resolved_cfgs(n);
  std::vector<FitSlot> slots(n);

  // FIX-D: which positions are CARRIED (re-emitted from the existing partition)
  // rather than fitted. Resolved once, up front, so the fit scheduling below can
  // simply skip them -- a carried cell never reaches fit_board.
  std::vector<bool> carried(n, false);
  if (!cfg.carry_over.empty()) {
    for (std::size_t pos = 0; pos < n; ++pos) {
      const CorpusBoard &board = boards[order[pos]];
      const auto it = cfg.carry_over.find(board.date);
      if (it == cfg.carry_over.end()) {
        continue;
      }
      const std::string canon = detail::canonicalize_symbol(board.symbol, kSurfaceDbKeyMax);
      carried[pos] = std::find(it->second.begin(), it->second.end(), canon) != it->second.end();
    }
  }

  // Enabled boards to fit, in deterministic (date asc, symbol asc) order, each
  // tagged with its date-range index. `remaining[r]` counts the enabled fits
  // still in flight for range r; a worker decrements it as each board finishes
  // and the drain thread below wakes on the range that reaches zero.
  std::vector<std::size_t> fit_positions;
  std::vector<std::size_t> fit_task_range; // parallel to fit_positions
  fit_positions.reserve(n);
  fit_task_range.reserve(n);
  std::vector<std::atomic<std::size_t>> remaining(date_ranges.size()); // value-init to 0
  for (std::size_t r = 0; r < date_ranges.size(); ++r) {
    const DateRange &range = date_ranges[r];
    if (range.skip) {
      continue;
    }
    std::size_t enabled_in_range = 0u;
    for (std::size_t pos = range.begin; pos < range.end; ++pos) {
      const CorpusBoard &board = boards[order[pos]];
      resolved_cfgs[pos] = resolve_symbol_config(db, board.symbol, cfg.fallback);
      // A carried cell is not fit work: it must not be queued and must not be
      // counted into this range's in-flight total, or the drain would wait on a
      // decrement that never comes.
      if (resolved_cfgs[pos].enabled && !carried[pos]) {
        fit_positions.push_back(pos);
        fit_task_range.push_back(r);
        ++enabled_in_range;
      }
    }
    remaining[r].store(enabled_in_range, std::memory_order_relaxed);
  }

  // ── U2 (R-13) [pure-refactor]: Longest-Processing-Time claim order ──────────
  // Claim the largest boards first (descending frame rows) so the shared bounded
  // queue starts the heavy SPY-scale tail immediately instead of stranding it
  // behind many small boards near the end -- that tail otherwise sets the
  // makespan and leaves cores idle once the small work drains. This reorders
  // *scheduling* only: `fit_task` writes `slots[pos]` keyed by board, every
  // fit_board is independent and deterministic, and the drain still visits
  // dates/boards in date/symbol order -- so all surfaces and stats stay
  // byte-identical (only which worker claims which board, and when, changes).
  // `remaining[r]` is a per-range count untouched by the reorder, so the
  // per-date drain is unaffected. `stable_sort` preserves the prior (date asc,
  // symbol asc) order for equal-row ties, keeping the claim order deterministic
  // (tie-break by original position). Frame rows is a cheap, monotone proxy for
  // fit cost (quotes to price); an exact cost model is unnecessary for LPT to
  // dominate the naive order on a skewed board-size distribution.
  {
    const std::size_t k = fit_positions.size();
    std::vector<std::size_t> claim_perm(k);
    for (std::size_t t = 0; t < k; ++t) {
      claim_perm[t] = t;
    }
    std::stable_sort(claim_perm.begin(), claim_perm.end(),
                     [&](std::size_t a, std::size_t b) noexcept {
                       return boards[order[fit_positions[a]]].frame.rows.size() >
                              boards[order[fit_positions[b]]].frame.rows.size();
                     });
    std::vector<std::size_t> claim_positions(k);
    std::vector<std::size_t> claim_ranges(k);
    for (std::size_t t = 0; t < k; ++t) {
      claim_positions[t] = fit_positions[claim_perm[t]];
      claim_ranges[t] = fit_task_range[claim_perm[t]];
    }
    fit_positions = std::move(claim_positions);
    fit_task_range = std::move(claim_ranges);
  }

  // SurfaceDb defines n_threads=0 as outer-serial. When several boards fan out
  // across the shared pool (worker_budget > 1) each board must NOT also claim the
  // whole pool -- that nests H^2 workers and oversubscribes.
  const unsigned requested_budget = cfg.n_threads != 0u ? cfg.n_threads : 1u;
  // C4 wave-2 (perf, finding 13): the compute-bound fit path scales to the physical
  // P-cores and REGRESSES past them on a hybrid P/E host (unpinned outer workers
  // spill onto E-cores and oversubscribe them; own baseline peaks at 8 = the P-core
  // logical CPUs on the i7-1260P). Cap the outer budget at the discovered P-core
  // count and pin each worker to a P-core (below), so the fan-out stays on the
  // cores that scale. Best-effort: performance_core_count() is 0 when discovery is
  // unavailable (non-Windows / API failure) -> no cap, historical behaviour.
  // Pinning never changes which board a worker fits, so every surface stays
  // byte-identical across the cap AND across worker counts (the existing
  // SharedWorkerBudgetKeepsOutputByteIdentical gate still holds).
  const unsigned p_cores = cfg.pin_outer_workers ? detail::performance_core_count() : 0u;
  const unsigned worker_budget =
      (p_cores > 0u && requested_budget > p_cores) ? p_cores : requested_budget;
  const detail::FitAffinity outer_affinity =
      (cfg.pin_outer_workers && p_cores > 0u) ? detail::FitAffinity::PerformanceCores
                                              : detail::FitAffinity::None;
  const std::size_t n_fit_boards = fit_positions.size();

  // ── U4 (R-14) [pure-refactor]: shared worker budget for small books ─────────
  // The prior fix pinned every board to a single inner worker (fit_workers = 1).
  // That is right once the book is at least as large as the budget, but strands
  // cores on a SMALL book: 2 boards on a 12-wide pool used 2 cores and left 10
  // idle. Instead, SPLIT the shared budget across the boards -- each board's
  // inner fit gets budget / min(budget, n_boards) workers (>= 1). A 1-board run
  // claims the whole budget; a 4-board run on a 12-wide pool gets 3 each (12
  // cores busy, not 4); a book at or above the budget still gets 1 each -- so the
  // slices sum to the budget and never nest-oversubscribe. The per-board fan-out
  // reaches E1's nested-budget executor through fit_board, so the concurrent
  // inner dispatches share the one pool cooperatively (E2 help-first
  // work-stealing) rather than spawning bare threads that re-enter it. This
  // changes ONLY how many workers each fit is offered: every fit_board is
  // deterministic and writes disjoint slots, so each surface is bit-identical for
  // any worker count -- proved byte-for-byte against the serial reference by
  // SurfaceDbPopulate.SharedWorkerBudgetKeepsOutputByteIdentical and the existing
  // GlobalParallelQueuePreservesDeterministicPartitions. worker_budget <= 1 is
  // the documented outer-serial mode: inner fits keep auto sizing (0).
  const unsigned inner_fit_workers =
      (worker_budget > 1u && n_fit_boards > 0u)
          ? std::max<unsigned>(1u, worker_budget / static_cast<unsigned>(std::min<std::size_t>(
                                                       worker_budget, n_fit_boards)))
          : 0u;
  if (test_hooks != nullptr && test_hooks->on_inner_fit_workers) {
    test_hooks->on_inner_fit_workers(inner_fit_workers);
  }

  const auto fit_task = [&](std::size_t task_index) -> Status {
    const std::size_t pos = fit_positions[task_index];
    std::atomic<std::size_t> &date_remaining = remaining[fit_task_range[task_index]];
    // Mark this board done -- and wake the drain when its date's last board
    // finishes -- on scope exit, so a completed date drains and releases even
    // if fit_board throws (a default FitSlot then reads as a failed fit). The
    // decrement runs after the slot write, so a drain that observes zero sees
    // every completed slot for the date (acq_rel/acquire pair). This is what
    // streams writes and bounds peak RSS.
    struct MarkDone {
      std::atomic<std::size_t> &counter;
      ~MarkDone() {
        if (counter.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
          counter.notify_all();
        }
      }
    } mark_done{date_remaining};

    const CorpusBoard &board = boards[order[pos]];
    if (test_hooks != nullptr && test_hooks->before_board_fit) {
      test_hooks->before_board_fit(board.date, board.symbol);
    }
    const SymbolFitConfig &resolved = resolved_cfgs[pos];
    PricerConfig pc = pricer_config_for_symbol(resolved);
    // Per-board slice of the shared budget (0 = auto, the outer-serial mode).
    pc.fit_workers = inner_fit_workers;
    // F4 (C2 warm-start chain — deliberately OFF for populate). fit_board's
    // `out_caches` is left nullptr here (no cross-date correction-cache carry),
    // unlike build_corpus's per-symbol chain (corpus.cpp). This is NOT an
    // oversight: every populate board is a v2 RISK request (map_legacy_fit_preset
    // -> {Balanced,Risk}; is_v2_request() true), and the risk pipeline pins
    // `use_correction_cache = false` (pricer_fitter.cpp apply_risk_policy) because
    // a served risk surface is priced by the ACCURATE cold Andersen-Lake path, not
    // by an interpolated correction cache. With the cache disabled the fit builds
    // NONE, so fit_board's `out_caches` would come back empty every board and the
    // chain would carry nothing — engaging it would add per-symbol chain
    // bookkeeping and P-core chain-sharding for zero reuse (and, if the cache were
    // force-built solely to warm the chain, it would spend hundreds of ms per board
    // on an artifact the served path never reads). The determinism gate is also
    // preserved trivially: nullptr caches => byte-identical across worker budgets.
    // Re-engaging it is only worthwhile if a future mark-grade populate tier turns
    // `use_correction_cache` back on for the SERVED surface (report M1/F4).
    slots[pos] = fit_board(board, pc, /*admission=*/nullptr, [&resolved](SessionInputs &in) {
      apply_symbol_config(resolved, in);
    });
    return Ok();
  };

  // Launch the shared bounded queue on a helper thread, then DRAIN each date in
  // ascending order as its fits complete -- aggregating stats, writing the
  // partition, and RELEASING that date's fitted surfaces before later dates
  // finish. Peak RSS is thus O(dates in flight), not O(all dates) (R-03: the
  // 519-name OOM). [pure-refactor] Numerically nothing changes: every
  // fit_board is independent and deterministic, and the drain visits dates and
  // boards in the exact same date/symbol order as the prior
  // launch-then-join-then-write loop, so all surfaces and stats are
  // byte-identical -- only memory lifetime changes. The helper thread never
  // waits on the drain (workers only fit + decrement), so the join at block
  // exit cannot deadlock even when the drain returns early on a write error.
  Status fit_status = Ok();
  // R1-a: fault-injection seam forwarded from the populate's own hooks (see
  // PopulateTestHooks). Declared OUTSIDE the block so it outlives `fit_runner`,
  // which captures a pointer to it. nullptr — the production shape — whenever no
  // scheduler hook is set, so the scheduler's hot path is untouched.
  detail::FitSchedulerTestHooks sched_hooks;
  if (test_hooks != nullptr) {
    sched_hooks.before_worker_launch = test_hooks->before_worker_launch;
    sched_hooks.before_setup = test_hooks->before_scheduler_setup;
  }
  const detail::FitSchedulerTestHooks *sched_hooks_ptr =
      (sched_hooks.before_worker_launch || sched_hooks.before_setup) ? &sched_hooks : nullptr;
  {
    std::jthread fit_runner([&] {
      // C4 wave-2: pin outer workers to the discovered P-cores (byte-identical to
      // the unpinned path — pinning only steers WHICH logical CPU a worker runs on).
      fit_status = detail::run_bounded_fit_tasks(fit_positions.size(), worker_budget, fit_task,
                                                 outer_affinity, sched_hooks_ptr);

      // ── R1-a (review C-06): publish scheduler TERMINATION to the drain ───────
      // `run_bounded_fit_tasks` has two PRE-TASK failure returns (a background
      // worker-launch failure and a scratch-allocation failure). On both, not one
      // task ran, so not one `MarkDone` destructor fired and every non-zero
      // `remaining[r]` is frozen at its initial value. The drain used to sleep on
      // exactly those counters forever — the process hung with no output and never
      // reached the join below that would have let it observe `fit_status`.
      //
      // MEMORY ORDER. `fit_status` is written by THIS thread and read by the drain,
      // so the sentinel store below is the RELEASE that publishes it and the drain's
      // acquire load of the same counter is the matching ACQUIRE. The assignment
      // above is sequenced before the store, so a drain that sees the sentinel sees
      // the Status.
      //
      // SAFETY OF MUTATING `remaining` HERE. `run_bounded_fit_tasks` joins every
      // worker it started before returning on EVERY path (the launch-abort path
      // clears the vector, which joins; the normal path joins at block exit; the
      // outer catch is reached only with no worker alive). So by this line no
      // `MarkDone` destructor exists to race a `fetch_sub` against these stores,
      // and `remaining` is touched only by this thread and the drain.
      //
      // A counter already at 0 is LEFT ALONE: that date completed and must still
      // drain and write normally. On a successful run every counter is 0 here and
      // this loop stores nothing at all — the success path is bit-for-bit
      // unchanged, which is what keeps the byte-identical-for-any-thread-count
      // invariant intact.
      for (std::atomic<std::size_t> &counter : remaining) {
        if (counter.load(std::memory_order_relaxed) == 0u) {
          continue;
        }
        counter.store(kSchedulerAborted, std::memory_order_release);
        counter.notify_all();
      }
    });

    for (std::size_t r = 0; r < date_ranges.size(); ++r) {
      const DateRange &range = date_ranges[r];
      if (range.skip) {
        continue;
      }
      // Block until every enabled fit in this date has completed, OR the fit
      // scheduler terminated with fits still outstanding. A range with no enabled
      // boards starts at zero and proceeds immediately.
      //
      // ── R1-a (review C-06): why this cannot miss a wakeup ────────────────────
      // The loop sleeps ONLY in `remaining[r].wait(left)` with `left` a value it
      // has just loaded from `remaining[r]` that is neither of the two exit values
      // (0 = date complete, kSchedulerAborted = scheduler gone). Every state change
      // that could release this date CHANGES THE VALUE OF THIS VERY OBJECT and then
      // notifies it: the last `MarkDone` fetch_sub reaching 0, or the fit runner's
      // sentinel store. `std::atomic<T>::wait(old)` is specified to return
      // immediately when the object's value differs from `old`, so:
      //   - if the change lands BEFORE the wait, the value comparison fails and
      //     the wait does not block;
      //   - if it lands AFTER, the notify wakes it.
      // There is no third window, and therefore no lost wakeup. A separate
      // `std::atomic<bool>` flag would NOT have this property — it can be set and
      // notified between the drain's last read of it and its entry into a wait
      // keyed on a different object — which is exactly why the termination signal
      // is folded into the counter instead of living beside it.
      std::size_t left = remaining[r].load(std::memory_order_acquire);
      while (left != 0u && left != kSchedulerAborted) {
        remaining[r].wait(left, std::memory_order_acquire);
        left = remaining[r].load(std::memory_order_acquire);
      }
      if (left == kSchedulerAborted) {
        // The scheduler died before this date's fits could run. STOP: do not write
        // a partition from a date whose cells were never fitted, and do not walk
        // on to later dates whose counters are frozen for the same reason.
        //
        // Propagate the SCHEDULER'S OWN Status — its message already names the
        // cause ("worker launch failed" / "scheduler setup failed") — rather than
        // inventing a code that would tell the operator less. The acquire load
        // that produced the sentinel synchronizes-with the runner's release store,
        // so `fit_status` is fully published here even though the runner thread is
        // not yet joined.
        //
        // Dates already drained AND written above are already durable (each
        // `write_partition` is an atomic tmp+rename plus a generation-bumped
        // manifest) and are NOT rolled back: the date is the resume unit, and a
        // re-run's cell-aware filter skips them. See the U3 note below.
        //
        // The `fit_status` fallback is unreachable by construction — an Ok return
        // from `run_bounded_fit_tasks` means every index ran, hence every counter
        // reached 0 and no sentinel was stored — and exists so that a future
        // scheduler change which broke that property would surface as a loud error
        // rather than silently reinstating the hang.
        return fit_status ? Err(ErrorCode::Internal,
                                "populate_surface_db: fit scheduler terminated with date '" +
                                    boards[order[range.begin]].date + "' incomplete")
                          : Err(fit_status.error());
      }

      const std::size_t range_n = range.end - range.begin;
      const std::string &date = boards[order[range.begin]].date;

      // ── Sequential aggregation: stats + owning storage for the write ────────
      // `items` holds a string_view into `names` and a pointer into `stamped`, so
      // NEITHER may reallocate while items are live. Carried cells push into the
      // same two vectors, so the reserve must cover them too -- a carry list is a
      // subset of this date's boards on the streaming path, but this is a public
      // config field and must not corrupt memory if a direct caller oversteps.
      const auto carry_it = cfg.carry_over.find(date);
      const std::size_t carry_n =
          carry_it != cfg.carry_over.end() ? carry_it->second.size() : 0u;
      std::vector<std::string> names;     // owning symbol storage (kept alive
      std::vector<PricedSurface> stamped; // across write_partition below)
      names.reserve(range_n + carry_n);
      stamped.reserve(range_n + carry_n);
      std::vector<SurfaceArchiveItem> items;
      items.reserve(range_n + carry_n);

      // ── FIX-D: read the carried cells back BEFORE the write ─────────────────
      // db.write_partition replaces the partition file (tmp+rename) and evicts
      // its cache, so the existing records must be materialized into OWNED
      // surfaces first. `reconstruct_entry` (not reconstruct_symbol) is required:
      // it returns the record's own SurfaceProvenance in the same pass, and
      // write_partition writes that provenance back into the manifest symbol
      // entry -- dropping it would silently downgrade every carried symbol's
      // manifest provenance to legacy. The archive is closed at the end of this
      // block, before the write.
      //
      // ── FIX-F (M-6): a read-back failure here ABORTS THE BUILD, deliberately ──
      // Before FIX-E only an ENABLED carry reached this loop, and only behind
      // `carry_valid`; a DISABLED carry now reaches it unconditionally, on the
      // database class most likely to hold an old or damaged record. So the
      // blast radius of a `find`/`reconstruct_entry` failure grew from "a carry
      // the caller opted into" to "any rewrite of a date holding a disabled
      // symbol", and the decision to fail loud is re-taken here rather than
      // inherited:
      //
      //   - The alternative is to SKIP the unreadable record and write the
      //     partition without it -- which is the FIX-E defect exactly, executed
      //     on the one record we already know we cannot reproduce. A corrupt
      //     record that cannot be re-emitted is a record the rewrite would
      //     DELETE. Silence here converts "one record is unreadable" into "one
      //     record no longer exists".
      //   - Aborting costs nothing already earned. `db.write_partition` for THIS
      //     date has not run, so the partition file and manifest are untouched;
      //     every EARLIER date was already committed atomically (see the U3 note
      //     at the end of this function) and a re-run with the cell-aware filter
      //     skips them. The operator loses the remainder of one run, not data.
      //   - It is diagnosable: the archive's own error (NotFound / ParseError /
      //     checksum) names the partition and the record, and `verify` walks the
      //     same bytes.
      //
      // Documented as a failure mode on `populate_surface_db` (header) and in
      // the operator manual's resume-semantics section. Do not "scope" this to a
      // skip without re-arguing the three points above.
      if (carry_n > 0u) {
        const Result<SurfaceArchiveV2> part = db.open_partition(date);
        if (!part) {
          return Err(part.error());
        }
        for (const std::string &sym : carry_it->second) {
          const Result<ArchiveV2DirEntry> entry = part->find(sym);
          if (!entry) {
            return Err(entry.error());
          }
          Result<ArchivedSurface> got = part->reconstruct_entry(*entry);
          if (!got) {
            return Err(got.error());
          }
          names.push_back(sym);
          stamped.push_back(std::move(got->surface));
          items.push_back(SurfaceArchiveItem{names.back(), &stamped.back(), got->provenance});
        }
      }

      for (std::size_t pos = range.begin; pos < range.end; ++pos) {
        const CorpusBoard &board = boards[order[pos]];
        SymbolAccum &acc = per_symbol[board.symbol];
        acc.stats.symbol = board.symbol;
        ++acc.stats.n_attempted;

        const SymbolFitConfig &resolved = resolved_cfgs[pos];
        if (!resolved.enabled) {
          ++acc.stats.n_disabled;
          // FIX-E. A disabled cell is still never FITTED -- that policy is
          // unchanged, and `n_disabled` keeps counting it, which is what the
          // stats CSV's fit denominator subtracts. What changed is that the
          // caller may have asked for its ALREADY-STORED record to be re-emitted
          // rather than dropped; the read-back above has already appended that
          // item, so it must be tallied here or the caller's cross-check would
          // describe a write that did not match its request. This branch stays
          // BEFORE the carried branch on purpose: `n_carried` must not absorb a
          // disabled cell (see the counter's contract in the header).
          if (carried[pos]) {
            ++stats.n_carried_disabled;
          }
          continue;
        }

        // FIX-D: a carried cell was never dispatched, so its slot is empty and
        // must not be read as a failed fit. Its item was already appended above.
        // It is counted in n_carried and deliberately NOT in n_ok: n_ok means
        // "cells this run fitted", which is what `is_total_fit_failure`'s
        // cells_ok == 0 clause (surface_db_build.cpp) depends on.
        if (carried[pos]) {
          ++acc.stats.n_carried;
          ++stats.n_carried;
          continue;
        }

        const FitSlot &slot = slots[pos];
        if (slot.status == CorpusFitStatus::Ok) {
          ++acc.stats.n_ok;
          ++stats.n_ok;
          if (slot.oos_in_band_available) {
            acc.oos_sum += slot.oos_in_band;
            ++acc.oos_count;
          }
          const std::uint32_t uid = uid_for_symbol(board.symbol);
          Result<PricedSurface> stamped_surface = with_uid(slot.surface.value(), uid);
          if (!stamped_surface) {
            return Err(stamped_surface.error());
          }
          names.push_back(board.symbol);
          stamped.push_back(std::move(*stamped_surface));
          items.push_back(SurfaceArchiveItem{names.back(), &stamped.back(), slot.provenance});
        } else {
          ++acc.stats.n_failed;
          ++stats.n_failed;
          // ── FIX-F (FIX-E review I-2): the stored record is NOT preserved here,
          // and that is a KNOWN, ARGUED limit rather than an oversight ─────────
          // A cell that fitted once and later degraded is `present`, ENABLED, and
          // fails its re-fit; nothing is appended to `items`, so the
          // whole-partition rewrite below drops the surface it had. The
          // would-drop guard cannot help — the cell was counted into `present`
          // before its outcome was known, the same structural cause FIX-E fixed
          // for the disabled case. Preserving the bytes is a ~10-line change
          // right here (read the record back beside the disabled carry above),
          // and it is deliberately NOT made, because presence is load-bearing
          // one frame up:
          //
          //   `populate_universe_streaming` counts a cell into `to_add` only when
          //   it is ABSENT from the partition, and a date with `to_add == 0` is
          //   `dates_skipped_complete` — never rewritten again. So a preserved
          //   failed cell retires its own date from the rewrite set: the cell is
          //   never re-attempted, the failure never re-reported, and a surface
          //   the fitter has just REJECTED is served indefinitely with nothing on
          //   disk recording that. Withholding the fingerprint attestation does
          //   not rescue this — the skip happens before `carry_valid` is ever
          //   consulted. It would also contradict the "a failing cell is retried
          //   forever; there is no persisted known-failed state" contract that
          //   `is_total_fit_failure`, `is_carry_masked_fit_failure` and
          //   `build_surface_db` all document and depend on.
          //
          // Trading a one-shot data loss for permanent silent staleness is the
          // worse of the two. The fix needs PERSISTED per-cell state — "these
          // bytes are a preserved failure: re-attempt this cell, never carry it"
          // — which neither the manifest nor the archive record today; see
          // `.superpowers/sdd/surface-db-prod/fixF-report.md` for the format
          // change that would carry it. Pinned by
          // SurfaceDbPopulate.DegradedCellLosesItsStoredSurfaceAndPresenceIsWhatDrivesTheRetry,
          // which asserts BOTH halves so a future preserve cannot land without
          // confronting the second one.
          //
          // Record WHY, not merely that it happened: `slot.error_message` is the
          // fitter's own rejection text (corpus_board_fit.cpp), which used to die
          // here alongside the code. DETERMINISM: this push_back runs on the
          // SINGLE drain thread inside the date-ascending / (date,symbol)-ascending
          // walk, never on a fit worker, so the list order is fixed by the walk and
          // is byte-identical for any worker budget. Copies (not moves) because the
          // slot is const here and is recycled below.
          stats.failed_cells.push_back(
              FailedCell{board.date, board.symbol, slot.error_code, slot.error_message});
        }
      }

      if (!items.empty()) {
        // FIX-D fix-2 (I-3): the attestation is the CALLER'S, forwarded, not this
        // function's to invent. `items` has two provenances and this frame can
        // only vouch for one of them: every FITTED item came out of `fit_board`
        // under the config this loop resolved from THIS manifest moments ago, but
        // every CARRIED item is re-emitted on the strength of `cfg.carry_over`,
        // whose validity `SurfaceDbPopulateConfig` explicitly says it carries no
        // predicate for. Stamping unconditionally asserted a gate that lives one
        // frame up, so a direct caller supplying its own `carry_over` had its
        // stale surfaces re-blessed on every resume. `cfg.attest` defaults to
        // `None`, which fails closed (fingerprint 0 = unknown = re-fit).
        //
        // `populate_universe_streaming` is the frame that RUNS the gate
        // (`carry_valid`), so it is the frame that sets `FitterProduced`.
        //
        // FIX-E qualifies the attesting caller's claim for the one item class it
        // does not cover: a PRESERVED DISABLED record is re-emitted regardless of
        // the fingerprint, so the stamp does not vouch for it. That is sound
        // because of what the stamp is USED for -- `populate_universe_streaming`
        // reads it to decide `carry_valid`, which admits only ENABLED present
        // cells to the fitted-output carry set. A disabled cell can re-enter that
        // set only by being re-enabled, and `enabled` is part of the folded
        // config, so re-enabling necessarily MOVES the fingerprint and forces a
        // re-fit. The alternative -- attesting `None` on any date holding a
        // disabled symbol -- would permanently un-carry that date's healthy
        // siblings and re-fit them on every run forever, which is the cost FIX-D
        // exists to remove.
        const Status w = db.write_partition(date, items, {}, cfg.attest);
        if (!w) {
          return Err(w.error());
        }
        ++stats.n_dates_written;
        if (test_hooks != nullptr && test_hooks->after_partition_write) {
          test_hooks->after_partition_write(date);
        }
      }

      // Release this date's fitted surfaces now that its partition is written
      // and its stats consumed -- the O(all)->O(in-flight) memory bound. The
      // lightweight status/oos fields were already read above.
      for (std::size_t pos = range.begin; pos < range.end; ++pos) {
        slots[pos] = FitSlot{};
      }
    }
  }
  // ── U3 (R-12) [correctness]: durability across a fit-worker exception ───────
  // By the time control reaches here, EVERY completed date has already been
  // written by the drain loop above: each db.write_partition atomically commits
  // that date's archive file (tmp+rename) AND a generation-bumped manifest, and
  // it runs BEFORE the fit_runner join at the enclosing block's exit. So a fit
  // worker exception -- bad_alloc in fit_board, the slot move, or any throw --
  // reaches `fit_status` only AFTER the earlier dates are durable on disk, and
  // returning Err here CANNOT roll them back: a re-run with skip_existing sees
  // the finished dates and skips them (crash-resume; no hours-of-fits loss).
  // This is exactly the date-granular durability R-12 flagged as regressed
  // before the streaming writer; the guarantee is pinned by
  // SurfaceDbPopulate.CompletedDatesSurviveLaterWorkerThrow.
  if (!fit_status) {
    return Err(fit_status.error());
  }

  stats.per_symbol.reserve(per_symbol.size());
  for (auto &[symbol, acc] : per_symbol) {
    (void)symbol;
    PopulateSymbolStats s = std::move(acc.stats);
    s.mean_oos_in_band = acc.oos_count > 0u ? acc.oos_sum / static_cast<double>(acc.oos_count)
                                            : std::numeric_limits<double>::quiet_NaN();
    stats.per_symbol.push_back(std::move(s));
  }

  return Ok(std::move(stats));
}

Status write_populate_stats_csv(const SurfaceDbPopulateStats &s, const MetaKv &meta,
                                std::string_view path) {
  MetaKv full_meta = meta;
  full_meta.emplace_back("n_boards", fmt_u32(s.n_boards));
  full_meta.emplace_back("n_ok", fmt_u32(s.n_ok));
  full_meta.emplace_back("n_failed", fmt_u32(s.n_failed));
  // FIX-D fix-1 (I2). Beside n_ok/n_failed because it is the third disposition of
  // the same cells and the only evidence that carry-over ran: a converged carry
  // resume writes n_ok=0, n_failed=0 and would otherwise look like a no-op.
  full_meta.emplace_back("n_carried", fmt_u32(s.n_carried));
  full_meta.emplace_back("n_dates_written", fmt_u32(s.n_dates_written));

  std::string body;
  body.reserve(s.per_symbol.size() * 64 + 64);
  // `n_carried` is APPENDED to the pinned header, not inserted, so a positional
  // reader of the older columns is unaffected. It goes last rather than beside
  // n_disabled for that reason, even though it reads better next to it.
  body += "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band,n_carried\n";
  for (const PopulateSymbolStats &sym : s.per_symbol) {
    // FIX-D fix-1 (I3): a CARRIED cell was never offered to the fitter, so it
    // belongs in neither half of a FIT success rate. Leaving it in the denominator
    // reported 0% for every carried symbol (n_ok = 0 over n_attempted = 1) on
    // exactly the healthy resume this feature produces.
    //
    // Excluding it is necessary but NOT sufficient: on a fully-carried symbol the
    // exclusion empties the denominator, and the old `max(1, used)` floor then
    // still printed 0/1 = 0 -- the same false 0% by a different route. An empty
    // denominator means the rate is UNDEFINED (no cell was offered to the fitter),
    // not zero, so it is emitted as `nan` -- the same "unavailable" convention
    // this row already uses for mean_oos_in_band, and read as a missing value by
    // the CSV's pandas consumers. This also corrects the pre-existing all-disabled
    // row, which reported 0% for the same reason.
    const std::uint32_t excluded = sym.n_disabled + sym.n_carried;
    const std::uint32_t used = sym.n_attempted > excluded ? sym.n_attempted - excluded : 0u;
    const double success_rate = used > 0u ? static_cast<double>(sym.n_ok) /
                                                static_cast<double>(used)
                                          : std::numeric_limits<double>::quiet_NaN();
    body += sym.symbol;
    body += ',';
    body += fmt_u32(sym.n_attempted);
    body += ',';
    body += fmt_u32(sym.n_ok);
    body += ',';
    body += fmt_u32(sym.n_failed);
    body += ',';
    body += fmt_u32(sym.n_disabled);
    body += ',';
    body += fmt_nan_aware10(success_rate);
    body += ',';
    body += fmt_nan_aware10(sym.mean_oos_in_band);
    body += ',';
    body += fmt_u32(sym.n_carried);
    body += '\n';
  }
  return write_meta_body(full_meta, body, path, "write_populate_stats_csv");
}

Result<UniversePopulateCoverage>
populate_universe_streaming(SurfaceDb &db, std::span<const CorpusBoard> boards,
                            const UniversePopulateSpec &spec,
                            const PopulateTestHooks *test_hooks) {
  UniversePopulateCoverage cov;
  cov.cells_loaded = static_cast<std::uint32_t>(boards.size());
  if (boards.empty()) {
    return Ok(std::move(cov)); // graceful no-op: an un-pulled window has no boards
  }

  // 1. Seed per-symbol manifest configs (idempotent). The index leg is pinned to the
  //    dense recipe (the SPY/index precedent, spy_ytd_corpus.cpp); every other symbol
  //    is left on the preset's auto-selector (pin_curve=false), which picks the
  //    parsimonious backbone the board's microstructure warrants. A symbol already in
  //    the manifest is left untouched (a resumed run / operator override wins).
  for (std::size_t i = 0; i < boards.size(); ++i) {
    const std::string &sym = boards[i].symbol;
    bool first = true;
    for (std::size_t j = 0; j < i; ++j) {
      if (boards[j].symbol == sym) {
        first = false;
        break;
      }
    }
    if (!first || db.symbol_config(sym).has_value()) {
      continue;
    }
    // Seeding recipe shared bit-for-bit with generate_symbol_configs (Task 4):
    // the preset's config, dense-index-pinned for the index leg. The pin stays
    // TRUE here on purpose: this seeding is a no-op inside `build_surface_db`
    // (generate_symbol_configs has already configured every symbol, so the
    // has_value() check above skips them all), and `UniversePopulateSpec` carries
    // no operator knob — so flipping it would silently change the contract for
    // direct callers of this driver without any way to opt back. The operator's
    // choice lives on AutoConfigSpec::pin_curve_family, one stage up.
    const SymbolFitConfig c = seed_symbol_config(sym, spec.preset, spec.index_symbol,
                                                 /*pin_curve_family=*/true);
    const Status up = db.upsert_symbol(sym, c);
    if (!up) {
      return Err(up.error());
    }
  }

  // 2. Cell-aware resume: group by date, and (re)write a date only when a loaded
  //    board adds a symbol the partition does not already carry. A whole-date rewrite
  //    re-fits the already-present cells (the price of date-keyed partitions), so it
  //    is guarded to never DROP an existing symbol absent from this run's loaded set.
  std::map<std::string, std::vector<std::size_t>> by_date;
  for (std::size_t i = 0; i < boards.size(); ++i) {
    by_date[boards[i].date].push_back(i);
  }
  cov.dates_total = static_cast<std::uint32_t>(by_date.size());

  // The fallback the populate below will resolve an unconfigured symbol against;
  // hoisted so the filter's disabled-check and the fit's are the SAME decision.
  const SymbolFitConfig fallback_cfg = symbol_config_from_preset(spec.preset);

  std::vector<CorpusBoard> kept; // boards on the dates that need a (re)write
  std::map<std::string, std::vector<std::string>> carry_over;
  for (const auto &[date, idxs] : by_date) {
    const Result<SurfaceArchiveV2> part = db.open_partition(date); // Err(NotFound) if none yet

    // ── FIX-D: is this partition's stored work still valid to reuse? ──────────
    // The predicate is deliberately WHOLE-PARTITION and conservative: if ANY
    // symbol in the file has a config that differs from the manifest's current
    // one, nothing on this date is carried and the date re-fits exactly as
    // before. A partition is rewritten whole anyway, so per-cell granularity
    // would buy nothing and would let a config change to one symbol hide another
    // symbol's staleness in the same file. A 0 stored fingerprint means UNKNOWN
    // (a manifest written before the field existed) and never carries -- which is
    // why the first resume of a pre-FIX-D database still re-fits once, then
    // converges. See kSurfaceDbCarryOverFitSalt for what this does NOT catch.
    bool carry_valid = false;
    if (part.has_value()) {
      std::vector<std::string> part_symbols;
      part_symbols.reserve(part->directory().size());
      for (const ArchiveV2DirEntry &e : part->directory()) {
        part_symbols.emplace_back(e.symbol, e.symbol_len);
      }
      const std::uint64_t stored = db.partition_config_fingerprint(date);
      carry_valid = stored != 0u && stored == db.config_fingerprint(part_symbols);
    }

    std::uint32_t present = 0;
    std::uint32_t to_add = 0;
    std::vector<std::string> carry_syms;
    // ── FIX-E: present-but-DISABLED cells, tracked SEPARATELY ────────────────
    // Kept in their own list because the question that admits them is a DIFFERENT
    // question. `carry_valid` is the fit-config fingerprint predicate; it asks
    // "are these stored surfaces still sound to reuse AS THIS RUN'S FITTED
    // OUTPUT?". That question does not arise for a symbol this run produces no
    // output for at all: the alternative to carrying a disabled cell is not
    // re-fitting it, it is DELETING it. So a disabled present cell is carried
    // UNCONDITIONALLY, and gating it on `carry_valid` would be a fix-shaped
    // no-op -- disabling a symbol MOVES the config fold, so `carry_valid` is
    // false on precisely the first run after the disable, and a pre-FIX-D
    // manifest stores the 0 "unknown" fingerprint, which never carries either.
    // Both cases are pinned by tests.
    std::vector<std::string> disabled_carry_syms;
    for (const std::size_t i : idxs) {
      const bool in_db = part.has_value() && part->find(boards[i].symbol).has_value();
      if (in_db) {
        ++present;
        // `enabled = false` means STOP FITTING this symbol. It does NOT mean
        // DELETE what is already stored -- and deletion is what happened before
        // FIX-E: the cell was counted into `present` here, excluded from the
        // carry set, then skipped by the populate's disabled branch, so the
        // whole-partition rewrite (tmp+rename, no merge, no soft-delete) simply
        // dropped it. The would-drop guard below could not save it either,
        // because it was already counted into the very number that guard
        // compares against `part->count()`.
        //
        // A disabled config is "a standing failure, not a settled state" (FIX-C,
        // which is why `--retry-disabled` exists): a provisional, reversible
        // marker must never trigger irreversible destruction. Preserving also
        // makes the read path honest -- nothing in load_surface/map_surface
        // gates on `enabled`, so a preserved surface stays servable for the
        // dates that already worked.
        if (!resolve_symbol_config(db, boards[i].symbol, fallback_cfg).enabled) {
          disabled_carry_syms.push_back(
              detail::canonicalize_symbol(boards[i].symbol, kSurfaceDbKeyMax));
        } else if (carry_valid) {
          carry_syms.push_back(
              detail::canonicalize_symbol(boards[i].symbol, kSurfaceDbKeyMax));
        }
        continue;
      }
      // A cell whose resolved config is DISABLED can never be added: the
      // population loop skips it (n_disabled) so it never lands in the written
      // partition. Counting it as "to add" would make EVERY later run see the
      // same permanent gap and rewrite -- and therefore re-fit -- the whole date,
      // so the build would never reach a fixed point. Resolved through the same
      // seam the population loop uses, so the two cannot drift.
      if (!resolve_symbol_config(db, boards[i].symbol, fallback_cfg).enabled) {
        continue;
      }
      ++to_add;
    }
    if (to_add == 0u) {
      // Nothing left to add: every loaded cell is either already present or
      // config-disabled (a disabled cell can never be added, so a date whose only
      // gap is disabled symbols is COMPLETE, not pending — that is what makes a
      // rebuild converge). `cells_already_present` counts only the present ones.
      ++cov.dates_skipped_complete;
      cov.cells_already_present += present;
      continue;
    }
    // Guard: the partition holds a symbol not in this run's loaded set (present <
    // total) — a whole-partition rewrite from `idxs` alone would drop it. Skip.
    //
    // It compares a COUNT, so it assumes `idxs` holds at most one board per
    // symbol. A duplicate (date, symbol) board would double-count `present` and
    // could push it up to `part->count()` while a stored symbol really is missing
    // from the loaded set. That fails safe TODAY only by accident of a downstream
    // check: `write_surface_archive_v2_file` rejects a duplicate canonical symbol
    // with AlreadyExists (surface_archive.cpp), so the build aborts loudly instead
    // of dropping the surface. Do not remove that rejection without making this a
    // set comparison — it is the only thing standing behind this line.
    if (part.has_value() && present < part->count()) {
      ++cov.dates_skipped_would_drop;
      cov.cells_already_present += present;
      continue;
    }
    ++cov.dates_written;
    cov.cells_to_fit += to_add;
    // FIX-D: the already-present cells split into carried (re-emitted verbatim)
    // and refit (everything the predicate could not vouch for). `kept` still gets
    // ALL of idxs so the per-symbol n_attempted / n_disabled bookkeeping is
    // unchanged; populate_surface_db skips the fit for the carried ones.
    const auto carried_here = static_cast<std::uint32_t>(carry_syms.size());
    // FIX-E: preserved-because-disabled cells are their OWN disposition. They
    // were never offered to the fitter, so they are not `cells_refit`; and
    // `cells_carried` must keep meaning "healthy stored surface reused instead of
    // re-fitted", because `is_total_fit_failure` / `is_total_config_failure` read
    // it as evidence the run produced a serviceable database -- which a switched-
    // off config's leftover bytes are not. Every in_db cell lands in exactly one
    // of the three, so `present - carried_here - disabled_here` cannot underflow.
    const auto disabled_here = static_cast<std::uint32_t>(disabled_carry_syms.size());
    cov.cells_carried += carried_here;
    cov.cells_carried_disabled += disabled_here;
    cov.cells_refit += present - carried_here - disabled_here;
    carry_syms.insert(carry_syms.end(), std::make_move_iterator(disabled_carry_syms.begin()),
                      std::make_move_iterator(disabled_carry_syms.end()));
    if (!carry_syms.empty()) {
      carry_over.emplace(date, std::move(carry_syms));
    }
    for (const std::size_t i : idxs) {
      kept.push_back(boards[i]);
    }
  }

  // 3. Fuse fit->serialize->release over the needing-work dates via the streaming
  //    populate (executor fan-out, RSS O(dates in flight)). skip_existing is off:
  //    the cell-aware filter above already chose exactly the dates to rewrite.
  if (!kept.empty()) {
    SurfaceDbPopulateConfig pcfg;
    pcfg.fallback = fallback_cfg;
    pcfg.n_threads = spec.fit_workers;
    pcfg.skip_existing = false;
    pcfg.carry_over = std::move(carry_over);
    // FIX-D fix-2 (I-3): the frame that RAN the gate is the frame that attests.
    // `carry_valid` above is the whole claim -- a non-zero stored fingerprint
    // equal to a freshly recomputed fold over the partition's symbols -- and it
    // is evaluated HERE, so the `FitterProduced` stamp is set HERE and forwarded
    // to `write_partition` by the populate rather than asserted by it. Everything
    // else in the write is this run's own fit under the configs seeded above.
    pcfg.attest = DbConfigAttestation::FitterProduced;
    const Result<SurfaceDbPopulateStats> st = populate_surface_db(db, kept, pcfg, test_hooks);
    if (!st) {
      return Err(st.error());
    }
    cov.cells_ok = st->n_ok;
    cov.cells_failed = st->n_failed;
    // Cross-check the two halves of the carry decision: what the filter above
    // asked for must equal what the populate actually re-emitted, or the
    // coverage counters would describe a write that did not happen.
    if (st->n_carried != cov.cells_carried) {
      return Err(ErrorCode::Internal,
                 "populate_universe_streaming: carried-cell count disagrees with populate");
    }
    // FIX-E: the same cross-check on the OTHER half of the carry request. A
    // disabled cell reaches the populate through the same `carry_over` map but is
    // tallied on a different branch, so a mis-ordered branch there would silently
    // stop counting preserved cells while still re-emitting them (or, worse, stop
    // re-emitting them) with nothing to notice.
    if (st->n_carried_disabled != cov.cells_carried_disabled) {
      return Err(ErrorCode::Internal, "populate_universe_streaming: preserved-disabled-cell count "
                                      "disagrees with populate");
    }
    cov.per_symbol = std::move(st->per_symbol);
    // The per-cell reasons ride along with the count they explain; the populate
    // already ordered them by (date, symbol), so nothing re-sorts here.
    cov.failed_cells = st->failed_cells;
  }

  return Ok(std::move(cov));
}

} // namespace atx::vol

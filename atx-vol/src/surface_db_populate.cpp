#include "atx/vol/surface_db_populate.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/detail/fit_scheduler.hpp" // run_bounded_fit_tasks
#include "atx/vol/dispersion.hpp"           // with_uid
#include "atx/vol/pricer_fitter.hpp"        // PricerConfig
#include "atx/vol/session.hpp"              // SessionInputs
#include "atx/vol/universe.hpp"             // uid_for_symbol
#include "atx/vol/vol_curve.hpp"            // CurveConfig (index dense pin)
#include "corpus_board_fit.hpp"             // FitSlot, fit_board (shared blessed fit path)

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
      const Result<SymbolFitConfig> found = db.symbol_config(board.symbol);
      resolved_cfgs[pos] = found.has_value() ? *found : cfg.fallback;
      if (resolved_cfgs[pos].enabled) {
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
  // P3.1 (perf): when the opt-in E-core tier is armed, the scaling knee moves from
  // "the P-cores" to "the P-cores plus the E-cores", because the second tier gives
  // each spilled worker its OWN E-core logical CPU instead of double-booking a
  // P-core. Raise the cap accordingly so the budget can actually reach the E-tier;
  // with the flag unset `e_cores` is 0 and both the cap and the affinity collapse
  // to the exact C4 wave-2 behaviour above.
  const unsigned e_cores = (cfg.pin_outer_workers && detail::efficiency_core_tier_enabled())
                               ? detail::efficiency_core_count()
                               : 0u;
  const unsigned core_cap = p_cores + e_cores;
  const unsigned worker_budget =
      (core_cap > 0u && requested_budget > core_cap) ? core_cap : requested_budget;
  const detail::FitAffinity outer_affinity =
      (cfg.pin_outer_workers && p_cores > 0u)
          ? (e_cores > 0u ? detail::FitAffinity::PerformanceThenEfficiencyCores
                          : detail::FitAffinity::PerformanceCores)
          : detail::FitAffinity::None;
  const std::size_t n_fit_boards = fit_positions.size();

  // ── U4 (R-14) [pure-refactor]: shared worker budget for small books ─────────
  // The prior fix pinned every board to a single inner worker (fit_workers = 1).
  // That is right once the book is at least as large as the budget, but strands
  // cores on a SMALL book: 2 boards on a 12-wide pool used 2 cores and left 10
  // idle. Instead, SPLIT the shared budget across the boards -- each board's
  // inner fit gets budget / min(budget, n_boards) workers (>= 1). [FIX-4 made
  // `n_boards` here the LIVE outstanding count rather than the whole book; the
  // sizing rule below is otherwise unchanged.] A 1-board run
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
  //
  // P3.1: the inner slice is deliberately derived from `inner_budget` -- the
  // OUTER budget MINUS the E-core tier -- not from `worker_budget`. Inner fan-out
  // is dispatched to the shared pricing executor, which is itself pinned to the
  // P-cores (configure_pricing_executor / Topology::PerformanceCores). Sizing the
  // inner slice off an E-core-widened outer budget would hand a small book more
  // inner workers than there are P-cores to run them on and re-create exactly the
  // nested oversubscription this block exists to prevent. The E-tier widens
  // across-board concurrency only; per-board inner concurrency is unchanged, so
  // this stays a pure no-op whenever the tier is disarmed.
  const unsigned inner_budget = (e_cores > 0u && worker_budget > e_cores)
                                    ? worker_budget - e_cores
                                    : worker_budget;
  // ── FIX-4: the inner budget is LIVE, not a constant of the whole call ───────
  // Before FIX-4 the slice above was computed ONCE, from `n_fit_boards` (the
  // TOTAL enabled book), and the same value was handed to every board. That is
  // strictly weaker than a claim-time resolution: a 500-board populate on an
  // 8-wide pool offered `8/8 = 1` inner worker to EVERY board, including the last
  // board of the last date, running alone while seven outer workers sat idle at
  // the join barrier. "A pool sized for the fan-out stays sized for the fan-out
  // long after the fan-out is over."
  //
  // `boards_outstanding` is the number of enabled fits that have NOT completed —
  // unclaimed plus claimed-and-still-running. It starts at the enabled count and
  // only falls (`MarkDone`, below). Every offer is resolved against its live
  // value, so the slice widens as the queue drains.
  //
  // NON-OVERSUBSCRIPTION (why the slices still sum to the budget). Fix an instant
  // t with k boards running. Board i's most recent resolution happened at some
  // t_i <= t; every board unfinished at t was also unfinished at t_i, so the
  // `left` it read satisfies left_i >= k, hence
  //   width_i = inner_budget / min(inner_budget, left_i)
  //          <= inner_budget / min(inner_budget, k).
  // Summing over the k running boards gives at most `inner_budget` whenever
  // k <= inner_budget, and at most `inner_budget` again when k > inner_budget
  // (each width is then 1). The monotone-decrease of `boards_outstanding` is the
  // whole proof; it is why the counter must be decremented only on completion.
  //
  // This is a PERF knob only: `fit_workers` selects how many workers a fan-out
  // that writes disjoint per-index slots uses, and every atx-vol fan-out is
  // documented bit-identical for any worker count (parallel_for.hpp's contract).
  // The gate is SurfaceDbPopulate.StragglerReclaimsInnerWorkersWhileStillRunning,
  // which fits one board at width 1 and another at the full budget IN THE SAME
  // RUN and byte-compares every surface against the serial reference.
  std::atomic<std::size_t> boards_outstanding{n_fit_boards};
  const auto offer_inner_fit_workers = [&](const std::string &symbol) -> unsigned {
    const std::size_t left = boards_outstanding.load(std::memory_order_acquire);
    unsigned inner = 0u; // 0 = auto sizing, the documented outer-serial mode
    if (inner_budget > 1u && n_fit_boards > 0u) {
      const std::size_t share = std::max<std::size_t>(
          1u, std::min<std::size_t>(inner_budget, std::max<std::size_t>(1u, left)));
      inner = std::max<unsigned>(1u, inner_budget / static_cast<unsigned>(share));
    }
    if (test_hooks != nullptr && test_hooks->on_inner_fit_workers) {
      test_hooks->on_inner_fit_workers(symbol, inner, left);
    }
    return inner;
  };

  const auto fit_task = [&](std::size_t task_index) -> Status {
    const std::size_t pos = fit_positions[task_index];
    std::atomic<std::size_t> &date_remaining = remaining[fit_task_range[task_index]];
    // Mark this board done -- and wake the drain when its date's last board
    // finishes -- on scope exit, so a completed date drains and releases even
    // if fit_board throws (a default FitSlot then reads as a failed fit). The
    // decrement runs after the slot write, so a drain that observes zero sees
    // every completed slot for the date (acq_rel/acquire pair). This is what
    // streams writes and bounds peak RSS.
    //
    // FIX-4: the same scope exit retires this board from `boards_outstanding`,
    // which is what makes a still-running straggler's next offer wider. The
    // outstanding decrement happens BEFORE the per-date one so that a test woken
    // by `on_board_fit_done` already observes the reclaimed count. The hook is
    // called inside try/catch: this runs from a destructor, and an escaping
    // exception would std::terminate.
    struct MarkDone {
      std::atomic<std::size_t> &counter;
      std::atomic<std::size_t> &outstanding;
      const PopulateTestHooks *hooks;
      ~MarkDone() {
        const std::size_t left = outstanding.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (hooks != nullptr && hooks->on_board_fit_done) {
          try {
            hooks->on_board_fit_done(left);
          } catch (...) { // NOLINT: a throwing test hook must not terminate
          }
        }
        if (counter.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
          counter.notify_all();
        }
      }
    } mark_done{date_remaining, boards_outstanding, test_hooks};

    const CorpusBoard &board = boards[order[pos]];
    const SymbolFitConfig &resolved = resolved_cfgs[pos];
    PricerConfig pc = pricer_config_for_symbol(resolved);
    // Decision point 1 of 2 -- CLAIM. Per-board slice of the shared budget
    // resolved against the live outstanding count (0 = auto, outer-serial mode).
    // This one sizes the pre-build phases the fitter runs off `PricerConfig`:
    // notably the held-out curve selection (pricer_fitter.cpp's `select_curve`).
    pc.fit_workers = offer_inner_fit_workers(board.symbol);
    if (test_hooks != nullptr && test_hooks->before_board_fit) {
      test_hooks->before_board_fit(board.date, board.symbol);
    }
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
    // Decision point 2 of 2 -- DRAIN-TIME, for a board ALREADY CLAIMED and STILL
    // RUNNING. `session_overlay` is invoked by PricerFitter::fit on the fitting
    // thread immediately before each `VolaSession::build` REQUEST it issues — the
    // mark build and the risk build on populate's v2 dual path
    // (pricer_fitter.cpp: the mark overlay before the async mark launch, then the
    // risk overlay after `apply_risk_policy`/`select_curve`) — and `in.fit_workers`
    // is what every fan-out inside that build reads (`run_deam_prepass` and the
    // calendar-repair fan-out in curve_fit.cpp, the slice fan-out in session.cpp,
    // the per-chain prepass in surface_parity.cpp). Re-asking here is what lets a
    // board that was claimed while the pool was saturated widen to the whole
    // budget once its siblings have retired. `apply_symbol_config` is applied
    // FIRST so the live budget wins over anything the per-symbol preset sets, and
    // `apply_risk_policy` (which pricer_fitter re-asserts after this overlay) does
    // not touch `fit_workers`, so the value survives to the build.
    //
    // ── THE TRIGGER, stated plainly ─────────────────────────────────────────
    // The trigger is the STRAGGLER BOARD'S OWN fitting thread reaching the next
    // surface-build request of the fit it is already inside. Nothing else: not a
    // sibling completing, not the drain thread, not a timer. Inner fan-out workers
    // never re-offer — only the board's own thread does.
    //
    // WHEN THE TRIGGER DOES NOT FIRE, the width does not move. Three cases, all
    // bounded and all real:
    //   (a) the board is already past its last overlay — i.e. inside the risk
    //       `VolaSession::build`. Trunk's `parallel_for`/`parallel_for_dynamic`
    //       resolve their width ONCE at entry and never re-ask, so a build that
    //       started at width 1 stays at width 1 for its whole duration however
    //       long the pool has been idle. Finer (per-inner-task) reclaim needs an
    //       ELASTIC fan-out primitive, which does not exist on this trunk; the
    //       one that does exist lives on the unmerged `feat/pipeline-t` branch,
    //       and duplicating it here would collide with that branch on a shared
    //       concurrency primitive. That is the reason the granularity here is a
    //       build request rather than an inner task.
    //   (b) the fallback ladder retries a build: the overlay is contractually
    //       applied once per request and a ladder rung does not re-invoke it, so
    //       a rung inherits the width its request resolved.
    //   (c) `inner_budget <= 1` (the documented outer-serial mode): every offer
    //       is 0 = auto and the reclaim is a no-op by construction.
    //
    // ── RESIDUAL, documented where the reclaim code is read ─────────────────
    // The U2 LPT claim order sorts boards by frame rows DESCENDING, so the single
    // most expensive board is claimed FIRST — while the pool is maximally
    // saturated. Its claim offer, and usually both of its overlay offers, are
    // therefore resolved against a full pool and it commonly runs its ENTIRE fit
    // at width 1. This is the same residual WS-T recorded for the corpus arm
    // (there: the straggler's largest expiry always runs at width 1), one level
    // coarser. It is accepted, not hidden: the reclaim's real yield is the drain
    // tail — every board whose build request lands after the queue has emptied —
    // not the LPT head. Do not read the gate below as a claim about the head.
    slots[pos] = fit_board(board, pc, /*admission=*/nullptr,
                           [&resolved, &board, &offer_inner_fit_workers](SessionInputs &in) {
                             apply_symbol_config(resolved, in);
                             in.fit_workers = offer_inner_fit_workers(board.symbol);
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
  {
    std::jthread fit_runner([&] {
      // C4 wave-2: pin outer workers to the discovered P-cores (byte-identical to
      // the unpinned path — pinning only steers WHICH logical CPU a worker runs on).
      fit_status =
          detail::run_bounded_fit_tasks(fit_positions.size(), worker_budget, fit_task, outer_affinity);
    });

    for (std::size_t r = 0; r < date_ranges.size(); ++r) {
      const DateRange &range = date_ranges[r];
      if (range.skip) {
        continue;
      }
      // Block until every enabled fit in this date has completed. A range with
      // no enabled boards starts at zero and proceeds immediately.
      for (std::size_t left = remaining[r].load(std::memory_order_acquire); left != 0u;
           left = remaining[r].load(std::memory_order_acquire)) {
        remaining[r].wait(left, std::memory_order_acquire);
      }

      const std::size_t range_n = range.end - range.begin;
      const std::string &date = boards[order[range.begin]].date;

      // ── Sequential aggregation: stats + owning storage for the write ────────
      std::vector<std::string> names;     // owning symbol storage (kept alive
      std::vector<PricedSurface> stamped; // across write_partition below)
      names.reserve(range_n);
      stamped.reserve(range_n);
      std::vector<SurfaceArchiveItem> items;
      items.reserve(range_n);

      for (std::size_t pos = range.begin; pos < range.end; ++pos) {
        const CorpusBoard &board = boards[order[pos]];
        SymbolAccum &acc = per_symbol[board.symbol];
        acc.stats.symbol = board.symbol;
        ++acc.stats.n_attempted;

        const SymbolFitConfig &resolved = resolved_cfgs[pos];
        if (!resolved.enabled) {
          ++acc.stats.n_disabled;
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
        }
      }

      if (!items.empty()) {
        const Status w = db.write_partition(date, items);
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
  full_meta.emplace_back("n_dates_written", fmt_u32(s.n_dates_written));

  std::string body;
  body.reserve(s.per_symbol.size() * 64 + 64);
  body += "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band\n";
  for (const PopulateSymbolStats &sym : s.per_symbol) {
    const std::uint32_t used =
        sym.n_attempted > sym.n_disabled ? sym.n_attempted - sym.n_disabled : 0u;
    const std::uint32_t denom = std::max<std::uint32_t>(1u, used);
    const double success_rate = static_cast<double>(sym.n_ok) / static_cast<double>(denom);
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
    body += fmt10(success_rate);
    body += ',';
    body += fmt_nan_aware10(sym.mean_oos_in_band);
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
    SymbolFitConfig c = symbol_config_from_preset(spec.preset);
    if (!spec.index_symbol.empty() && sym == spec.index_symbol) {
      c.pin_curve = true;
      c.curve = CurveConfig{}; // default = the dense index recipe (node_cap 40)
    }
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

  std::vector<CorpusBoard> kept; // boards on the dates that need a (re)write
  for (const auto &[date, idxs] : by_date) {
    const Result<SurfaceArchiveV2> part = db.open_partition(date); // Err(NotFound) if none yet
    std::uint32_t present = 0;
    std::uint32_t to_add = 0;
    for (const std::size_t i : idxs) {
      const bool in_db = part.has_value() && part->find(boards[i].symbol).has_value();
      if (in_db) {
        ++present;
      } else {
        ++to_add;
      }
    }
    if (to_add == 0u) {
      ++cov.dates_skipped_complete; // every loaded cell already present
      cov.cells_already_present += present;
      continue;
    }
    // Guard: the partition holds a symbol not in this run's loaded set (present <
    // total) — a whole-partition rewrite from `idxs` alone would drop it. Skip.
    if (part.has_value() && present < part->count()) {
      ++cov.dates_skipped_would_drop;
      cov.cells_already_present += present;
      continue;
    }
    ++cov.dates_written;
    cov.cells_to_fit += to_add;
    cov.cells_refit += present;
    for (const std::size_t i : idxs) {
      kept.push_back(boards[i]);
    }
  }

  // 3. Fuse fit->serialize->release over the needing-work dates via the streaming
  //    populate (executor fan-out, RSS O(dates in flight)). skip_existing is off:
  //    the cell-aware filter above already chose exactly the dates to rewrite.
  if (!kept.empty()) {
    SurfaceDbPopulateConfig pcfg;
    pcfg.fallback = symbol_config_from_preset(spec.preset);
    pcfg.n_threads = spec.fit_workers;
    pcfg.skip_existing = false;
    const Result<SurfaceDbPopulateStats> st = populate_surface_db(db, kept, pcfg, test_hooks);
    if (!st) {
      return Err(st.error());
    }
    cov.cells_ok = st->n_ok;
    cov.cells_failed = st->n_failed;
    cov.per_symbol = std::move(st->per_symbol);
  }

  return Ok(std::move(cov));
}

} // namespace atx::vol

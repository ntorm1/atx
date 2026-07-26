#include "atx/vol/surface_db_populate.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator> // make_move_iterator (FIX-E disabled-carry append)
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
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
  std::vector<std::size_t> remaining(date_ranges.size(), 0u);
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
    remaining[r] = enabled_in_range;
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
  // One wait domain covers BOTH kinds of progress that can unblock the
  // date-ordered drain: a board completes, or the scheduler returns before
  // completing every board. Keeping counter mutation under the same mutex as
  // the wait predicate prevents a notification from being lost between
  // predicate evaluation and sleeping.
  std::mutex progress_mu;
  std::condition_variable progress_cv;
  bool scheduler_complete = false;
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
    std::size_t &date_remaining = remaining[fit_task_range[task_index]];
    // Mark this board done -- and wake the drain when its date's last board
    // finishes -- on scope exit, so a completed date drains and releases even
    // if fit_board throws (a default FitSlot then reads as a failed fit). The
    // decrement runs after the slot write under `progress_mu`, so a drain that
    // observes zero under the same mutex sees every completed slot for the date.
    // This is what streams writes and bounds peak RSS.
    //
    // FIX-4: the same scope exit retires this board from `boards_outstanding`,
    // which is what makes a still-running straggler's next offer wider. The
    // outstanding decrement happens BEFORE the per-date one so that a test woken
    // by `on_board_fit_done` already observes the reclaimed count. The hook is
    // called inside try/catch: this runs from a destructor, and an escaping
    // exception would std::terminate.
    struct MarkDone {
      std::size_t &counter;
      std::atomic<std::size_t> &outstanding;
      std::mutex &progress_mu;
      std::condition_variable &progress_cv;
      const PopulateTestHooks *hooks;
      ~MarkDone() {
        const std::size_t left = outstanding.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (hooks != nullptr && hooks->on_board_fit_done) {
          try {
            hooks->on_board_fit_done(left);
          } catch (...) { // NOLINT: a throwing test hook must not terminate
          }
        }
        {
          std::lock_guard<std::mutex> lock(progress_mu);
          --counter;
        }
        progress_cv.notify_all();
      }
    } mark_done{date_remaining, boards_outstanding, progress_mu, progress_cv, test_hooks};

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
  bool scheduler_ended_with_unfinished_tasks = false;
  {
    detail::FitSchedulerTestHooks scheduler_hooks;
    const detail::FitSchedulerTestHooks *scheduler_hooks_ptr = nullptr;
    try {
      if (test_hooks != nullptr) {
        const auto before_launch = test_hooks->before_worker_launch;
        const auto before_fit_launch = test_hooks->before_fit_worker_launch;
        if (before_launch || before_fit_launch) {
          scheduler_hooks.before_worker_launch =
              [before_launch, before_fit_launch](std::size_t worker_ordinal) {
                if (before_launch) {
                  before_launch(worker_ordinal);
                }
                if (before_fit_launch) {
                  before_fit_launch(worker_ordinal);
                }
              };
        }
        scheduler_hooks.before_setup = test_hooks->before_scheduler_setup;
        if (scheduler_hooks.before_worker_launch || scheduler_hooks.before_setup) {
          scheduler_hooks_ptr = &scheduler_hooks;
        }
      }
    } catch (...) {
      return Err(ErrorCode::Internal, "populate_surface_db: scheduler hook setup failed");
    }

    std::jthread fit_runner;
    try {
      fit_runner = std::jthread([&] {
        // C4 wave-2: pin outer workers to the discovered P-cores
        // (byte-identical to the unpinned path — pinning only steers WHICH
        // logical CPU a worker runs on).
        Status completed = detail::run_bounded_fit_tasks(
            fit_positions.size(), worker_budget, fit_task, outer_affinity, scheduler_hooks_ptr);
        {
          std::lock_guard<std::mutex> lock(progress_mu);
          fit_status = std::move(completed);
          scheduler_complete = true;
        }
        // Wake every date waiter even when allocation/setup/transactional
        // worker launch failed before a single fit_task entered MarkDone.
        progress_cv.notify_all();
      });
    } catch (...) {
      return Err(ErrorCode::Internal, "populate_surface_db: fit-runner launch failed");
    }

    for (std::size_t r = 0; r < date_ranges.size(); ++r) {
      const DateRange &range = date_ranges[r];
      if (range.skip) {
        continue;
      }
      // Block until every enabled fit in this date completes OR the scheduler
      // returns. The second condition is essential: setup/launch failure can
      // return without invoking any task, so no MarkDone exists to retire the
      // pre-counted work. In that case stop draining and surface the bounded
      // scheduler error after the fit-runner joins.
      {
        std::unique_lock<std::mutex> lock(progress_mu);
        progress_cv.wait(lock, [&] { return remaining[r] == 0u || scheduler_complete; });
        if (remaining[r] != 0u) {
          scheduler_ended_with_unfinished_tasks = true;
          break;
        }
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

      // Safe rewrites are a record merge. Open by FILE (not manifest) so an
      // unlisted-but-present partition is retained rather than overwritten. A
      // requested carry also needs the old generation even in destructive mode.
      // The archive is opened only when this date reaches the drain, preserving
      // the one-old-partition-at-a-time streaming RSS bound.
      std::optional<SurfaceArchiveV2> existing;
      if (!cfg.destructive_rewrite || carry_n > 0u) {
        Result<SurfaceArchiveV2> opened = db.open_partition_file(date);
        if (opened.has_value()) {
          existing.emplace(std::move(*opened));
        } else if (opened.error().code() != ErrorCode::NotFound || carry_n > 0u) {
          return Err(opened.error());
        }
      }

      std::vector<std::string> names;     // owning symbol storage (kept alive
      std::vector<PricedSurface> stamped; // across write_partition below)
      const std::size_t output_cap =
          range_n + carry_n + (existing.has_value() ? existing->count() : 0u);
      names.reserve(output_cap);
      stamped.reserve(output_cap);
      std::vector<SurfaceArchiveItem> items;
      items.reserve(output_cap);
      // Every symbol already appended to `items`: successful replacements and
      // explicit carries. The safe merge uses this set to append each remaining
      // old record exactly once.
      std::set<std::string, std::less<>> emitted_symbols;
      bool retained_unreplaced = false;

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
        for (const std::string &sym : carry_it->second) {
          const Result<ArchiveV2DirEntry> entry = existing->find(sym);
          if (!entry) {
            return Err(entry.error());
          }
          Result<ArchivedSurface> got = existing->reconstruct_entry(*entry);
          if (!got) {
            return Err(got.error());
          }
          names.push_back(sym);
          stamped.push_back(std::move(got->surface));
          items.push_back(SurfaceArchiveItem{names.back(), &stamped.back(), got->provenance});
          emitted_symbols.insert(detail::canonicalize_symbol(sym, kArchiveSymbolMax));
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
          emitted_symbols.insert(
              detail::canonicalize_symbol(board.symbol, kArchiveSymbolMax));
        } else {
          ++acc.stats.n_failed;
          ++stats.n_failed;
          // C-10: the failed replacement is reported, but safe mode appends the
          // old byte-identical record during the merge below. Destructive mode
          // intentionally omits it and the coverage audit records the deletion.
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

      // C-10 safe default: merge every prior record that did not get a
      // successful replacement (or an explicit carry already appended above).
      // Failed/disabled refits and symbols absent from the incoming board set
      // therefore retain their exact serialized surface and provenance.
      if (!cfg.destructive_rewrite && existing.has_value() && !items.empty()) {
        for (const ArchiveV2DirEntry &entry : existing->entries()) {
          const std::string symbol(entry.symbol, static_cast<std::size_t>(entry.symbol_len));
          if (emitted_symbols.contains(symbol)) {
            continue;
          }
          Result<ArchivedSurface> retained = existing->reconstruct_entry(entry);
          if (!retained) {
            return Err(retained.error());
          }
          names.push_back(symbol);
          stamped.push_back(std::move(retained->surface));
          items.push_back(
              SurfaceArchiveItem{names.back(), &stamped.back(), retained->provenance});
          emitted_symbols.insert(symbol);
          retained_unreplaced = true;
        }
      }

      // ── REV-R3 (review C-02 / F-02): refuse a write that DESTROYS a stored
      // surface ────────────────────────────────────────────────────────────────
      //
      // A partition write is WHOLE-FILE -- tmp + rename, no merge, no soft-delete
      // -- so the committed file is EXACTLY `items`, and every symbol the existing
      // partition holds that is missing from `items` is destroyed by the commit.
      // FIX-E and the filter's `present < part->count()` each closed one route
      // into that state; the route neither can see is a loaded, ENABLED,
      // already-stored cell whose RE-FIT FAILS. Nothing is appended for it (the
      // `n_failed` branch above), the filter had already counted it into `present`
      // before its outcome existed, and the rewrite simply does not contain it.
      // One production-shaped run at the wrong `--r` destroyed 95 stored surfaces
      // exactly this way and reported success; the format keeps no tombstone, so
      // a destroyed cell is byte-for-byte a cell that was never fitted and nothing
      // on disk recorded the loss.
      //
      // So the question is asked HERE, where the candidate really exists, and as a
      // SET comparison rather than the filter's count comparison -- counts are
      // what made the earlier guard blind. The EXISTING side is read from the
      // partition's own DIRECTORY (FIX-H's precedent: the file is the authority on
      // what the file holds), never inferred from the manifest, which can disagree
      // with it and is exactly the disagreement that would matter here. REV-R3
      // fix-1 (review I-1) made that true of the EXISTENCE decision as well, not
      // just the contents: `open_partition_file` skips the manifest lookup
      // entirely, so a partition file that is on disk but unlisted -- a crash
      // between `write_partition`'s archive rename and its manifest persist, a
      // restored older manifest, a hand-assembled root -- is read and compared
      // instead of being mistaken for a first write and overwritten. Until that
      // fix this comment was a false guarantee: the contents came from the
      // directory but "is there anything here at all" came from the manifest,
      // which is the one question the disagreement actually decides.
      //
      // WHY IT DOES NOT FIRE ON A HEALTHY RUN. It is a SUPERSET test, not an
      // equality test, so adding cells is always allowed. A cell that permanently
      // fails to fit was never stored, so it is not in the existing set and cannot
      // be missed from the candidate -- the converged production database's
      // residual failures are precisely that shape (measured: `cells_refit 0`,
      // `cells_carried 150`, 9 permanently-absent cells, guard silent). It fires
      // only when a cell that WAS stored is absent from the candidate.
      //
      // A refusal is PER-DATE and is NOT an Err: later dates in the same run are
      // unaffected, and the caller gets counters plus the named cells. It is also
      // structurally downstream of the scheduler-abort sentinel above -- an aborted
      // date `return`s before this line, so it can never be misreported as a
      // coverage regression; that failure is an error and the error path owns it.
      //
      // DETERMINISM: this runs on the SINGLE drain thread, inside the
      // date-ascending walk, over an `items` vector the same walk built in
      // (date, symbol) order. Both sides are SORTED before the compare and the
      // difference is a `std::set_difference` of sorted ranges, so the refusal
      // decision and the reported cell order are byte-identical for any worker
      // count. No unordered container is involved.
      //
      // SCOPE: the check is gated on `!items.empty()` because an EMPTY candidate
      // writes no partition at all (the pre-existing `if (!items.empty())` below),
      // so the stored surfaces survive untouched with or without this guard. There
      // is no commit to refuse. That shape is already non-silent by another route
      // -- a date that produced nothing has no `cells_ok`, so the CLI's
      // `is_total_fit_failure` / `is_carry_masked_fit_failure` verdicts speak for
      // it. Do not "tidy" this by hoisting the check above that condition without
      // re-deciding what a refusal COUNT would then mean for a date nobody was
      // going to write.
      std::vector<std::string> lost_symbols; // stored here, absent from `items`
      if (!items.empty()) {
        std::vector<std::string> stored_symbols;
        {
          // REV-R3 fix-1 (review I-1): `open_partition_file`, NOT
          // `open_partition`. The latter consults the manifest FIRST and returns
          // NotFound when the key is unlisted, without ever touching the file --
          // so a partition file holding surfaces but missing from the manifest
          // came back as "no existing coverage" and was overwritten, which is the
          // exact outcome this guard exists to prevent.
          //
          // What the branches below DO, case by case. Only the first proceeds,
          // and it is the only one on which anything here has been told the file
          // is absent:
          //   - NotFound                       -> proceed; the probe was asked
          //                                       with an error_code, reported no
          //                                       error, and reported no file.
          //   - Ok                             -> its directory is the existing
          //                                       set, whatever the manifest says.
          //   - IoError / ParseError / anything -> abort (below). This covers BOTH
          //     else                              "the file is there and will not
          //                                       read" AND "the filesystem
          //                                       declined to say whether it is
          //                                       there at all".
          //
          // REV-R3 fix-2 (review N-1) is that last merge. The probe used to fold
          // a FAILED existence query into NotFound -- a denied ACL on the file or
          // on partitions/, a transient volume or SMB fault -- so an unanswerable
          // filesystem took the proceed-and-overwrite branch. The branch below
          // keys on `!= NotFound` and therefore needed no change; what changed is
          // that `open_partition_file` no longer reports an unanswered question
          // as an answer. Do not "simplify" this by going back to
          // `SurfaceArchiveV2::open_file` directly: that is where the fold is.
          const Result<SurfaceArchiveV2> part = db.open_partition_file(date);
          if (part.has_value()) {
            stored_symbols.reserve(part->directory().size());
            for (const ArchiveV2DirEntry &e : part->directory()) {
              stored_symbols.emplace_back(e.symbol, e.symbol_len);
            }
          } else if (part.error().code() != ErrorCode::NotFound) {
            // A date with NO partition FILE cannot lose coverage. That covers the
            // ordinary first-write path AND documented remedy #1 in
            // surface_db.hpp ("delete the partition file and re-run"): the
            // operator removed the file, so there is nothing on disk to destroy
            // and the rebuild proceeds exactly as before this guard existed.
            // A partition file that IS there and will not OPEN is different in
            // kind: its contents are unknown, so whether the write destroys
            // anything is unanswerable -- and overwriting it is the one action
            // that makes the answer unrecoverable. Fail loud, for the same reason
            // FIX-F fails loud on an unreadable carry record.
            //
            // REV-R3 fix-1 (review I-3): this abort is UNCONDITIONAL. It used to
            // be waived by `allow_coverage_regression`, which fused two unrelated
            // waivers onto one whole-run flag: "I have read the named list and I
            // want THOSE surfaces gone" says nothing about "and please also
            // overwrite any partition you could not parse". A caller who
            // authorised the destruction of a NAMED list has not answered a
            // question about contents nobody could read, and the realistic user
            // of the flag -- retiring one cell after a marginal re-fit failure --
            // would have been silently opted into the second waiver for every
            // date in the run. The remedy for a genuinely corrupt partition is
            // the same as it has always been: delete the file (remedy #1), which
            // turns this into the NotFound path above.
            return Err(part.error());
          }
        }
        std::sort(stored_symbols.begin(), stored_symbols.end());

        // The candidate side, in the form the archive actually stores: the writer
        // canonicalizes every item's symbol before hashing/storing it, so an
        // uncanonicalized compare would report a spurious drop for any item whose
        // board symbol differs from its stored key by case or length.
        //
        // REV-R3 fix-1 (review M-3): canonicalized under `kArchiveSymbolMax`, the
        // ARCHIVE's bound, because the directory entries being compared against
        // were written under it (`surface_archive.cpp`'s writer). It was
        // `kSurfaceDbKeyMax` -- the db's partition-KEY bound -- which is a
        // different constant that happens to hold the same value. The
        // static_assert keeps the coincidence honest for the OTHER direction: the
        // `CoverageRegressionCell::symbol` values produced here are documented as
        // the canonical db key, and every other symbol canonicalization on this
        // path (write_partition's fingerprint fold, the carry-over key match)
        // uses `kSurfaceDbKeyMax`. If the two ever diverge, this line must be
        // re-decided rather than silently picking a side: too small a bound
        // misses a real drop, too large a one refuses every rewrite of a date
        // holding a long symbol.
        static_assert(kArchiveSymbolMax == kSurfaceDbKeyMax,
                      "the coverage guard compares CANDIDATE symbols against the ARCHIVE "
                      "directory's keys; if the archive's and the db's symbol bounds diverge, "
                      "this comparison and CoverageRegressionCell's documented key form must "
                      "be re-decided, not silently truncated to one of the two");
        std::vector<std::string> candidate_symbols;
        candidate_symbols.reserve(items.size());
        for (const SurfaceArchiveItem &item : items) {
          candidate_symbols.push_back(
              detail::canonicalize_symbol(item.symbol, kArchiveSymbolMax));
        }
        std::sort(candidate_symbols.begin(), candidate_symbols.end());

        std::set_difference(stored_symbols.begin(), stored_symbols.end(),
                            candidate_symbols.begin(), candidate_symbols.end(),
                            std::back_inserter(lost_symbols));
      }

      // Detection runs on BOTH sides of the opt-out on purpose. A retirement run
      // still gets a complete, ordered record of every surface it destroyed --
      // which is the one thing the 95-surface incident had no way to produce.
      // `destructive_rewrite` defines the candidate shape; it does not by
      // itself authorize destroying stored coverage. Authorization remains the
      // separate, auditable `allow_coverage_regression` decision.
      const bool destructive_authorized = cfg.allow_coverage_regression;
      if (!lost_symbols.empty()) {
        for (const std::string &sym : lost_symbols) {
          stats.coverage_regression_cells.push_back(CoverageRegressionCell{date, sym});
        }
        if (destructive_authorized) {
          ++stats.n_dates_dropped_coverage_regression;
        } else {
          ++stats.n_dates_refused_coverage_regression;
          // REV-R3 fix-2 (review N-3). WHY the rewrite would have lost coverage
          // decides what the operator should do about it, and the two causes want
          // opposite actions. The wrong-`--r` incident the banner was written for
          // is a FIT problem: the cells really did fail, the fix is the build
          // input. A partition that is on disk but UNLISTED is a manifest/file
          // DISAGREEMENT: nothing failed, the run was simply narrower than a file
          // the index does not know about, and it will be refused on every run
          // forever until the file is deleted or the date is rebuilt over the
          // full board set. Sending that operator to check `--r` sends them at
          // the wrong problem, and the escape the banner offers for the fit case
          // (`--allow-coverage-regression`) would delete the surfaces.
          //
          // We are in the branch where `open_partition_file` SUCCEEDED (a
          // non-empty `lost_symbols` requires a non-empty stored set), so the
          // file is known present; `partition_listed` is a pure manifest
          // snapshot lookup with no file I/O, and it is asked only on a date that
          // is already being refused. DETERMINISM: drain thread, same
          // date-ascending walk, snapshot read, no atomic, no unordered
          // iteration.
          if (!db.partition_listed(date)) {
            ++stats.n_dates_refused_partition_unlisted;
          }
        }
      }
      const bool refuse_write = !lost_symbols.empty() && !destructive_authorized;

      if (!items.empty() && !refuse_write) {
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
        // A failed/absent replacement retained from the old generation was not
        // produced or fingerprint-gated by this run. Do not re-bless it under the
        // current config: the zero/unknown fold forces a later resume to retry it.
        const DbConfigAttestation date_attest =
            retained_unreplaced ? DbConfigAttestation::None : cfg.attest;
        const Status w = db.write_partition(date, items, {}, date_attest);
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
  if (scheduler_ended_with_unfinished_tasks && fit_status) {
    return Err(ErrorCode::Internal,
               "populate_surface_db: scheduler returned before completing every fit task");
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
  //    board adds a symbol the partition does not already carry. A same-date
  //    rewrite re-fits already-present incoming cells; populate_surface_db merges
  //    successful fits with the old partition, so a failed refit or a symbol
  //    absent from this run remains intact unless destructive mode was explicit.
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
        } else if (carry_valid && !boards[i].frame.rows.empty()) {
          // A structurally empty incoming board is an explicit failed-refit
          // shape, not a healthy resume candidate. Let it reach the fitter so
          // C-10's safe merge can report the failure and retain the old record.
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
    // REV-R3: the operator's retirement opt-out, forwarded verbatim.
    pcfg.allow_coverage_regression = spec.allow_coverage_regression;
    pcfg.destructive_rewrite = spec.destructive_rewrite;
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
    // ── REV-R3: what the WRITE path refused, and the correction it forces ──────
    // `dates_written` was incremented by the filter above, which decides which
    // dates to REWRITE -- an intention, not a commit. Take the populate's own
    // count instead: `n_dates_written` is incremented at the write site, after
    // `write_partition` succeeded, so it is the number of dates that really
    // landed on disk.
    //
    // REV-R3 fix-1 (review M-2). This was a SUBTRACTION of the refusals, which
    // left a second, older overcount in place: a date whose candidate ended up
    // EMPTY writes no partition and is not a refusal either (the guard is gated
    // on `!items.empty()`), so the filter's increment stood for a commit that
    // never happened. That is the exact class of lie this guard exists to end,
    // and it contradicted what this comment, the header's `dates_written`, and
    // the manual's counter table all now claim. Assignment also removes the
    // underflow question rather than arguing it away.
    cov.dates_refused_coverage_regression = st->n_dates_refused_coverage_regression;
    cov.dates_refused_partition_unlisted = st->n_dates_refused_partition_unlisted;
    cov.dates_dropped_coverage_regression = st->n_dates_dropped_coverage_regression;
    cov.dates_written = st->n_dates_written;
    cov.coverage_regression_cells = st->coverage_regression_cells;
    cov.per_symbol = std::move(st->per_symbol);
    // The per-cell reasons ride along with the count they explain; the populate
    // already ordered them by (date, symbol), so nothing re-sorts here.
    cov.failed_cells = st->failed_cells;
  }

  return Ok(std::move(cov));
}

} // namespace atx::vol

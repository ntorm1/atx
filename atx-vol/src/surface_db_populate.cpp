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
    // Seeding recipe shared bit-for-bit with generate_symbol_configs (Task 4):
    // the preset's config, dense-index-pinned for the index leg.
    const SymbolFitConfig c = seed_symbol_config(sym, spec.preset, spec.index_symbol);
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
  for (const auto &[date, idxs] : by_date) {
    const Result<SurfaceArchiveV2> part = db.open_partition(date); // Err(NotFound) if none yet
    std::uint32_t present = 0;
    std::uint32_t to_add = 0;
    for (const std::size_t i : idxs) {
      const bool in_db = part.has_value() && part->find(boards[i].symbol).has_value();
      if (in_db) {
        ++present;
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
    pcfg.fallback = fallback_cfg;
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

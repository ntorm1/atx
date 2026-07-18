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
      const Result<SurfaceArchive> existing = db.open_partition(date);
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

  // SurfaceDb defines n_threads=0 as outer-serial. A real multi-board outer
  // fan-out pins each fit to one worker to avoid nested H^2 parallelism.
  const unsigned worker_budget = cfg.n_threads != 0u ? cfg.n_threads : 1u;
  const bool parallel_outer = fit_positions.size() > 1u && worker_budget > 1u;

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
    if (parallel_outer) {
      pc.fit_workers = 1u;
    }
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
      fit_status = detail::run_bounded_fit_tasks(fit_positions.size(), worker_budget, fit_task);
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

} // namespace atx::vol

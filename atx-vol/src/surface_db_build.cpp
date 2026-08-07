#include "atx/vol/tools/surface_db_build.hpp"

#include <algorithm>
#include <chrono> // coarse build-phase wall clock (diagnostic only)
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/chain.hpp"          // OptionChain (board -> Underlying, corpus_board_fit path)
#include "atx/vol/curve_selector.hpp" // select_curve, production_selector_config, SelectorResult
#include "atx/vol/fit_policy.hpp"     // select_fit_policy, FitDecision
#include "atx/vol/opra_batch.hpp"     // corpus_board_from_opra, OpraBatchEntry/Result
#include "atx/vol/opra_hive.hpp"      // load_opra_hive, OpraHiveSpec
#include "atx/vol/pricer_fitter.hpp"  // ExpiryBuildOutcome (Task 3 slice_drop rows)
#include "atx/vol/session.hpp"        // make_session_inputs, SessionInputs
#include "atx/vol/tools/surface_db_populate.hpp" // populate_universe_streaming, UniversePopulateSpec
#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs
#include "atx/vol/surface_policy.hpp" // has_output, SurfacePurpose (LinearVariance guard)
#include "atx/vol/universe.hpp"       // Underlying, Chain
#include "atx/vol/vol_curve.hpp"      // CurveConfig (dense index recipe)
#include "surface_db_seed.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

SymbolFitConfig seed_symbol_config(std::string_view symbol, FitPreset preset,
                                   std::string_view index_symbol, bool pin_curve_family) {
  SymbolFitConfig cfg = symbol_config_from_preset(preset);
  if (!index_symbol.empty() && symbol == index_symbol) {
    // The recipe is recorded either way; `pin_curve` decides whether it is a hard
    // pin (one attempt, no recovery) or the family the auto route starts from
    // with PricerFitter's two fallback ladders still live. A hard pin on this
    // (ConvexDense) family remains eligible for the strict same-family
    // admission-recovery refit; a hard pin naming any other family gets none.
    cfg.pin_curve = pin_curve_family;
    cfg.curve = CurveConfig{}; // default = the dense index recipe (node_cap 40)
  }
  return cfg;
}

namespace {

// Seconds elapsed since `begin` on the steady clock. Used only to fill the
// diagnostic `SurfaceDbBuildReport::t_*_s` fields.
[[nodiscard]] double phase_seconds_since(std::chrono::steady_clock::time_point begin) noexcept {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

// A board can be selected on only when its underlying carries at least one
// expiry with >= 2 strikes: a single point cannot pin or select a curve (the
// held-out selector has nothing to hold out, and a pinned family has no smile).
// A board gutted below this is a fail-closed selection failure.
[[nodiscard]] bool underlying_selectable(const Underlying &under) noexcept {
  for (const Chain &c : under.chains) {
    if (c.n_strikes() >= 2u) {
      return true;
    }
  }
  return false;
}

// Lower the board's chain to the market/pricing context `select_curve` consumes,
// mirroring PricerFitter::fit's SessionInputs assembly (make_session_inputs +
// per-expiry term rates + dividends), then map it onto SurfaceParityInputs
// exactly as pricer_fitter.cpp does before its own select_curve call.
[[nodiscard]] SurfaceParityInputs parity_inputs_from_chain(const OptionChain &chain,
                                                           FitPreset preset) {
  SessionInputs in = make_session_inputs(preset, chain.spot(), chain.rate(), chain.now_ns());
  const Underlying &under = chain.underlying();
  if (chain.env().yield.size() > 0u) {
    in.expiry_rate_T.reserve(under.chains.size());
    in.expiry_rates.reserve(under.chains.size());
    for (const Chain &expiry : under.chains) {
      in.expiry_rate_T.push_back(expiry.T);
      in.expiry_rates.push_back(chain.env().rate_at(expiry.T));
    }
  }
  in.cash_divs = chain.env().cash_divs;

  SurfaceParityInputs sp;
  sp.S = in.S;
  sp.r = in.r;
  sp.expiry_rate_T = in.expiry_rate_T;
  sp.expiry_rates = in.expiry_rates;
  sp.cash_divs = in.cash_divs;
  sp.now_ts_ns = in.now_ts_ns;
  sp.deam = in.deam;
  sp.calib = in.calib;
  sp.band_k = in.band_k;
  sp.repair = in.calendar_repair;
  sp.fit_workers = 1u; // single board; never nest a fan-out here
  sp.score_parity = in.score_parity;
  sp.enforce_calendar_floor = in.enforce_calendar_floor;
  sp.use_deam_cache_for_fit = in.use_deam_cache_for_fit;
  return sp;
}

// Produce the per-symbol config for one board, or return false when selection
// failed (fail-closed: the caller stores a disabled config). Never throws.
[[nodiscard]] bool config_for_board(const CorpusBoard &board, const AutoConfigSpec &spec,
                                    SymbolFitConfig &out) noexcept {
  try {
    Result<OptionChain> chain = OptionChain::from_frame(board.frame, board.env);
    if (!chain.has_value()) {
      return false;
    }
    const Underlying &under = chain->underlying();
    if (!underlying_selectable(under)) {
      return false;
    }

    // Board-feature classification: chooses the curve FAMILY. The numerical
    // preset tier stays the operator-selected `spec.preset` (matching
    // populate_universe_streaming's seeding and the StoresConfigPerSymbol
    // contract); the policy decides the family, not the tier.
    //
    // `pin_curve` is the operator's call, not this stage's: pinned, the fit gets
    // exactly one family attempt and PricerFitter's construction-failure and
    // admission-rejection ladders are both dead for the symbol; unpinned
    // (default), the family below is a recorded preference and the fit
    // auto-routes with both ladders live.
    const FitDecision decision = select_fit_policy(under, board.symbol, board.fit_context, {});
    SymbolFitConfig cfg = symbol_config_from_preset(spec.preset);
    cfg.pin_curve = spec.pin_curve_family;
    cfg.curve = decision.curve;

    if (spec.deep_selection) {
      const SurfaceParityInputs sp = parity_inputs_from_chain(*chain, cfg.preset);
      const Result<SelectorResult> selected =
          select_curve(under, sp, production_selector_config());
      if (selected.has_value()) {
        cfg.curve = selected->chosen; // the held-out winner replaces the policy's
      } else {
        const ErrorCode code = selected.error().code();
        // NotFound (no scorable holdout) / Unavailable (budget) are not defects:
        // fall back to the fit-policy decision curve already recorded above. Any
        // other error (e.g. InvalidArgument) is a hard selection failure.
        if (code != ErrorCode::NotFound && code != ErrorCode::Unavailable) {
          return false;
        }
      }
    }

    // A PINNED LinearVariance config is a guaranteed 100% cell loss, not a fit
    // risk: the RISK pipeline's input validation refuses it outright
    // (`cfg_.curve->kind == VolCurveKind::LinearVariance` -> hard InvalidArgument,
    // "invalid correctness policy for requested risk surface", pricer_fitter.cpp),
    // so EVERY cell of the symbol fails identically, every run. Reachable here for
    // any ultra-liquid index/ETF name that is not the one `--index` slot
    // (`select_fit_policy`: IndexEtfUltraLiquid -> LinearVariance). Fail closed at
    // CONFIG time so the symbol is named in `failed_symbols` instead of showing up
    // as an unexplained `cells_failed` count.
    //
    // Both conjuncts narrow the guard to exactly the case that is fatal, because
    // over-rejecting here silently loses cells that would have fitted:
    //   - `pin_curve`: unpinned, the auto route substitutes ConvexDense for a
    //     LinearVariance decision, so the family is harmless.
    //   - a Risk output: the clause above lives INSIDE the risk build, which
    //     `pricer_fitter.cpp`'s mark-only early return skips entirely when the
    //     request carries no Risk purpose — and that mark path pins
    //     LinearVariance itself, so it is the family's home, not its grave.
    //     `symbol_config_from_preset(FitPreset::Hft)` maps to MarketMark-only
    //     (`map_legacy_fit_preset`), so without this conjunct
    //     `--preset hft --pin-curve-family true` would disable every
    //     IndexEtfUltraLiquid-profiled symbol for a fit that would have succeeded.
    if (cfg.pin_curve && cfg.curve.kind == VolCurveKind::LinearVariance &&
        has_output(cfg.surface_policy.outputs, SurfacePurpose::Risk)) {
      return false;
    }

    out = std::move(cfg);
    return true;
  } catch (...) {
    // A selection helper must never let an exception escape the per-symbol loop
    // (e.g. bad_alloc from a fit); record the symbol as a fail-closed disable.
    return false;
  }
}

} // namespace

Result<AutoConfigReport> generate_symbol_configs(SurfaceDb &db, std::span<const CorpusBoard> boards,
                                                 const AutoConfigSpec &spec) {
  // Pick one config board per distinct symbol: the `spec.config_date` board when
  // present, else the symbol's earliest board. std::map keys by symbol so the
  // walk is deterministic (canonical-symbol order) and re-runnable.
  std::map<std::string, const CorpusBoard *> chosen;
  for (const CorpusBoard &board : boards) {
    const CorpusBoard *&slot = chosen[board.symbol];
    if (slot == nullptr) {
      slot = &board;
      continue;
    }
    const bool board_is_config_date = !spec.config_date.empty() && board.date == spec.config_date;
    const bool slot_is_config_date = !spec.config_date.empty() && slot->date == spec.config_date;
    if (board_is_config_date && !slot_is_config_date) {
      slot = &board; // prefer the requested config date
    } else if (board_is_config_date == slot_is_config_date && board.date < slot->date) {
      slot = &board; // tie on config-date match -> earliest date wins
    }
  }

  AutoConfigReport report;
  // Selected configs accumulate here and land in ONE upsert_symbols batch after
  // the loop: the per-symbol upsert re-encoded and atomically rewrote the whole
  // manifest once per symbol — O(N^2) manifest bytes on a fresh N-symbol build.
  // Each entry's string_view references the `chosen` map key, which outlives the
  // batch call. The batch is also all-or-nothing, so a failed write no longer
  // leaves a partially-configured manifest behind.
  std::vector<DbSymbolEntry> pending;
  pending.reserve(chosen.size());
  for (const auto &[symbol, board_ptr] : chosen) {
    ++report.n_symbols;

    // Idempotent by default: an already-configured symbol is left untouched so a
    // re-run never clobbers an operator override.
    //
    // FIX-C-2. A stored DISABLED config is the one existing config a skip must not
    // swallow silently. It is a standing failure, not a settled state: no NEW cell
    // will ever be written for the symbol, yet before this it left no trace after
    // the run that created it (skipped here, and its dates reported
    // `dates_skipped_complete` by the populate). So a skipped disabled symbol is
    // now counted AND named on every run — and, with `retry_disabled`, re-selected
    // instead of skipped, which is the only way a fix to the loader or the hive
    // can ever reach an ALREADY-BUILT database.
    //
    // FIX-E corrected this paragraph's premise. It used to read "the symbol is
    // absent from every partition and will stay absent forever", which holds only
    // for a symbol disabled BEFORE it ever fitted — the case FIX-C-2 was
    // diagnosing. A symbol disabled AFTER it fitted keeps every surface it already
    // produced (and they still load; nothing on the read path gates on `enabled`).
    // The reason that premise LOOKED universal is that the rewrite path was
    // silently deleting those surfaces, which is the defect FIX-E repaired: the
    // invariant is now corrected rather than enforced by destruction.
    if (!spec.overwrite_existing) {
      if (const Result<SymbolFitConfig> existing = db.symbol_config(symbol); existing.has_value()) {
        if (existing->enabled || !spec.retry_disabled) {
          ++report.n_skipped_existing;
          if (!existing->enabled) {
            ++report.n_disabled_existing;
            report.failed_symbols.push_back(symbol);
          }
          continue;
        }
        // else: disabled + retry_disabled => fall through and re-select it as if
        // it were new (the index-leg and config_for_board branches below).
      }
    }

    // The index leg takes the dense recipe (shared with
    // populate_universe_streaming), bypassing per-board selection. Whether that
    // recipe is a hard pin is `spec.pin_curve_family` here too — the index leg is
    // exactly where an unrecovered single attempt cost production cells.
    if (!spec.index_symbol.empty() && symbol == spec.index_symbol) {
      pending.push_back(DbSymbolEntry{
          symbol, seed_symbol_config(symbol, spec.preset, spec.index_symbol, spec.pin_curve_family),
          std::nullopt});
      ++report.n_configured;
      continue;
    }

    SymbolFitConfig cfg;
    if (!config_for_board(*board_ptr, spec, cfg)) {
      // Fail closed: store the preset config DISABLED and record the symbol; the
      // top-level call still succeeds so one bad board never sinks the build.
      cfg = symbol_config_from_preset(spec.preset);
      cfg.enabled = false;
      pending.push_back(DbSymbolEntry{symbol, cfg, std::nullopt});
      ++report.n_disabled_failed;
      report.failed_symbols.push_back(symbol);
      continue;
    }

    pending.push_back(DbSymbolEntry{symbol, cfg, std::nullopt});
    ++report.n_configured;
  }

  if (const Status up = db.upsert_symbols(pending); !up.has_value()) {
    return Err(up.error());
  }

  std::sort(report.failed_symbols.begin(), report.failed_symbols.end());
  return Ok(std::move(report));
}

// ── Stage 2: the one-call build driver + its report CSV ──────────────────────

Result<SurfaceDbBuildReport> build_surface_db(const SurfaceDbBuildSpec &spec) {
  // 1. Create-or-open. Mirror SurfaceDb::open's NotFound probe: a manifest at
  //    the root => open (resume); absent => create. This keeps a re-run of the
  //    build over the same root idempotent (open the db it made last time).
  const std::filesystem::path root_path{spec.db_root};
  const std::filesystem::path manifest_file = root_path / std::string(kSurfaceDbManifestName);
  std::error_code exists_ec;
  const bool manifest_present = std::filesystem::exists(manifest_file, exists_ec);
  if (exists_ec) {
    return Err(ErrorCode::IoError, "build_surface_db: failed to stat db_root");
  }
  Result<SurfaceDb> db_res =
      manifest_present ? SurfaceDb::open(spec.db_root) : SurfaceDb::create(spec.db_root);
  if (!db_res) {
    return Err(db_res.error());
  }
  SurfaceDb db = std::move(*db_res);

  // 2. Load the hive window. A missing/un-pulled window is Ok (no boards); the
  //    ONLY top-level Err here is a malformed hive spec (empty root, bad dates).
  const auto t_load_begin = std::chrono::steady_clock::now();
  Result<OpraBatchResult> loaded = load_opra_hive(spec.hive);
  if (!loaded) {
    return Err(loaded.error());
  }
  const double t_load_s = phase_seconds_since(t_load_begin);

  // 3. One board per SUCCESSFULLY loaded cell; missing/corrupt cells are tallied
  //    and never fit. The date counters are DISTINCT dates (a date is "loaded"
  //    when any of its cells produced a panel, "missing" when none did — so a
  //    per-symbol coverage hole inside a present date is not a missing date);
  //    note the window is enumerated as CALENDAR days, so weekends and holidays
  //    land in n_dates_missing.
  SurfaceDbBuildReport report;
  std::vector<CorpusBoard> boards;
  boards.reserve(loaded->n_loaded);
  std::set<std::string> loaded_dates;
  std::set<std::string> all_dates;
  for (OpraBatchEntry &entry : loaded->entries) {
    all_dates.insert(entry.date);
    if (entry.panel.has_value()) {
      loaded_dates.insert(entry.date);
      boards.push_back(corpus_board_from_opra(entry.date, entry.symbol, std::move(*entry.panel)));
    }
  }
  report.n_dates_loaded = loaded_dates.size();
  report.n_dates_missing = all_dates.size() - loaded_dates.size();
  // Split the loader's erroring cells into "the universe is sparse" and "the data
  // is bad" — taken from the loader's OWN structural classification, never
  // inferred from an error code here (a coverage hole and a wrong-schema file are
  // both InvalidArgument, so a code test would misreport a corrupt date as holes).
  // n_coverage_holes is a sub-count of n_error, so the subtraction cannot wrap.
  report.n_coverage_holes = loaded->n_coverage_holes;
  report.n_load_errors = loaded->n_error - loaded->n_coverage_holes;

  // 4. Auto-generate per-symbol manifest configs from the loaded boards. Runs
  //    BEFORE the populate so its richer per-board family pin is in place; the
  //    populate's own seeding then finds every symbol already configured. An
  //    empty board set is an all-zero report (Ok).
  const auto t_config_begin = std::chrono::steady_clock::now();
  Result<AutoConfigReport> cfg = generate_symbol_configs(db, boards, spec.auto_config);
  if (!cfg) {
    return Err(cfg.error());
  }
  report.config = std::move(*cfg);
  const double t_config_s = phase_seconds_since(t_config_begin);

  // 5. Cell-aware streaming populate. The index leg / preset / worker budget come
  //    from this spec; an empty board set is a graceful all-zero no-op.
  UniversePopulateSpec pspec;
  pspec.index_symbol = spec.auto_config.index_symbol;
  pspec.preset = spec.preset;
  pspec.fit_workers = spec.fit_workers;
  pspec.allow_coverage_regression = spec.allow_coverage_regression; // REV-R3
  const auto t_populate_begin = std::chrono::steady_clock::now();
  Result<UniversePopulateCoverage> cov = populate_universe_streaming(db, boards, pspec);
  if (!cov) {
    return Err(cov.error());
  }
  report.coverage = std::move(*cov);

  report.t_load_s = t_load_s;
  report.t_config_s = t_config_s;
  report.t_populate_s = phase_seconds_since(t_populate_begin);
  return Ok(std::move(report));
}

namespace {

// CSV scalar formatting — an independent small twin of surface_db_populate.cpp's
// fmt helpers (those are TU-private there), keeping this file's report writer
// self-contained rather than coupling the two.
[[nodiscard]] std::string fmt_u32(std::uint32_t v) {
  char buf[16];
  const int len = std::snprintf(buf, sizeof buf, "%u", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

[[nodiscard]] std::string fmt_usize(std::size_t v) {
  char buf[24];
  const int len = std::snprintf(buf, sizeof buf, "%zu", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// Task 3: a year-fraction tenor. %.6f (per the ambiguity resolution this task
// was scoped under) is plenty of precision for anything derived from a
// calendar expiry, and unlike %g it never switches to exponential notation on
// a small value, which would be an odd shape for a `key,value` scalar.
[[nodiscard]] std::string fmt_t(double v) {
  char buf[48];
  const int len = std::snprintf(buf, sizeof buf, "%.6f", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// Task 3: the admission layer's own per-expiry outcome spelling
// (`ExpiryBuildOutcome`, pricer_fitter.hpp), verbatim as the enum name so a
// reader can grep the report for the same word the fitter's own diagnostics
// use (spy_fit_rca.cpp). `Fitted` is unreachable through
// `SliceDropCell::outcome` (FitSlot::slice_drops filters it out before it
// leaves corpus_board_fit.cpp) but is spelled anyway so a hand-built report in
// a test does not fall through to "Unknown" for the one outcome that is not,
// in fact, unknown.
[[nodiscard]] const char *coarse_outcome_name(ExpiryBuildOutcome outcome) noexcept {
  switch (outcome) {
  case ExpiryBuildOutcome::Fitted:
    return "Fitted";
  case ExpiryBuildOutcome::Missing:
    return "Missing";
  case ExpiryBuildOutcome::DuplicateMaturity:
    return "DuplicateMaturity";
  }
  return "Unknown"; // unreachable for a valid enumerator
}

// Fix Round 1 (spec gap). The fit driver's own reason a `Missing` expiry
// never reached `expiries()`/`parity()` -- ONLY the five values the task
// brief's outcome vocabulary actually names
// (`Missing`/`CarryFailed`/`PrepStarved`/`PrepFailed`/`Skipped`/`FitFailed`).
// The three `Fitted*` family values (`Fitted`, `FittedFallbackCurve`,
// `FittedLegacyPrep`) are deliberately NOT drop reasons -- a chain reported
// `Fitted` by the fit driver cannot also be admission-`Missing` by
// construction (both come from the same `context`/`expiry_reports` the
// session retains, see `ExpiryBuildReport::fit_outcome`'s doc) -- so seeing
// one here means the rich report was not usefully aligned (or is the
// `ExpiryFitOutcome::Fitted` default sentinel meaning "no rich reason is
// available") and the caller must fall back to the coarse spelling. Returns
// nullptr for exactly that "fall back" case.
[[nodiscard]] const char *rich_drop_reason_name(ExpiryFitOutcome outcome) noexcept {
  switch (outcome) {
  case ExpiryFitOutcome::CarryFailed:
    return "CarryFailed";
  case ExpiryFitOutcome::PrepStarved:
    return "PrepStarved";
  case ExpiryFitOutcome::PrepFailed:
    return "PrepFailed";
  case ExpiryFitOutcome::FitFailed:
    return "FitFailed";
  case ExpiryFitOutcome::Skipped:
    return "Skipped";
  case ExpiryFitOutcome::PrepUncovered:
    return "PrepUncovered";
  case ExpiryFitOutcome::FitRefusedCalendar:
    return "FitRefusedCalendar";
  case ExpiryFitOutcome::Fitted:
  case ExpiryFitOutcome::FittedFallbackCurve:
  case ExpiryFitOutcome::FittedLegacyPrep:
    return nullptr; // not a drop reason -- fall back to the coarse spelling
  }
  return nullptr; // unreachable for a valid enumerator
}

// The CSV's `outcome` column, exactly as the header comment above the row
// loop promises: the rich fit-driver reason when one is genuinely available
// (`outcome == Missing` and `rich_drop_reason_name` names it), the coarse
// admission-layer spelling otherwise (`Missing` with no rich reason on
// record, or `DuplicateMaturity`, which the fit driver never classifies at
// all -- it is caught at input validation, before any per-chain fit runs).
[[nodiscard]] std::string slice_drop_outcome_name(const SliceDropCell &d) {
  if (d.outcome == ExpiryBuildOutcome::Missing) {
    if (const char *rich = rich_drop_reason_name(d.fit_outcome); rich != nullptr) {
      return rich;
    }
  }
  return coarse_outcome_name(d.outcome);
}

} // namespace

ReportedFailedCells reported_failed_cells(const SurfaceDbBuildReport &r,
                                          std::size_t max_reported) noexcept {
  const std::span<const FailedCell> all{r.coverage.failed_cells};
  const std::size_t shown = std::min(max_reported, all.size());
  return ReportedFailedCells{all.first(shown), all.size() - shown};
}

ReportedCoverageRegressionCells
reported_coverage_regression_cells(const SurfaceDbBuildReport &r,
                                   std::size_t max_reported) noexcept {
  const std::span<const CoverageRegressionCell> all{r.coverage.coverage_regression_cells};
  const std::size_t shown = std::min(max_reported, all.size());
  return ReportedCoverageRegressionCells{all.first(shown), all.size() - shown};
}

std::size_t coverage_regression_display_cap(const SurfaceDbBuildReport &r,
                                            std::size_t max_reported) noexcept {
  // See the header: the presentation cap is waived exactly when this run
  // DESTROYED stored surfaces, because then the printed list is the only record
  // that they existed. Returning the full size (rather than a sentinel) keeps
  // `reported_coverage_regression_cells`' `reported + elided == total` invariant
  // trivially true with `elided == 0`.
  return r.coverage.dates_dropped_coverage_regression > 0u
             ? r.coverage.coverage_regression_cells.size()
             : max_reported;
}

bool is_total_load_failure(const SurfaceDbBuildReport &r) {
  // R1-b (review C-04). Two terms, and the header explains why neither may be
  // dropped or widened:
  //   `n_dates_loaded == 0`  -- NOT ONE date produced a board, so the fit stage
  //                             was handed an empty span and every counter the
  //                             other predicates read is structurally zero.
  //   `n_load_errors > 0`    -- and the reason is REAL DEFECTS, not absence. This
  //                             is the defect-only cell count: coverage holes are
  //                             split out into `n_coverage_holes` by the loader's
  //                             own structural classification, so a healthy sparse
  //                             discover-all build cannot trip this.
  // Together: "every present file in the requested window is unreadable", which
  // no other predicate in this file can see and which used to exit 0.
  return r.n_dates_loaded == 0u && r.n_load_errors > 0u;
}

bool is_total_fit_failure(const SurfaceDbBuildReport &r) {
  // Three conditions, all required (see the header for why none may be dropped):
  // work WAS scheduled, none of it landed, and nothing was CARRIED either.
  // `cells_to_fit == 0` is the resume/empty-window path and stays a success; any
  // `cells_ok > 0` is partial coverage, which is normal production output; any
  // `cells_carried > 0` means the date this run rewrote is full of healthy stored
  // surfaces, so the build produced a populated database whatever the scheduled
  // cells did.
  //
  // The carry clause is FIX-D's. Without it the converged steady state this whole
  // feature exists to produce -- N permanently-failing cells retried forever
  // beside their carried healthy siblings -- reads as (cells_to_fit = N,
  // cells_ok = 0) and trips the alarm on a perfectly healthy database. Before
  // carry-over those siblings were re-fitted, so `cells_ok` was large and the
  // predicate stayed quiet by accident.
  return r.coverage.cells_to_fit > 0 && r.coverage.cells_ok == 0 &&
         r.coverage.cells_carried == 0;
}

bool is_strict_total_fit_failure(const SurfaceDbBuildReport &r) {
  // REV-R4 (review C-05). The predicate above WITHOUT its carry clause -- the
  // literal reading of "this run scheduled work and fitted none of it", which is
  // what an unattended scheduler needs and what the carry exemption gave up.
  //
  // `cells_to_fit > 0` is retained and is what keeps `--strict` meaning
  // "SCHEDULED WORK ALL DIED" rather than "nothing happened": the converged
  // resume, the un-pulled empty window and the carried-only rewrite that lost
  // nothing all schedule zero cells, so none of them can reach this even in
  // strict mode. Dropping that conjunct would make `--strict` fire on every
  // healthy no-op, which is the failure mode this whole exit-code family keeps
  // rediscovering.
  //
  // Only `cells_ok` counts as production. `cells_carried` is deliberately NOT
  // read here (that is the entire difference from `is_total_fit_failure`), and
  // `cells_carried_disabled` is not read by either -- see the header for why the
  // exemption above is nonetheless correct and stays.
  return r.coverage.cells_to_fit > 0 && r.coverage.cells_ok == 0;
}

bool is_carry_masked_fit_failure(const SurfaceDbBuildReport &r) {
  // The complement of the clause above, and the honest accounting of what it gave
  // up (see the header for the full argument). Nothing fitted, something failed,
  // and something healthy was carried -- the shape the carry clause exempts, and
  // the one that is ambiguous between the converged steady state and a run whose
  // every scheduled cell died systematically.
  //
  // This is a WARNING predicate: the CLI prints it and returns 0. Turning it into
  // an exit code would recreate C1 exactly, because the converged steady state
  // over a healthy production database matches it on every run.
  //
  // `cells_carried`, not `cells_carried_disabled`: the first is what grants the
  // exemption, so it is what the warning must track. A run carrying only
  // preserved-because-disabled surfaces is still a TOTAL FIT FAILURE and exits 3.
  //
  // No `cells_to_fit` conjunct, and it would be a NO-OP if there were one (M-B).
  // `cells_carried` is only ever accumulated in the frame that also runs
  // `cov.cells_to_fit += to_add` on a date being written, and that frame is
  // reached only when `to_add > 0` (surface_db_populate.cpp: a date with
  // `to_add == 0` is `dates_skipped_complete` and `continue`s before either
  // counter moves). So `cells_carried > 0` already implies `cells_to_fit > 0` and
  // adding the conjunct could not change this predicate's value on any reachable
  // report.
  //
  // The reason previously written here -- that a REFIT cell can reach the fitter
  // without being counted in `cells_to_fit`, and a run that lost every such cell
  // would be silenced by the conjunct -- is true about refits but describes a
  // shape this predicate cannot see: a run whose every refit died has
  // `cells_carried == 0` (a refit is precisely a present cell the carry gate
  // declined), so it lands in `is_total_fit_failure` and exits 3, correctly.
  // Recorded rather than deleted because it is the kind of not-quite-right reason
  // this branch's history is made of, and a future reader will otherwise re-derive
  // it. `cells_failed > 0` is still the right conjunct on its own merits: it is
  // the direct statement of "something was offered to the fitter and died".
  return r.coverage.cells_ok == 0 && r.coverage.cells_failed > 0 &&
         r.coverage.cells_carried > 0;
}

bool is_total_config_failure(const SurfaceDbBuildReport &r) {
  // Same shape as above (attempted > 0, succeeded == 0) read on the CONFIG stage,
  // because a universe swallowed here never reaches the fit stage's counters at
  // all: everything is disabled, nothing is scheduled, and `is_total_fit_failure`
  // sees the resume path.
  //
  // Read off the STANDING state, not this run's fresh verdicts (FIX-C-2). A
  // skipped-existing symbol was not tried by THIS run, but the database either
  // serves it or does not, and that is the question the exit code answers. Before
  // this, the FIRST run over a hopeless universe exited 3 and every later run over
  // the very same dead database exited 0, because its disabled configs had turned
  // into `n_skipped_existing`. The `cells_ok` clause still keeps a run that DID
  // produce surfaces out of it: partial, not dead.
  //
  // ── The C1 audit: does this predicate need the carry clause too? ────────────
  // It carries the identical `cells_ok == 0` conjunct that FIX-D turned into a
  // false verdict one stage down, so the question is real. The answer is that the
  // trap is UNREACHABLE here, and the clause is added anyway. Both halves matter,
  // so both are written down.
  //
  // REACHABILITY. `enabled == 0` and `cells_carried > 0` cannot hold together in
  // any report `build_surface_db` produces:
  //   1. `generate_symbol_configs` and `populate_universe_streaming` are handed
  //      the SAME `boards` span, and the config stage's dispositions partition the
  //      DISTINCT symbols of that span. So `enabled == 0` means: not one symbol
  //      among this run's boards has an enabled manifest config.
  //   2. The config stage stores a config for every symbol it sees (fail-closed
  //      disabled on a selection failure), so the populate's
  //      `resolve_symbol_config` never falls back to its enabled default for any
  //      of them -- every cell resolves DISABLED.
  //   3. `populate_universe_streaming` increments `to_add` only for an ENABLED
  //      cell, so `to_add == 0` on every date, so every date takes the
  //      `dates_skipped_complete` branch, which `continue`s BEFORE
  //      `cov.cells_carried += carried_here`. `kept` stays empty, the populate is
  //      never called, and both `cells_carried` and `cells_ok` are 0.
  // Symmetrically, the carry gate itself only ever admits a cell whose resolved
  // config is enabled to `cells_carried` -- so a carried cell is by construction
  // proof that `enabled > 0`.
  //
  // FIX-E LANDED THE PENDING CHANGE THIS PARAGRAPH ANTICIPATED, and step 3 above
  // is why it did not break either predicate. A present-but-disabled cell is now
  // PRESERVED through a rewrite instead of being deleted, so the carry request is
  // no longer enabled-only -- but its cells are tallied in
  // `cells_carried_disabled`, a separate counter neither predicate reads, exactly
  // so `cells_carried` keeps meaning "healthy stored surface reused as this run's
  // output" and keeps being proof that `enabled > 0`. Had the preserved cells
  // been folded into `cells_carried`, this clause would have started suppressing
  // a genuine all-disabled verdict on any database holding an old surface for one
  // of those symbols.
  //
  // WHY THE CLAUSE IS HERE ANYWAY. C1 happened because this predicate's twin was
  // silently correct for a reason nobody had written down ("a healthy resume
  // re-fits its siblings, so cells_ok is large"), and FIX-D invalidated that
  // reason without the predicate knowing. The reachability argument above is the
  // same kind of unwritten cross-module coupling: it depends on the populate's
  // carry gate testing `enabled`, and on the skipped-complete branch short-
  // circuiting before the carry tally -- two lines in a different file that a
  // future change may reasonably move. (The already-recorded
  // present-but-disabled-symbol drop is exactly such a pending change.) The
  // clause costs one comparison, it is pinned by a unit test, and it keeps the
  // two exit-code predicates reading the SAME evidence for "did this run produce
  // a surface at all" -- a carried cell is that evidence just as much as a fitted
  // one, because the database provably holds and re-emitted it.
  const std::uint32_t disabled = r.config.n_disabled_failed + r.config.n_disabled_existing;
  // n_disabled_existing is a sub-count of n_skipped_existing, so this cannot wrap.
  const std::uint32_t enabled =
      r.config.n_configured + (r.config.n_skipped_existing - r.config.n_disabled_existing);
  return disabled > 0 && enabled == 0 && r.coverage.cells_ok == 0 &&
         r.coverage.cells_carried == 0;
}

int build_exit_code(const SurfaceDbBuildReport &r, bool report_write_failed, bool strict) {
  // REV-R3 fix-1 (review I-2). Lifted verbatim out of `main()`'s `verdict`
  // lambda + return sites so it can be tested; see the header for the precedence
  // argument. The ORDER of these three blocks IS the contract.
  //
  // 5 first, because it preempts everything below it.
  if (r.coverage.dates_refused_coverage_regression > 0u) {
    return kSurfaceDbBuildExitCoverageRegression;
  }
  // One code for the four "the build ran and produced nothing" shapes. `main`
  // tests them in this same order to decide which diagnostic to print (the most
  // upstream cause first), but for the NUMBER the order is immaterial — they all
  // map here — so this is a disjunction rather than a chain of returns, and a
  // future predicate that also means "produced nothing" belongs in it.
  if (is_total_load_failure(r) || is_total_config_failure(r) || is_total_fit_failure(r) ||
      (strict && is_strict_total_fit_failure(r))) {
    return kSurfaceDbBuildExitTotalFitFailure;
  }
  // Last, and only when nothing above fired: the run was fine and the one thing
  // that broke is the file the operator asked for. A --report failure must never
  // bury a 3 or a 5 — that regression is what put this ordering here.
  return report_write_failed ? kSurfaceDbBuildExitReportWriteFailed : kSurfaceDbBuildExitOk;
}

bool refusal_advice_names_the_carry_rate(const SurfaceDbBuildReport &r) {
  // REV-R5 (review I-3). Two conjuncts; the header carries the argument for both.
  //
  // Saturating, for the reason recorded there: `dates_refused_partition_unlisted`
  // is a documented SUBSET of `dates_refused_coverage_regression`, guaranteed by
  // the populate that fills them, and a diagnostic is not the place to discover
  // that a hand-built report disagrees.
  const std::uint32_t unlisted = r.coverage.dates_refused_partition_unlisted <
                                         r.coverage.dates_refused_coverage_regression
                                     ? r.coverage.dates_refused_partition_unlisted
                                     : r.coverage.dates_refused_coverage_regression;
  const std::uint32_t listed = r.coverage.dates_refused_coverage_regression - unlisted;

  // (a) at least one refusal whose partition the manifest LISTS -- the only shape
  //     for which a failed re-fit, and therefore the carry rate, is the suspect.
  // (b) NOTHING was carried. A run that carried stored surfaces is a run whose
  //     stored records validated for reuse, so its rate is not what is wrong, and
  //     the escape this advice offers would delete exactly those surfaces. This
  //     is also what makes the advice mutually exclusive with the `--strict`
  //     block's "Do NOT reach for --r", which is reachable only when something
  //     WAS carried.
  return listed > 0u && r.coverage.cells_carried == 0u;
}

Status write_build_report_csv(const SurfaceDbBuildReport &r, std::string_view path) {
  std::string out;
  out.reserve(1024 + r.coverage.per_symbol.size() * 48);

  // Section 1: key,value scalar table. First line is the pinned header.
  out += "key,value\n";
  const auto kv = [&out](std::string_view key, const std::string &value) {
    out += key;
    out += ',';
    out += value;
    out += '\n';
  };
  kv("config.n_symbols", fmt_u32(r.config.n_symbols));
  kv("config.n_configured", fmt_u32(r.config.n_configured));
  kv("config.n_skipped_existing", fmt_u32(r.config.n_skipped_existing));
  kv("config.n_disabled_failed", fmt_u32(r.config.n_disabled_failed));
  kv("config.n_disabled_existing", fmt_u32(r.config.n_disabled_existing));
  kv("coverage.cells_loaded", fmt_u32(r.coverage.cells_loaded));
  kv("coverage.cells_to_fit", fmt_u32(r.coverage.cells_to_fit));
  kv("coverage.cells_refit", fmt_u32(r.coverage.cells_refit));
  // FIX-D fix-1 (I2). With `is_total_fit_failure` widened to tolerate a
  // carried-only resume, this counter is the ONLY external evidence that
  // carry-over engaged at all: the healthy steady state it produces is
  // `cells_ok = 0, cells_refit = 0`, which without this line is indistinguishable
  // from a build that did nothing. It sits beside cells_refit because the two are
  // the halves of `cells_already_present`-on-a-rewritten-date and are only
  // interpretable together.
  kv("coverage.cells_carried", fmt_u32(r.coverage.cells_carried));
  // FIX-E. Beside cells_carried because it is the same act -- re-emitting a
  // stored record instead of re-fitting it -- for a different reason, and the
  // operator needs to be able to tell them apart: this one names cells whose
  // config is switched OFF and whose bytes were kept anyway. Before FIX-E they
  // were deleted by any unrelated rewrite of their date, with no counter, no
  // message, and no way to notice.
  kv("coverage.cells_carried_disabled", fmt_u32(r.coverage.cells_carried_disabled));
  kv("coverage.cells_already_present", fmt_u32(r.coverage.cells_already_present));
  kv("coverage.cells_ok", fmt_u32(r.coverage.cells_ok));
  kv("coverage.cells_failed", fmt_u32(r.coverage.cells_failed));
  kv("coverage.dates_total", fmt_u32(r.coverage.dates_total));
  kv("coverage.dates_written", fmt_u32(r.coverage.dates_written));
  kv("coverage.dates_skipped_complete", fmt_u32(r.coverage.dates_skipped_complete));
  kv("coverage.dates_skipped_would_drop", fmt_u32(r.coverage.dates_skipped_would_drop));
  // REV-R3. The WRITE path's guard, distinct from the counter above it: dates
  // whose rewrite would have DESTROYED a stored surface. `refused` = the guard
  // held and the old partition is intact; `dropped` = --allow-coverage-regression
  // was given and those surfaces are gone. Both are emitted always, so a
  // scripted diff of two report CSVs sees a regression appear.
  kv("coverage.dates_refused_coverage_regression",
     fmt_u32(r.coverage.dates_refused_coverage_regression));
  // REV-R3 fix-2 (review N-3). A SUBSET of the row above — the refusals whose
  // partition file is on disk but unlisted in the manifest. Also always emitted:
  // it is the row that tells a reader which of the two causes a refusal had, and
  // a row that only appears when non-zero is a row nothing can diff against.
  kv("coverage.dates_refused_partition_unlisted",
     fmt_u32(r.coverage.dates_refused_partition_unlisted));
  kv("coverage.dates_dropped_coverage_regression",
     fmt_u32(r.coverage.dates_dropped_coverage_regression));
  // Task 3 (mark-domain-robustness observability). `dates_with_slice_drops`
  // counts WRITTEN dates that carry at least one section-5 row below;
  // `max_T_min` is the worst written date's own longest fitted pillar (min
  // over dates of that date's own max, %.6f is plenty of precision for a
  // year-fraction tenor). Both are 0 on a run with nothing to report, and both
  // are emitted always so a scripted diff of two report CSVs sees a
  // regression APPEAR rather than a line materialise (same discipline REV-R3
  // uses for the coverage-regression counters just above).
  kv("coverage.dates_with_slice_drops", fmt_u32(r.coverage.n_dates_with_slice_drops));
  kv("coverage.max_T_min", fmt_t(r.coverage.max_T_min));
  kv("n_dates_loaded", fmt_usize(r.n_dates_loaded));
  kv("n_dates_missing", fmt_usize(r.n_dates_missing));
  kv("n_load_errors", fmt_usize(r.n_load_errors));
  kv("n_coverage_holes", fmt_usize(r.n_coverage_holes));

  // Section 2: WHICH symbols the config stage left DISABLED — the durable
  // counterpart to the terminal's `config.failed_symbols` line, and the whole
  // point of FIX-C-2. The count above says how many; only this says which, and
  // `config.failed_symbols` carries the STANDING set, so a resume names the same
  // casualties the first build named instead of reporting green over them.
  // Header emitted even when the list is empty, so the file's shape is constant.
  out += "config_disabled_symbol\n";
  for (const std::string &s : r.config.failed_symbols) {
    out += s;
    out += '\n';
  }

  // Section 3: one per-symbol coverage row, straight from the populate — over the
  // dates this run PROCESSED, which since REV-R3's coverage guard is NOT the set
  // it WROTE: a refused date ran its fits in full and withheld only the commit, so
  // its cells are in these rows while `coverage.dates_written` never counted the
  // date. (REV-R6: this line said "written dates", the same staleness REV-R5
  // corrected on the counter's own doc and in `print_report` without reaching the
  // CSV writer. `UniversePopulateCoverage::per_symbol` is the contract.)
  // `n_carried` appended (FIX-D fix-1, I2): a carried symbol's row otherwise reads
  // attempted=1 ok=0 failed=0 disabled=0, which names no disposition at all. The
  // column is APPENDED so a positional reader of the first five fields is
  // unaffected; the header is a pinned contract and its test was updated with it.
  out += "symbol,n_attempted,n_ok,n_failed,n_disabled,n_carried\n";
  for (const PopulateSymbolStats &s : r.coverage.per_symbol) {
    out += s.symbol;
    out += ',';
    out += fmt_u32(s.n_attempted);
    out += ',';
    out += fmt_u32(s.n_ok);
    out += ',';
    out += fmt_u32(s.n_failed);
    out += ',';
    out += fmt_u32(s.n_disabled);
    out += ',';
    out += fmt_u32(s.n_carried);
    out += '\n';
  }

  // Section 4: WHY each failed cell failed — the fit stage's counterpart to the
  // config stage's failed-symbol list. The FULL list, deliberately uncapped: the
  // terminal gets a bounded sample (reported_failed_cells) but this file is the
  // artifact an operator greps to root-cause, so truncating it here would defeat
  // the whole point of preserving the reason.
  out += "date,symbol,code,detail\n";
  for (const FailedCell &f : r.coverage.failed_cells) {
    out += f.date;
    out += ',';
    out += f.symbol;
    out += ',';
    out += atx::core::to_string(f.code);
    out += ',';
    // The one free-text field: RFC4180-quote it so a comma inside a fitter
    // message can never shift the columns.
    out += '"';
    for (const char c : f.detail) {
      if (c == '"') {
        out += '"'; // doubled, per RFC4180
      }
      out += c;
    }
    out += '"';
    out += '\n';
  }

  // Section 5 (Task 3, mark-domain-robustness): every non-Fitted expiry of a
  // WRITTEN date's fresh fits — the per-slice counterpart to section 4's
  // per-CELL failures. A `FailedCell` (section 4) is a whole (date, symbol)
  // that never produced a surface; a `SliceDropCell` here is a slice missing
  // from a surface that WAS produced and IS served, which is exactly the
  // silent-extrapolation shape this task exists to surface. Uncapped, for the
  // same reason section 4 is uncapped: this file is where an operator
  // root-causes a dropped tenor, not a terminal that would elide most of a
  // whole-universe stressed-day event. The header's own spelling
  // (`slice_drop.date`, not `date`) matches the `key,value` section's
  // `coverage.*` naming convention rather than section 4's bare `date` — a
  // deliberate difference, same reasoning as section 4 vs section 6's
  // deliberately different column names below: a naive parser must not be
  // able to splice sections that carry different things. Header emitted even
  // when empty — constant shape, same as every other section here.
  out += "slice_drop.date,symbol,T,outcome,n_used\n";
  for (const SliceDropCell &d : r.coverage.slice_drops) {
    out += d.date;
    out += ',';
    out += d.symbol;
    out += ',';
    out += fmt_t(d.T);
    out += ',';
    out += slice_drop_outcome_name(d);
    out += ',';
    out += fmt_usize(d.n_used);
    out += '\n';
  }

  // Section 6 (REV-R3, renumbered from 5 by Task 3's section 5 insertion above):
  // WHICH stored surfaces the write path refused to destroy — or, under
  // `--allow-coverage-regression`, did destroy. Uncapped for the same reason
  // section 4 is: this file is the artifact, and on the destructive path it is
  // the ONLY durable record of what was removed (the archive format keeps no
  // tombstone, which is why the 95-surface incident left no trace). Column
  // names deliberately differ from section 4's so a naive parser cannot splice
  // the two sections together. Header emitted even when empty — constant shape.
  out += "regression_date,regression_symbol\n";
  for (const CoverageRegressionCell &c : r.coverage.coverage_regression_cells) {
    out += c.date;
    out += ',';
    out += c.symbol;
    out += '\n';
  }

  std::ofstream os(std::string(path), std::ios::binary | std::ios::trunc);
  if (!os) {
    return Err(ErrorCode::IoError, "write_build_report_csv: cannot open file");
  }
  os.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!os) {
    return Err(ErrorCode::IoError, "write_build_report_csv: write failed");
  }
  return Ok();
}

} // namespace atx::vol

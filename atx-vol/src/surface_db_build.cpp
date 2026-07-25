#include "atx/vol/surface_db_build.hpp"

#include <algorithm>
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
#include "atx/vol/session.hpp"        // make_session_inputs, SessionInputs
#include "atx/vol/surface_db_populate.hpp" // populate_universe_streaming, UniversePopulateSpec
#include "atx/vol/surface_parity.hpp" // SurfaceParityInputs
#include "atx/vol/universe.hpp"       // Underlying, Chain
#include "atx/vol/vol_curve.hpp"      // CurveConfig (dense index recipe)
#include "surface_db_seed.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

SymbolFitConfig seed_symbol_config(std::string_view symbol, FitPreset preset,
                                   std::string_view index_symbol) {
  SymbolFitConfig cfg = symbol_config_from_preset(preset);
  if (!index_symbol.empty() && symbol == index_symbol) {
    cfg.pin_curve = true;
    cfg.curve = CurveConfig{}; // default = the dense index recipe (node_cap 40)
  }
  return cfg;
}

namespace {

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

    // Board-feature classification: pins the curve FAMILY. The numerical preset
    // tier stays the operator-selected `spec.preset` (matching
    // populate_universe_streaming's seeding and the StoresConfigPerSymbol
    // contract); the policy decides the family, not the tier.
    const FitDecision decision = select_fit_policy(under, board.symbol, board.fit_context, {});
    SymbolFitConfig cfg = symbol_config_from_preset(spec.preset);
    cfg.pin_curve = true;
    cfg.curve = decision.curve;

    if (spec.deep_selection) {
      const SurfaceParityInputs sp = parity_inputs_from_chain(*chain, cfg.preset);
      const Result<SelectorResult> selected =
          select_curve(under, sp, production_selector_config());
      if (selected.has_value()) {
        cfg.curve = selected->chosen; // pin the held-out winner
        cfg.pin_curve = true;
      } else {
        const ErrorCode code = selected.error().code();
        // NotFound (no scorable holdout) / Unavailable (budget) are not defects:
        // fall back to the fit-policy decision curve already pinned above. Any
        // other error (e.g. InvalidArgument) is a hard selection failure.
        if (code != ErrorCode::NotFound && code != ErrorCode::Unavailable) {
          return false;
        }
      }
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
  for (const auto &[symbol, board_ptr] : chosen) {
    ++report.n_symbols;

    // Idempotent by default: an already-configured symbol is left untouched so a
    // re-run never clobbers an operator override.
    if (!spec.overwrite_existing && db.symbol_config(symbol).has_value()) {
      ++report.n_skipped_existing;
      continue;
    }

    // The index leg is pinned to the dense recipe (shared with
    // populate_universe_streaming), bypassing per-board selection.
    if (!spec.index_symbol.empty() && symbol == spec.index_symbol) {
      const SymbolFitConfig cfg = seed_symbol_config(symbol, spec.preset, spec.index_symbol);
      if (const Status up = db.upsert_symbol(symbol, cfg); !up.has_value()) {
        return Err(up.error());
      }
      ++report.n_configured;
      continue;
    }

    SymbolFitConfig cfg;
    if (!config_for_board(*board_ptr, spec, cfg)) {
      // Fail closed: store the preset config DISABLED and record the symbol; the
      // top-level call still succeeds so one bad board never sinks the build.
      cfg = symbol_config_from_preset(spec.preset);
      cfg.enabled = false;
      if (const Status up = db.upsert_symbol(symbol, cfg); !up.has_value()) {
        return Err(up.error());
      }
      ++report.n_disabled_failed;
      report.failed_symbols.push_back(symbol);
      continue;
    }

    if (const Status up = db.upsert_symbol(symbol, cfg); !up.has_value()) {
      return Err(up.error());
    }
    ++report.n_configured;
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
  Result<OpraBatchResult> loaded = load_opra_hive(spec.hive);
  if (!loaded) {
    return Err(loaded.error());
  }

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
  Result<AutoConfigReport> cfg = generate_symbol_configs(db, boards, spec.auto_config);
  if (!cfg) {
    return Err(cfg.error());
  }
  report.config = std::move(*cfg);

  // 5. Cell-aware streaming populate. The index leg / preset / worker budget come
  //    from this spec; an empty board set is a graceful all-zero no-op.
  UniversePopulateSpec pspec;
  pspec.index_symbol = spec.auto_config.index_symbol;
  pspec.preset = spec.preset;
  pspec.fit_workers = spec.fit_workers;
  Result<UniversePopulateCoverage> cov = populate_universe_streaming(db, boards, pspec);
  if (!cov) {
    return Err(cov.error());
  }
  report.coverage = std::move(*cov);

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

} // namespace

bool is_total_fit_failure(const SurfaceDbBuildReport &r) {
  // Exactly two conditions, both required (see the header for why neither may be
  // widened): work WAS scheduled, and none of it landed. `cells_to_fit == 0` is
  // the resume/empty-window path and stays a success; any `cells_ok > 0` is
  // partial coverage, which is normal production output.
  return r.coverage.cells_to_fit > 0 && r.coverage.cells_ok == 0;
}

bool is_total_config_failure(const SurfaceDbBuildReport &r) {
  // Same shape as above (attempted > 0, succeeded == 0) read on the CONFIG stage's
  // counters, because a universe swallowed here never reaches the fit stage's
  // counters at all: everything is disabled, nothing is scheduled, and
  // `is_total_fit_failure` sees the resume path. `n_skipped_existing` is
  // deliberately NOT part of "attempted" — a symbol left untouched by the
  // idempotent resume was not tried, so a re-run over an already-configured db
  // stays green. The `cells_ok` clause keeps a run that DID produce surfaces out
  // of it (only its new names failed selection): partial, not dead.
  return r.config.n_disabled_failed > 0 && r.config.n_configured == 0 &&
         r.coverage.cells_ok == 0;
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
  kv("coverage.cells_loaded", fmt_u32(r.coverage.cells_loaded));
  kv("coverage.cells_to_fit", fmt_u32(r.coverage.cells_to_fit));
  kv("coverage.cells_refit", fmt_u32(r.coverage.cells_refit));
  kv("coverage.cells_already_present", fmt_u32(r.coverage.cells_already_present));
  kv("coverage.cells_ok", fmt_u32(r.coverage.cells_ok));
  kv("coverage.cells_failed", fmt_u32(r.coverage.cells_failed));
  kv("coverage.dates_total", fmt_u32(r.coverage.dates_total));
  kv("coverage.dates_written", fmt_u32(r.coverage.dates_written));
  kv("coverage.dates_skipped_complete", fmt_u32(r.coverage.dates_skipped_complete));
  kv("coverage.dates_skipped_would_drop", fmt_u32(r.coverage.dates_skipped_would_drop));
  kv("n_dates_loaded", fmt_usize(r.n_dates_loaded));
  kv("n_dates_missing", fmt_usize(r.n_dates_missing));
  kv("n_load_errors", fmt_usize(r.n_load_errors));
  kv("n_coverage_holes", fmt_usize(r.n_coverage_holes));

  // Section 2: one per-symbol coverage row (from the populate; written dates).
  out += "symbol,n_attempted,n_ok,n_failed,n_disabled\n";
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

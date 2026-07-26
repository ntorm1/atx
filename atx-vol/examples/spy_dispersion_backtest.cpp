// Real-data workflow for the traditional SPY listed-options dispersion proxy.
// Each command is a process boundary; no fitter/session object crosses it.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/run_archive.hpp"
#include "atx/vol/run_diagnostics.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/dispersion_workflow.hpp"
#include "atx/vol/historical_projection.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_dispersion_pipeline.hpp"
#include "atx/vol/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/listed_dispersion_strategy.hpp"
#include "atx/vol/listed_definitions_cache.hpp"
#include "atx/vol/listed_opra.hpp"
#include "atx/vol/occ_ess.hpp"
#include "atx/vol/opra_batch.hpp"
#include "atx/vol/phase_profile.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/strategy.hpp"
#include "atx/vol/tearsheet.hpp"
#include "atx/vol/types.hpp"

#ifdef _WIN32
#include <fcntl.h> // _O_BINARY (runarchive dump: byte-exact stdout, no CRLF)
#include <io.h>    // _setmode / _fileno
#endif

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

template <class T> bool parse_number(std::string_view text, T &value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

std::vector<std::string_view> split(std::string_view line, char delimiter) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t end = line.find(delimiter, start);
    fields.push_back(
        line.substr(start, end == std::string_view::npos ? line.size() - start : end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return fields;
}

Result<std::string> read_text(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::NotFound, "cannot open " + path.string());
  }
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    return Err(ErrorCode::IoError, "cannot read " + path.string());
  }
  return Ok(std::move(text));
}

std::uint64_t hash_text(std::string_view text) {
  const std::uint64_t hash = atx::core::hash_bytes(text.data(), text.size());
  return hash == 0u ? 1u : hash;
}

// ATX_VOL_CACHE: the default source for `--cache DIR` when the flag is not
// given explicitly. `std::getenv` trips this build's /WX
// (unused-result-adjacent MSVC deprecation), so `_dupenv_s` is used on
// Windows, matching the existing pattern in
// listed_definitions_cache_test.cpp's `definitions_tsv_path_from_env`. An
// EMPTY return (unset or explicitly empty) means "disabled" — the same
// sentinel `read_listed_definitions_cached` already uses for "no cache".
std::string cache_dir_from_env() {
  std::string path;
#if defined(_WIN32)
  char *raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, "ATX_VOL_CACHE") == 0 && raw != nullptr) {
    path.assign(raw);
    std::free(raw);
  }
#else
  if (const char *raw = std::getenv("ATX_VOL_CACHE")) {
    path.assign(raw);
  }
#endif
  return path;
}

// The archive-file fingerprint that build-schedule stamped as `surface_fingerprint`
// now lives in the library (listed_dispersion_pipeline.cpp `hash_archive_file`, an
// anonymous-namespace helper byte-for-byte identical to the old example `hash_file`).
// After the T9 build-schedule cutover the example had no remaining caller, so its
// duplicate `hash_file` was removed rather than exporting the library's internal
// helper (O5). `hash_text` (build-corpus input/policy fingerprints) and `read_text`
// stay — they have other callers.

// ── Runtime diagnostics ──────────────────────────────────────────────────────
// PhaseTimer + the `diagnostics` section encoder now live in the library
// (atx/vol/run_diagnostics.hpp), so the binary result container measures phases
// exactly as the old loose diagnostics_<subcommand>.tsv did. `PhaseTimer` here
// resolves to atx::vol::PhaseTimer via the `using namespace atx::vol` above.

// One-line stderr telemetry summary (preserved verbatim from the old
// write_diagnostics stderr line). `total` is measured over the whole command
// independently of the phase sum; `unit`/`units` name the count denominator
// ("session"/"sessions" for backtests, "roll"/"rolls" for the schedule builders).
// Telemetry only: no economic output, no stdout line, no artifact.
void print_diag_summary(const char *subcommand, PhaseTimer::Duration total,
                        std::uint64_t total_count, const char *unit, const char *units) {
  const double total_ms = std::chrono::duration<double, std::milli>(total).count();
  const double per_unit = total_count > 0u ? total_ms / static_cast<double>(total_count) : 0.0;
  std::fprintf(stderr, "diag %s: total=%.3fms %s=%llu (%.3f ms/%s)\n", subcommand, total_ms, units,
               static_cast<unsigned long long>(total_count), per_unit, unit);
}

// ── RunArchive staging helpers ───────────────────────────────────────────────

// Intern `value` into a first-appearance dict column, returning its u32 code.
// O(dict) per call — fine for the small mark-divergence set the only caller
// builds. The dict/codes vectors grow in lockstep.
std::uint32_t dict_intern(std::vector<std::string> &dict, std::string_view value) {
  for (std::size_t i = 0; i < dict.size(); ++i) {
    if (dict[i] == value) {
      return static_cast<std::uint32_t>(i);
    }
  }
  dict.emplace_back(value);
  return static_cast<std::uint32_t>(dict.size() - 1u);
}

// Mark-divergence rows collected by the Record replay, staged for the
// `mark_divergence` RunArchive section (there is no library encoder for it — it
// is example-owned, so the section is hand-built here). Owns every array its
// columns span (dict codes/tables + numeric columns), so the returned section is
// self-contained via RaSectionData::storage.
struct MarkDivergenceArena {
  std::vector<std::string> date_dict, symbol_dict, raw_symbol_dict;
  std::vector<std::uint32_t> date_codes, symbol_codes, raw_symbol_codes;
  std::vector<std::uint8_t> side_codes;              // 0 = Call, 1 = Put
  std::vector<std::string> side_labels{"Call", "Put"};
  std::vector<double> strike, schedule_mark, live_mark, diff, abs_diff_bps_of_mark;
  std::vector<std::int64_t> expiry_ts_ns;
  std::uint64_t n_rows{0};
};

// Build the `mark_divergence` SubTable section in kMarkDivergenceCols registry
// order from a filled arena. The arena is moved into the section's storage, so
// every spanned array outlives the write.
RaSectionData build_mark_divergence_section(std::shared_ptr<MarkDivergenceArena> arena) {
  RaSectionData sec;
  sec.name = "mark_divergence";
  sec.kind = RaSectionKind::SubTable;
  sec.n_rows = arena->n_rows;
  sec.columns.emplace_back("date", RaColumnData::of_dict(arena->date_codes, arena->date_dict));
  sec.columns.emplace_back("symbol",
                           RaColumnData::of_dict(arena->symbol_codes, arena->symbol_dict));
  sec.columns.emplace_back("raw_symbol",
                           RaColumnData::of_dict(arena->raw_symbol_codes, arena->raw_symbol_dict));
  sec.columns.emplace_back("strike", RaColumnData::of_f64(arena->strike));
  sec.columns.emplace_back("expiry_ts_ns", RaColumnData::of_i64(arena->expiry_ts_ns));
  sec.columns.emplace_back("side", RaColumnData::of_u8enum(arena->side_codes, arena->side_labels));
  sec.columns.emplace_back("schedule_mark", RaColumnData::of_f64(arena->schedule_mark));
  sec.columns.emplace_back("live_mark", RaColumnData::of_f64(arena->live_mark));
  sec.columns.emplace_back("diff", RaColumnData::of_f64(arena->diff));
  sec.columns.emplace_back("abs_diff_bps_of_mark",
                           RaColumnData::of_f64(arena->abs_diff_bps_of_mark));
  sec.storage = std::move(arena);
  return sec;
}

Status write_input_inventory(const fs::path &path, const OpraBatchResult &batch) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot write input inventory");
  }
  out << "date\tsymbol\tpath\tstatus\tsource_schema_version\tsource_fingerprint\t"
         "market_input_fingerprint\n";
  for (const OpraBatchEntry &entry : batch.entries) {
    out << entry.date << '\t' << entry.symbol << '\t' << entry.path << '\t';
    if (entry.panel) {
      out << "Loaded\t" << entry.panel->source_schema_version << '\t'
          << entry.panel->source_fingerprint << '\t'
          << entry.panel->market_input_provenance.fingerprint;
    } else {
      out << (entry.panel.error().code() == ErrorCode::NotFound ? "Missing" : "Error")
          << "\t0\t0\t0";
    }
    out << '\n';
  }
  return out ? Ok() : Err(ErrorCode::IoError, "cannot flush input inventory");
}

Status persist_occ_ess_evidence(const fs::path &run_dir, const RunSpec &spec,
                                const OpraBatchResult &batch) {
  std::set<std::string> loaded_dates;
  for (const OpraBatchEntry &entry : batch.entries) {
    if (entry.panel) {
      loaded_dates.insert(entry.date);
    }
  }
  if (loaded_dates.empty()) {
    return Err(ErrorCode::NotFound, "no loaded dates for OCC ESS evidence");
  }

  const fs::path evidence_dir = run_dir / "occ_ess";
  std::error_code error;
  fs::create_directories(evidence_dir, error);
  if (error) {
    return Err(ErrorCode::IoError, "cannot create OCC ESS evidence directory");
  }
  std::ofstream inventory(run_dir / "occ_ess_inventory.tsv", std::ios::binary | std::ios::trunc);
  if (!inventory) {
    return Err(ErrorCode::IoError, "cannot write OCC ESS inventory");
  }
  inventory << "date\tpath\tn_special_symbols\tsource_fingerprint\n";
  for (const std::string &date : loaded_dates) {
    const fs::path source = spec.occ_ess_root / (date + ".txt");
    ATX_TRY(OccEssReport report, read_occ_ess_report_file(source.string()));
    if (report.trade_date() != date) {
      return Err(ErrorCode::InvalidArgument, "OCC ESS evidence date mismatch");
    }
    ATX_TRY(std::string bytes, read_text(source));
    const fs::path target = evidence_dir / (date + ".txt");
    const fs::path pending = target.string() + ".pending";
    {
      std::ofstream output(pending, std::ios::binary | std::ios::trunc);
      if (!output || !output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return Err(ErrorCode::IoError, "cannot write pending OCC ESS evidence");
      }
    }
    fs::rename(pending, target, error);
    if (error) {
      return Err(ErrorCode::IoError, "cannot publish OCC ESS evidence");
    }
    inventory << date << '\t' << target.string() << '\t' << report.special_symbols().size() << '\t'
              << report.source_fingerprint() << '\n';
  }
  return inventory ? Ok() : Err(ErrorCode::IoError, "cannot flush OCC ESS inventory");
}

Status verify_occ_ess_evidence(const fs::path &run_dir, const Clock &clock) {
  ATX_TRY(std::string inventory, read_text(run_dir / "occ_ess_inventory.tsv"));
  const std::vector<std::string_view> lines = split(inventory, '\n');
  if (lines.empty() || lines[0] != "date\tpath\tn_special_symbols\tsource_fingerprint") {
    return Err(ErrorCode::ParseError, "bad OCC ESS inventory header");
  }
  std::set<std::string> verified_dates;
  for (std::size_t i = 1u; i < lines.size(); ++i) {
    std::string_view line = lines[i];
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1u);
    }
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string_view> row = split(line, '\t');
    std::size_t n_special = 0u;
    std::uint64_t fingerprint = 0u;
    if (row.size() != 4u || !parse_number(row[2], n_special) ||
        !parse_number(row[3], fingerprint) || fingerprint == 0u ||
        !verified_dates.emplace(row[0]).second) {
      return Err(ErrorCode::ParseError, "malformed OCC ESS inventory row");
    }
    const fs::path expected =
        (run_dir / "occ_ess" / (std::string(row[0]) + ".txt")).lexically_normal();
    if (fs::path(row[1]).lexically_normal() != expected) {
      return Err(ErrorCode::InvalidArgument, "OCC ESS inventory path escapes run envelope");
    }
    ATX_TRY(OccEssReport report, read_occ_ess_report_file(expected.string()));
    if (report.trade_date() != row[0] || report.special_symbols().size() != n_special ||
        report.source_fingerprint() != fingerprint) {
      return Err(ErrorCode::InvalidArgument, "OCC ESS inventory/report mismatch");
    }
  }
  for (const SnapshotRef &ref : clock.refs()) {
    if (!verified_dates.contains(ref.date)) {
      return Err(ErrorCode::NotFound, "qualified date lacks OCC ESS authority");
    }
  }
  return Ok();
}

Status write_methodology_map(const fs::path &path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot write methodology map");
  }
  out << "choice\tpublic_anchor\tatx_adaptation\n"
      << "short_index_atm_straddle\tCboe traditional dispersion\tSPY American ETF options "
         "replace SPX\n"
      << "long_component_atm_straddles\tCboe traditional dispersion\tpoint-in-time supplied "
         "SPY constituent proxy\n"
      << "top_50_breadth\tCboe COR3M top-50 value-weighted basket\texact only when supplied "
         "schedule matches official effective basket\n"
      << "surface_prices_and_greeks\tCboe fitted option analytics\tatx-vol American fitted "
         "surfaces reloaded from ATXVSA\n"
      << "daily_hedge_monthly_roll\tBNP Paribas public dispersion implementation\tdaily close "
         "delta hedge and common listed monthly expiry\n"
      << "standard_contract_rule\tOCC daily Equity Special Settlements and OIC contract-size "
         "guidance\tvalidated non-special products use 100 shares when OPRA deliverable fields "
         "are undefined\n"
      << "vega_flat\tdirect Greek identity\tcontinuous notional using served American vegas\n";
  return out ? Ok() : Err(ErrorCode::IoError, "cannot flush methodology map");
}

Status build_corpus_command(const fs::path &source_spec_path, const fs::path &run_dir) {
  ATX_TRY(RunSpec spec, read_run_spec(source_spec_path));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(spec.universe_path));
  const std::vector<std::string> symbols = all_symbols(universe_rows, spec.index_symbol);
  // L9: the loose entry-gate floor (SPY + 50 names) reads from the one versioned
  // methodology policy instead of a scattered literal. `min_names_entry` == 51.
  const ListedDispersionMethodology methodology;
  if (spec.core_mode && symbols.size() < methodology.min_names_entry) {
    return Err(ErrorCode::InvalidArgument, "core mode requires SPY plus at least 50 names");
  }
  ATX_TRY(OpraBatchResult batch,
          load_opra_daterange(batch_spec(spec, symbols, spec.date_lo, spec.date_hi)));

  std::error_code fs_error;
  fs::create_directories(run_dir / "archives", fs_error);
  if (fs_error) {
    return Err(ErrorCode::IoError, "cannot create run directory");
  }
  ATX_TRY_VOID(write_input_inventory(run_dir / "input_inventory.tsv", batch));
  if (!spec.occ_ess_root.empty())
    ATX_TRY_VOID(persist_occ_ess_evidence(run_dir, spec, batch));
  ATX_TRY_VOID(write_methodology_map(run_dir / "methodology_map.tsv"));
  QualifiedCorpusConfig config;
  config.build.n_threads = spec.fit_workers;
  config.build.fit_template.preset = FitPreset::Hft;
  CurveConfig direct_curve;
  direct_curve.kind = VolCurveKind::LinearVariance;
  config.build.fit_template.curve = direct_curve;
  config.build.fit_template.enforce_calendar_floor = true;
  config.admission.enabled = true;
  CorpusAdmissionRule rule;
  rule.min_quotes = 20u;
  rule.min_slices = 2u;
  rule.require_calendar_arb_free = true;
  rule.calendar_abs_k = 0.7;
  rule.require_source_provenance = true;
  for (CorpusAdmissionRule &profile_rule : config.admission.by_profile) {
    profile_rule = rule;
  }
  config.input_fingerprint =
      hash_text(spec.date_lo + "|" + spec.date_hi + "|" + std::to_string(symbols.size()));
  config.policy_fingerprint =
      hash_text("spy-listed-dispersion-admission-v4-pinned-linear-calendar-floor-k0.7");
  ATX_TRY(CorpusBuildSession session,
          CorpusBuildSession::create((run_dir / "archives").string(), config));
  std::size_t cursor = 0;
  while (cursor < batch.entries.size()) {
    const std::string date = batch.entries[cursor].date;
    std::vector<CorpusCellInput> cells;
    while (cursor < batch.entries.size() && batch.entries[cursor].date == date) {
      OpraBatchEntry &entry = batch.entries[cursor++];
      if (entry.panel) {
        cells.emplace_back(
            corpus_board_from_opra(entry.date, entry.symbol, std::move(*entry.panel)));
      } else {
        CorpusSourceFailure failure;
        failure.date = entry.date;
        failure.symbol = entry.symbol;
        failure.reason = entry.panel.error().code() == ErrorCode::NotFound
                             ? CorpusAdmissionReason::MissingSource
                             : CorpusAdmissionReason::InvalidSourceSchema;
        failure.error_code = entry.panel.error().code();
        cells.emplace_back(std::move(failure));
      }
    }
    ATX_TRY_VOID(session.append_date(date, cells));
  }
  ATX_TRY(QualifiedCorpusManifest built, session.finish());
  ATX_TRY_VOID(write_manifest_file((run_dir / "surface_manifest.tsv").string(), built.manifest));
  ATX_TRY_VOID(write_quality_report_file((run_dir / "quality.tsv").string(), built.quality));
  fs::copy_file(spec.universe_path, run_dir / "universe_schedule.tsv",
                fs::copy_options::overwrite_existing, fs_error);
  if (fs_error) {
    return Err(ErrorCode::IoError, "cannot copy universe schedule");
  }
  RunSpec persisted_spec = spec;
  persisted_spec.universe_path = "universe_schedule.tsv";
  if (!spec.definitions_path.empty()) {
    fs_error.clear();
    fs::copy_file(spec.definitions_path, run_dir / "definitions.tsv",
                  fs::copy_options::overwrite_existing, fs_error);
    if (fs_error)
      return Err(ErrorCode::IoError, "cannot copy definitions");
    persisted_spec.definitions_path = "definitions.tsv";
  }
  ATX_TRY_VOID(write_resolved_spec(run_dir / "run_spec.tsv", persisted_spec));
  std::printf("built qualified corpus: admitted=%u quarantined=%u source_failed=%u\n",
              built.quality.n_admitted, built.quality.n_quarantined, built.quality.n_source_failed);
  return Ok();
}

// The per-date OPRA quote join (formerly the example's `load_listed_quotes`) now
// lives in the library as `listed_quotes_for_date` (verbatim lift, T1). Both former
// consumers — build-schedule (via build_listed_dispersion_schedule) and run-backtest
// reconciliation — call the library seam, so the example's duplicate was removed (T9).

Status build_schedule_command(const fs::path &run_dir, const fs::path &cache_dir) {
  const auto cmd_start = PhaseTimer::now();
  PhaseTimer timer(kBuildSchedulePhases);

  // setup_read is charged in TWO disjoint segments around the definitions read,
  // which is billed to its own `definitions_parse` phase. The two phases
  // partition the old single `setup_read` span; nothing between them is
  // uncharged and nothing is charged twice.
  auto phase = PhaseTimer::now();
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  timer.add("setup_read", phase);

  // Task 8: `--cache DIR` / `ATX_VOL_CACHE` (default DISABLED, i.e. `cache_dir`
  // empty) opts into the ATXDEFS1 pre-parsed cache. Disabled, this is
  // byte-for-byte `read_listed_definitions_file` — see that seam's costs
  // disclosure in listed_definitions_cache.hpp before enabling it. `&timer`
  // charges the `definitions_cache` hit/miss phase ONLY when the cache is
  // consulted (`cache_dir` non-empty); a disabled run charges nothing extra,
  // so its `diagnostics` row set is identical to today's.
  const auto definitions_start = PhaseTimer::now();
  ATX_TRY(ListedDefinitionTable definitions,
          read_listed_definitions_cached((run_dir / "definitions.tsv").string(), cache_dir.string(),
                                         kDefinitionsCacheFingerprintDefault, &timer));
  timer.add("definitions_parse", definitions_start, 1u);

  phase = PhaseTimer::now();
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY_VOID(verify_occ_ess_evidence(run_dir, clock));
  // L9: the core-mode admitted-date floor reads from the versioned methodology
  // (`core_min_dates` == 60) instead of a scattered literal. The same policy is
  // handed to the library builder for the entry/three-roll acceptance gate.
  const ListedDispersionMethodology method;
  if (spec.core_mode && clock.size() < method.core_min_dates) {
    return Err(ErrorCode::Unavailable, "core mode requires at least 60 admitted dates");
  }
  timer.add("setup_read", phase);

  // Selection + roll economics live in the library (listed_dispersion_pipeline). The
  // swept knobs are pulled from RunSpec into the POD ListedScheduleSpec; the rest of
  // RunSpec (OPRA parquet coordinates) is handed on as the quote source. The CLI's
  // PhaseTimer is threaded in (T9/O4) so the library charges the `selection` /
  // `quote_join` phases and build-schedule's diagnostics keep per-phase granularity.
  // The DTE roll-trigger, per-date universe rebind, forward lookup, coverage gate,
  // deferral, cohort numbering, surface fingerprint, roll sizing, the entry/three-roll
  // acceptance gate, and the M1 clock/first-roll coupling check all run inside the call.
  ListedScheduleSpec sched_spec;
  sched_spec.target_dte_days = spec.target_dte_days;
  sched_spec.min_dte_days = spec.min_dte_days;
  sched_spec.max_dte_days = spec.max_dte_days;
  sched_spec.roll_dte_days = spec.roll_dte_days;
  sched_spec.min_names = spec.min_names;
  sched_spec.min_weight_coverage = spec.min_weight_coverage;
  sched_spec.gross_index_vega = spec.gross_index_vega;
  sched_spec.core_mode = spec.core_mode;
  ATX_TRY(ListedDispersionSchedule schedule,
          build_listed_dispersion_schedule(clock, sched_spec, method, universe_rows, definitions,
                                           spec, &timer));

  const auto write_start = PhaseTimer::now();
  // trade_schedule.tsv stays a text INPUT: run-backtest / project-schedule /
  // run-projected-backtest read it back through read_listed_dispersion_schedule_
  // file (the retained input read path). The schedule is ALSO folded into
  // run.atxrun as the trade_schedule section (the result container). run-backtest
  // later republishes run.atxrun with the full economic result set.
  //
  // ORDERING IS LOAD-BEARING (Wave E T6): trade_schedule.tsv is one of the five
  // files RunDir::run_identity_hash folds, and that identity is the merge-write
  // cache key. This write MUST stay ABOVE the write_run_archive call below. If the
  // archive were stamped first, trade_schedule.tsv would appear afterwards, the
  // next route (run-backtest) would recompute a different identity, and its write
  // would start FRESH — silently dropping this command's trade_schedule section
  // with no error. Pinned by
  // RunDir.MergeWriteDropsCarriedSectionsWhenAFoldedInputAppearsLate.
  ATX_TRY_VOID(
      write_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string(), schedule));
  std::vector<RaSectionData> sections;
  sections.push_back(encode_schedule_section("trade_schedule", schedule));
  timer.add("write_outputs", write_start);
  sections.push_back(encode_diagnostics_section(timer, "build_schedule", schedule.rolls.size()));
  // Must stay BELOW the trade_schedule.tsv write above — see the ordering note
  // there and the contract on RunDir::run_identity_hash.
  ATX_TRY_VOID(RunDir(run_dir).write_run_archive(sections));
  std::printf("built immutable schedule: rolls=%zu\n", schedule.rolls.size());
  print_diag_summary("build_schedule", PhaseTimer::now() - cmd_start, schedule.rolls.size(), "roll",
                     "rolls");
  return Ok();
}

Status verify_command(const fs::path &run_dir) {
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(CorpusQualityReport quality,
          read_quality_report_file((run_dir / "quality.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY_VOID(verify_occ_ess_evidence(run_dir, clock));

  // Result envelope: run.atxrun opens, every result section's payload CRC
  // validates, the required result sections are present + framed, the retained
  // text inputs parse, the backtest/reconciliation cardinalities agree, the
  // schedule passes its structural + vega-arithmetic validation, and the
  // core-mode date/roll/breadth floors hold. This is the example's old verify
  // gate lifted into RunDir::verify — the loose backtest.tsv / contract_marks.tsv
  // / reconciliation.tsv result files no longer exist, so their existence checks
  // are now the layered-CRC envelope over run.atxrun's sections.
  ATX_TRY_VOID(RunDir(run_dir).verify());

  // Retained text inputs still required to exist and be non-empty.
  for (const fs::path &required : {run_dir / "input_inventory.tsv", run_dir / "methodology_map.tsv",
                                   run_dir / "occ_ess_inventory.tsv"}) {
    std::error_code error;
    if (!fs::is_regular_file(required, error) || fs::file_size(required, error) == 0u) {
      return Err(ErrorCode::NotFound, "missing final artifact " + required.string());
    }
  }
  if (quality.n_admitted != manifest.n_ok) {
    return Err(ErrorCode::InvalidArgument, "quality/manifest admitted count mismatch");
  }
  ATX_TRY(ListedDispersionSchedule schedule,
          read_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string()));
  std::printf("verified artifact envelope: dates=%zu admitted=%u rolls=%zu\n", clock.size(),
              quality.n_admitted, schedule.rolls.size());
  return Ok();
}

Status run_backtest_command(const fs::path &run_dir, const fs::path &cache_dir) {
  const auto cmd_start = PhaseTimer::now();
  PhaseTimer timer(kRunBacktestPhases);

  // Two disjoint setup_read segments around the definitions read (as in
  // build_schedule_command): definitions_parse + setup_read sum to the old
  // single setup_read span.
  auto phase = PhaseTimer::now();
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  timer.add("setup_read", phase);

  // Task 8: see build_schedule_command's comment on `--cache`/`ATX_VOL_CACHE`.
  const auto definitions_start = PhaseTimer::now();
  ATX_TRY(ListedDefinitionTable definitions,
          read_listed_definitions_cached((run_dir / "definitions.tsv").string(), cache_dir.string(),
                                         kDefinitionsCacheFingerprintDefault, &timer));
  timer.add("definitions_parse", definitions_start, 1u);

  phase = PhaseTimer::now();
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY(ListedDispersionSchedule schedule,
          read_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string()));
  ATX_TRY(ListedDispersionStrategy strategy,
          ListedDispersionStrategy::create(schedule, spec.delta_band));
  RunConfig config;
  config.unpriced = UnpricedLotPolicy::Error;
  config.snapshot_cache = std::make_shared<SnapshotCache>();
  timer.add("setup_read", phase);

  phase = PhaseTimer::now();
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
  if (!strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "backtest did not consume every scheduled roll");
  }
  timer.add("engine_run", phase, backtest.size());

  // The re-mark pass: per date, load the certified surface snapshot and join the
  // OPRA parquet tape, then fold both into the reconciliation. These are three
  // different costs with three different optimisations, so they are charged to
  // three DISJOINT phases (`snapshot_load` / `quote_join` / `reconcile`) that
  // together partition what the old aggregate `reconciliation` phase measured.
  // Each per-date phase counts one unit per session, so ms/unit reads as
  // ms/session; `reconcile` keeps the old phase's clock.size() count so its
  // per-session figure stays comparable across the split.
  //
  // The symbol list and the owner reserves are quote-join setup (the symbols are
  // the join's key set), so they are charged to `quote_join` rather than left
  // uncharged — that keeps the partition exact rather than approximate.
  const auto join_setup_start = PhaseTimer::now();
  const std::vector<std::string> symbols = all_symbols(universe_rows, spec.index_symbol);
  // The exact contract set this re-mark pass will read: every leg of every roll.
  // The OPRA panels carry the whole listed universe for these symbols — on the
  // Wave E fixture the unfiltered join emitted 41k quotes per session against a
  // schedule of 66 legs — so handing the join the key set lets it skip the
  // definition lookup, the OSI parse and quote construction for everything else.
  // Read the `wanted` contract in listed_opra.hpp first: it also NARROWS six of
  // the join's seven fatal exits to these keys, and it REQUIRES a strictly
  // increasing span — hence the sort + unique below, which is enforced, not
  // advisory.
  //
  // The UNION over ALL rolls, not the active cohort. A roll date marks both the
  // held and the entering cohort (listed_dispersion_reconciliation.cpp), so a
  // per-date cohort set would be a cohort-boundary reasoning error waiting to
  // happen; the union is one key per leg per roll and trivially small.
  std::vector<ListedQuoteKey> wanted;
  for (const ListedScheduleRoll &roll : schedule.rolls) {
    for (const ListedScheduleLeg &leg : roll.legs) {
      wanted.push_back(quote_key_of(leg));
    }
  }
  std::sort(wanted.begin(), wanted.end());
  wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
  std::vector<std::shared_ptr<const MarketSnapshot>> snapshot_owners;
  std::vector<std::vector<ListedOptionQuote>> quote_owners;
  snapshot_owners.reserve(clock.size());
  quote_owners.reserve(clock.size());
  timer.add("quote_join", join_setup_start);
  for (const SnapshotRef &ref : clock.refs()) {
    const auto snapshot_start = PhaseTimer::now();
    ATX_TRY(std::shared_ptr<const MarketSnapshot> snapshot,
            config.snapshot_cache->load(ref.archive_path, config.query_pricing_tier));
    snapshot_owners.push_back(std::move(snapshot));
    timer.add("snapshot_load", snapshot_start, 1u);

    const auto quote_start = PhaseTimer::now();
    ATX_TRY(std::vector<ListedOptionQuote> quotes,
            listed_quotes_for_date(spec, definitions, symbols, ref.date, wanted));
    quote_owners.push_back(std::move(quotes));
    timer.add("quote_join", quote_start, 1u);
  }
  const auto reconcile_start = PhaseTimer::now();
  std::vector<ListedReconciliationSnapshot> reconciliation_snapshots;
  reconciliation_snapshots.reserve(clock.size());
  for (std::size_t i = 0; i < clock.size(); ++i) {
    reconciliation_snapshots.push_back(
        ListedReconciliationSnapshot{clock.refs()[i].date, snapshot_owners[i]->ts_ns(),
                                     &snapshot_owners[i]->set(), quote_owners[i]});
  }
  // M1 wired into production (design §3): the reconciler is fed the FULL clock.refs()
  // timeline (every session, including any leading warm-up / low-coverage session
  // before the first roll). reconcile_listed_schedule trims that lead-in down to the
  // first roll date before reconciling, so a warm-up session no longer aborts an
  // otherwise-valid corpus (the old reconcile_listed_dispersion hard-required the
  // front date to equal the first roll date).
  ATX_TRY(ListedDispersionReconciliation reconciliation,
          reconcile_listed_schedule(schedule, reconciliation_snapshots));
  ATX_TRY_VOID(validate_listed_reconciliation_backtest(reconciliation, backtest));
  timer.add("reconcile", reconcile_start, clock.size());

  // Hard cutover: the loose backtest.tsv / reconciliation.tsv / contract_marks.tsv
  // result files are replaced by the run.atxrun result container. The economic
  // sections plus the resolved-spec echo (meta) and the schedule (so the report
  // layer reads coverage straight from the archive) are staged and published
  // atomically via RunDir. `backtest` is spanned in place by
  // encode_backtest_section, so it must outlive the write — it is local here.
  phase = PhaseTimer::now();
  std::vector<RaSectionData> sections;
  sections.push_back(encode_schedule_section("trade_schedule", schedule));
  sections.push_back(encode_backtest_section("backtest", backtest));
  sections.push_back(encode_reconciliation_section(reconciliation));
  sections.push_back(encode_contract_marks_section(reconciliation));
  sections.push_back(encode_meta_section(spec));
  timer.add("write_outputs", phase);
  sections.push_back(encode_diagnostics_section(timer, "run_backtest", backtest.size()));
  ATX_TRY_VOID(RunDir(run_dir).write_run_archive(sections));

  std::printf("backtest complete: dates=%zu rolls=%zu final_nav=%.10g\n", backtest.size(),
              schedule.rolls.size(), backtest.nav.back());
  print_diag_summary("run_backtest", PhaseTimer::now() - cmd_start, backtest.size(), "session",
                     "sessions");
  return Ok();
}

// Projected-definition schedule (route P canonical): take each frozen listed roll and
// reprice its members at the surface ATM-forward strike (the exact interpolated strike
// instead of the nearest listed grid strike) with COLD certified greeks, keeping
// roll_date / valuation_ts_ns / cohort / expiry_ts_ns / n_names / weights / side
// identical to the listed build. The projected portfolio differs from the listed one
// ONLY by contract idealization — not by tenor and not by solver tier. The listed
// sizing rule (build_listed_dispersion_roll) is reused verbatim, so projected_schedule
// .tsv is the exact ATX_LISTED_DISPERSION_SCHEDULE format and passes the shared
// validator (net vega ~ 0, gross = 2x target); only per-member strike and its cold
// greeks differ from trade_schedule.tsv.
Status project_schedule_command(const fs::path &run_dir) {
  const auto cmd_start = PhaseTimer::now();
  PhaseTimer timer({"setup_read", "archive_load", "cold_solve", "validate_write"});

  auto phase = PhaseTimer::now();
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY(ListedDispersionSchedule listed,
          read_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string()));

  std::map<std::string, std::string> archive_of;
  for (const SnapshotRef &ref : clock.refs()) {
    archive_of.emplace(ref.date, ref.archive_path);
  }

  // Owning SINGLE-SLOT per-roll snapshot cache for the projection. The
  // ListedArchiveLookup hands project_listed_schedule a BORROWED MarketSnapshot*,
  // which must stay valid until the next lookup call — the projection dereferences
  // it only while processing that one roll and never retains it afterwards (the
  // rolls it emits are plain data), so exactly one board needs to be resident at a
  // time. A cumulative cache kept every roll-date board alive for the whole call;
  // each is a full heap deserialize (not an mmap), so peak memory scaled with the
  // roll count — harmless at 7 rolls, ~120 boards resident on a multi-year corpus.
  // Re-emplacing releases the previous board before the new one is stored. A roll
  // date absent from the qualified clock returns Ok(nullptr);
  // project_listed_schedule turns that into the example's exact NotFound message
  // ("no qualified archive for roll date ...") (O2).
  std::string cached_date;
  std::optional<MarketSnapshot> cached_snapshot;
  const ListedArchiveLookup archive_lookup =
      [&](std::string_view roll_date) -> Result<const MarketSnapshot *> {
    const std::string key(roll_date);
    const auto archive = archive_of.find(key);
    if (archive == archive_of.end()) {
      const MarketSnapshot *none = nullptr;
      return Ok(none);
    }
    if (cached_snapshot.has_value() && cached_date == key) {
      const MarketSnapshot *hit = &*cached_snapshot;
      return Ok(hit);
    }
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(archive->second));
    cached_snapshot.emplace(std::move(snapshot)); // frees the previous roll's board
    cached_date = key;
    const MarketSnapshot *loaded = &*cached_snapshot;
    return Ok(loaded);
  };
  timer.add("setup_read", phase);

  // Cold reprice in the library (listed_dispersion_pipeline). ProjectionConfig{} is the
  // single asserted parity constant (analytic + ColdReference) that the
  // run-projected-backtest --execution cold replay ALSO reads (I1), so one config
  // governs both cold routes and no hardcoded analytic/ColdReference literal survives
  // in the projection path. The per-roll snapshot load / valuation-ts / residual-tenor
  // / roll-shape guards, member restrike to the surface ATM forward, cold certified
  // greeks, listed sizing (build_listed_dispersion_roll) and the schedule validator all
  // run inside the call; the per-roll stdout diagnostic line prints from within it. The
  // whole call is charged to cold_solve (archive_load's per-load subset is no longer
  // separable now the loop lives in the library — only build-schedule per-phase
  // granularity was a T9 obligation; the diagnostics SECTION format is unchanged).
  const auto solve_start = PhaseTimer::now();
  ATX_TRY(ListedDispersionSchedule projected,
          project_listed_schedule(listed, archive_lookup, ProjectionConfig{}));
  std::uint64_t projected_legs = 0;
  for (const ListedScheduleRoll &roll : listed.rolls) {
    projected_legs += roll.legs.size();
  }
  timer.add("cold_solve", solve_start, projected_legs);

  const auto write_start = PhaseTimer::now();
  // projected_schedule.tsv stays a text INPUT: run-projected-backtest reads it
  // back via --schedule (the retained input read path). It is ALSO folded into
  // run.atxrun as the projected_schedule section.
  ATX_TRY_VOID(write_listed_dispersion_schedule_file(
      (run_dir / "projected_schedule.tsv").string(), projected));
  std::vector<RaSectionData> sections;
  sections.push_back(encode_schedule_section("projected_schedule", projected));
  timer.add("validate_write", write_start);
  sections.push_back(encode_diagnostics_section(timer, "project_schedule", listed.rolls.size()));
  ATX_TRY_VOID(RunDir(run_dir).write_run_archive(sections));
  std::printf("projected schedule built: rolls=%zu\n", projected.rolls.size());
  print_diag_summary("project_schedule", PhaseTimer::now() - cmd_start, listed.rolls.size(), "roll",
                     "rolls");
  return Ok();
}

// Projected replay of a listed-format schedule. `--execution configured` (default) is
// the Task 2 diagnostic: reprice through the fast cached-surrogate tier under
// QueryExecution::Configured (genuine interpolation) — its fast-tier accuracy gap is
// under separate investigation. `--execution cold` is route P canonical: no fast tier,
// QueryExecution::ColdReference with ScheduleMarkPolicy::Record (Configured-required
// economics permitted with a cold price execution while no fast tier is prepared).
// Records per-roll mark divergence between the frozen schedule marks and the live seed
// marks. `--schedule` selects the input schedule (default trade_schedule.tsv);
// `--out` is a provenance label only — no file is written under it; the name is
// recorded in the run.atxrun `meta` section (requested_out) so the request stays
// visible.
//
// MARK DIVERGENCE HAS EXACTLY ONE SOURCE (L10, Wave D T5): the `StepObserver` that
// rides the priced run. There is no second pass. The shadow replay that used to
// re-walk the clock, re-load every archive and re-step a private
// ListedDispersionStrategy purely to recompute rows the priced run already computed
// is DELETED — it was proven bit-exactly redundant on a 135-session production
// corpus (137 rows across 7 rolls and 11 underlyings) before removal, and the
// library-side observer carries its own fail-closed guards
// (make_mark_divergence_observer: roll cursor + valuation timestamp).
// `--no-divergence` therefore now means "do not install the observer", leaving the
// bare priced backtest — the bare-backtest wall-time path. The priced run never
// consulted the shadow, and `Strategy.StepObserverAbsentIsBitIdentical` pins that
// installing the observer does not perturb it, so the backtest output is
// byte-identical either way.
Status run_projected_backtest_command(const fs::path &run_dir, const fs::path &schedule_file,
                                      const std::string &execution, const fs::path &out_file,
                                      bool skip_divergence) {
  const auto cmd_start = PhaseTimer::now();
  // PHASE LIST IS BYTE-STABLE AND STAYS THAT WAY. The `diagnostics` section emits
  // one row per PRE-DECLARED phase, so adding, removing or renaming a name here
  // changes that section's row set — a RunArchive-visible artifact change, not a
  // cosmetic one. It is therefore held at exactly the five names the shadow-replay
  // era declared, even though T5's deletion changed what two of them measure:
  //   * `divergence_replay` no longer times a second pass over the clock. It now
  //     accumulates the per-step observer-callback time charged inside the observer
  //     wrapper below (count still == sessions, one add per step).
  //   * `archive_load` legitimately reads 0/0. Its per-load adds lived in the
  //     deleted shadow loop; the snapshot loads now belong exclusively to the
  //     priced run and are charged to `priced_run`, exactly as they already were
  //     under `--no-divergence`. This is the same benign zero-phase pattern Wave B
  //     accepted for project-schedule's `archive_load` once its loop moved into the
  //     library. Renaming or dropping the phase to "fix" the zero would break the
  //     section's row set for no measurement gain.
  PhaseTimer timer(
      {"setup_read", "divergence_replay", "archive_load", "priced_run", "write_outputs"});

  if (execution != "configured" && execution != "cold") {
    return Err(ErrorCode::InvalidArgument,
               "run-projected-backtest: --execution must be 'configured' or 'cold'");
  }
  const bool cold = execution == "cold";
  auto phase = PhaseTimer::now();
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY(ListedDispersionSchedule schedule,
          read_listed_dispersion_schedule_file((run_dir / schedule_file).string()));

  // Query route for the priced run — the only run there is.
  RunConfig config;
  config.unpriced = UnpricedLotPolicy::Error;
  config.snapshot_cache = std::make_shared<SnapshotCache>();
  if (cold) {
    // Route P canonical (I1): cold certified economics both sides, no fast tier
    // attached. The SAME ProjectionConfig{} constant that project_listed_schedule
    // authored the persisted projected_schedule marks through governs this replay's
    // cold seed — one config drives BOTH cold routes (execution AND analytic), so the
    // persisted marks and the marks this replay recomputes cannot silently drift (the
    // I1 root cause: two hand-maintained copies). `analytic` equals RunConfig::price's
    // default (true), so setting it explicitly is economically a no-op that removes the
    // last hardcoded ColdReference/analytic literal from the replay path.
    // Record policy reprices the projected definitions through the ColdReference route.
    // required_economic_execution() == Configured means "no cold requirement": the
    // engine gate only enforces anything when a strategy requires ColdReference, so a
    // Record strategy runs under any execution, including this explicit cold override.
    const ProjectionConfig cold_cfg;
    config.price.query_execution = cold_cfg.execution;
    config.price.analytic_greeks = cold_cfg.analytic;
  } else {
    // Attaching the prepared fast tier (with_query_pricing, propagated by
    // MarketSnapshot::load with no silent cold fallback) makes the Configured queries
    // genuinely interpolate the cached surrogate instead of reproducing the cold
    // archive marks.
    config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
    config.price.query_execution = QueryExecution::Configured;
  }
  timer.add("setup_read", phase);

  // Staging for the `mark_divergence` section. Filled AFTER the priced run from
  // `observed` (there is nothing to stage before the run produces the rows); the
  // section itself is still hand-built from this arena by the unchanged
  // build_mark_divergence_section, so the emitted artifact's construction is
  // untouched by T5 — only its upstream row source moved.
  auto divergence_arena = std::make_shared<MarkDivergenceArena>();
  // The observer's sink — now the SOLE mark-divergence source, filled only by the
  // priced run's step_observer. Declared out here because both it and `schedule`
  // are borrowed by the observer and must outlive the run_backtest call that
  // consumes it.
  std::vector<ListedMarkDivergenceRow> observed;
  // Callback count for the observer-coverage gate after the priced run — the half of
  // the evidence-channel contract that `all_rolls_consumed()` structurally cannot
  // supply (see the two-gate comment at the priced run). A DEDICATED counter, not a
  // read-back of the `divergence_replay` phase's count: `PhaseTimer::phases()` does
  // expose one, but that phase list is held byte-stable for the `diagnostics`
  // ARTIFACT, so keying a fail-closed correctness gate off a phase NAME would let a
  // future rename there silently disarm it. Same lifetime story as `observed` — the
  // wrapper borrows it and both outlive the run_backtest call.
  std::size_t observer_calls = 0u;
  if (!skip_divergence) {
    // `schedule` is the very object ListedDispersionStrategy::create copies below,
    // so the observer's schedule and the strategy's are value-equal by construction
    // — which is the invariant make_mark_divergence_observer's two fail-closed
    // guards (roll cursor, valuation timestamp) rest on.
    //
    // Wrapped, not assigned directly, purely to keep the `divergence_replay`
    // diagnostics phase measuring something real: it now accumulates the
    // observer-callback time, one add per observed step, so its count still equals
    // the session count the shadow-replay era recorded. The wrapper is pure
    // measurement — it forwards the event and the Status unchanged, so a
    // fail-closed guard inside the observer still aborts the run through
    // run_backtest's ATX_TRY_VOID exactly as it would unwrapped. The inner
    // observer is captured BY VALUE so the wrapper owns it; `timer` is a local of
    // this function and outlives `config`.
    //
    // The wrapper is ALSO where collection coverage is witnessed: `observer_calls` is
    // bumped in lockstep with the phase's unit count, so the two can never disagree.
    // Counting here rather than inside make_mark_divergence_observer is deliberate —
    // this is the site that is conditional, so it is the site whose absence has to be
    // observable.
    config.step_observer = [&timer, &observer_calls,
                            inner = make_mark_divergence_observer(schedule, observed)](
                               const StepEvent &event) -> Status {
      const auto cb_start = PhaseTimer::now();
      Status observed_status = inner(event);
      ++observer_calls;
      timer.add("divergence_replay", cb_start, 1u);
      return observed_status;
    };
  }

  // Primary priced run: the same strategy-aware engine as run-backtest, under the
  // Record policy so the interpolated live seed marks (not the frozen schedule
  // marks) seed each entry, and Configured economics end to end.
  const auto priced_start = PhaseTimer::now();
  ATX_TRY(ListedDispersionStrategy strategy,
          ListedDispersionStrategy::create(schedule, spec.delta_band, ScheduleMarkPolicy::Record));
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
  // EVIDENCE CHANNEL FOR THE ZERO-DIVERGENCE CLAIM — TWO GATES, ONE CONTRACT.
  // An empty `mark_divergence` section must mean "every roll fired and none diverged",
  // never "nothing was looking". It takes BOTH checks below to say that, and neither
  // one alone is sufficient:
  //
  //   (1) ROLL COVERAGE — `all_rolls_consumed()`, carried from the deleted shadow
  //       replay's post-loop gate (the message differs because the subject does: one
  //       run, not a replay). It interrogates the priced STRATEGY: every scheduled
  //       roll actually fired, so no roll went unexamined by the object the observer
  //       reads.
  //   (2) COLLECTION COVERAGE — `observer_calls == clock.size()`. It interrogates the
  //       observer WRAPPER: the observer was installed and fired on every session.
  //
  // (2) exists because (1) structurally cannot supply it. The strategy does not know
  // whether an observer was ever attached, so on its own (1) cannot distinguish "the
  // observer ran and legitimately found zero divergences" — the normal, expected cold
  // outcome — from "the observer was never installed, so of course the section is
  // empty". The shadow's single gate COULD distinguish them, because the object it
  // interrogated was the object its own collection loop had just walked; moving
  // collection into `config.step_observer` split that one guarantee into two, and only
  // one of the two halves came across with the comment.
  //
  // What the pair deliberately does NOT claim: that the observer's detection logic is
  // itself correct. That is the observer's own contract — its roll-cursor and
  // valuation-timestamp guards fail CLOSED through run_backtest's ATX_TRY_VOID, so a
  // fired-but-misaligned observer aborts the run rather than under-reporting, and the
  // ListedDispersionPipeline observer tests pin the row content.
  if (!strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "projected backtest did not consume every scheduled roll");
  }
  // Skipped entirely under --no-divergence: no observer is installed there BY DESIGN
  // and no `mark_divergence` section is written, so there is no empty-section claim to
  // protect and a zero count is the correct state, not a failure.
  if (!skip_divergence && observer_calls != clock.size()) {
    return Err(ErrorCode::Unavailable,
               "the mark-divergence observer did not fire on every session (" +
                   std::to_string(observer_calls) + " of " + std::to_string(clock.size()) +
                   "), so an empty mark_divergence section cannot be read as 'no divergence'");
  }
  timer.add("priced_run", priced_start, backtest.size());

  // Stage the observer's rows into the arena, in kMarkDivergenceCols REGISTRY
  // ORDER — the same order, the same dict_intern and the same Side->u8 mapping the
  // deleted shadow staged with, so build_mark_divergence_section (unchanged) sees a
  // bit-identical arena and the emitted section is byte-identical to the shadow era's.
  // Positional and append-only: the observer accumulates in step order and, within a
  // step, in the strategy's divergence order, and that order IS part of the pinned
  // artifact, so nothing here sorts, joins or de-duplicates.
  //
  // Placed after timer.add("priced_run", ...) on purpose: `observed` only exists once
  // the run has finished, and charging this staging to no phase keeps every phase's
  // meaning exactly what the phase list declares.
  if (!skip_divergence) {
    MarkDivergenceArena &arena = *divergence_arena;
    for (const ListedMarkDivergenceRow &row : observed) {
      arena.date_codes.push_back(dict_intern(arena.date_dict, row.date));
      arena.symbol_codes.push_back(dict_intern(arena.symbol_dict, row.symbol));
      arena.raw_symbol_codes.push_back(dict_intern(arena.raw_symbol_dict, row.raw_symbol));
      arena.strike.push_back(row.strike);
      arena.expiry_ts_ns.push_back(row.expiry_ts_ns);
      arena.side_codes.push_back(row.side == Side::Call ? std::uint8_t{0} : std::uint8_t{1});
      arena.schedule_mark.push_back(row.schedule_mark);
      arena.live_mark.push_back(row.live_mark);
      arena.diff.push_back(row.diff);
      arena.abs_diff_bps_of_mark.push_back(row.abs_diff_bps_of_mark);
      ++arena.n_rows;
    }
    // Evidence line, inherited from the deleted arbiter's verdict line. There is no
    // longer a second source to compare against, so this reports the one number a
    // reader of a stdout-only transcript still needs: how many rows the sole source
    // produced. Zero remains legitimate on either route — a row exists only where
    // the live seed mark differs from the frozen schedule mark, so a schedule
    // replayed on the tier that authored it reproduces its own marks — and it is now
    // exactly as informative as the section it describes, with no equivalence claim
    // attached to be vacuous about.
    std::printf("mark divergence rows: %llu\n", static_cast<unsigned long long>(arena.n_rows));
  }

  // Hard cutover: the loose projected_backtest.tsv / mark_divergence.tsv result
  // files are replaced by the run.atxrun result container. The priced backtest is
  // named for the observer's presence — the registry's two projected
  // variants: projected_cold (the canonical cold divergence route) or
  // projected_nodiv (--no-divergence, the bare priced run). The execution tier
  // (cold vs the diagnostic fast tier) and the requested --out name are recorded
  // in meta so --out/--execution stay provenance-visible. `backtest` is spanned in
  // place by encode_backtest_section (local, so it outlives the write).
  const auto write_start = PhaseTimer::now();
  const std::string projected_section = skip_divergence ? "projected_nodiv" : "projected_cold";
  const std::vector<std::pair<std::string, std::string>> meta_extra = {
      {"projected_execution", execution},
      {"skip_divergence", skip_divergence ? "1" : "0"},
      {"requested_out", out_file.string()}};
  std::vector<RaSectionData> sections;
  sections.push_back(encode_backtest_section(projected_section, backtest));
  if (!skip_divergence) {
    sections.push_back(build_mark_divergence_section(divergence_arena));
  }
  sections.push_back(encode_meta_section(spec, meta_extra));
  timer.add("write_outputs", write_start);
  sections.push_back(encode_diagnostics_section(timer, "run_projected_backtest", backtest.size()));
  ATX_TRY_VOID(RunDir(run_dir).write_run_archive(sections));
  std::printf("projected backtest complete [%s]: dates=%zu rolls=%zu final_nav=%.10g\n",
              execution.c_str(), backtest.size(), schedule.rolls.size(), backtest.nav.back());
  print_diag_summary("run_projected_backtest", PhaseTimer::now() - cmd_start, backtest.size(),
                     "session", "sessions");
  return Ok();
}

Status run_surface_backtest_command(const fs::path &run_dir) {
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  if (clock.size() == 0u) {
    return Err(ErrorCode::Unavailable, "surface backtest: empty qualified clock");
  }
  ATX_TRY(DispersionUniverse universe,
          universe_at(universe_rows, clock.refs().front().date, spec.index_symbol));

  DispersionBacktestConfig config;
  config.target_dte_days = spec.target_dte_days;
  config.roll_dte_days = spec.roll_dte_days;
  // spec.gross_index_vega is dollars vega per VOL POINT per side; the library
  // dispersion configs take dollars vega per UNIT vol (a unit vol is 100 vol
  // points), so scale by the per-vol-point -> per-unit-vol factor at this boundary
  // (M9/I4: the named constant replaces the hand-applied * 100.0 literal).
  config.gross_index_vega = spec.gross_index_vega * kVegaVolPointToUnitVol;
  config.delta_band = spec.delta_band;
  config.min_names = spec.min_names;
  config.run.unpriced = UnpricedLotPolicy::Error;
#if defined(ATX_VOL_PROFILE)
  phase_profile::reset();
#endif
#if defined(ATX_VOL_COUNTERS)
  counters::reset();
#endif
  ATX_TRY(BacktestResult backtest, run_dispersion_backtest(clock, std::move(universe), config));
#if defined(ATX_VOL_PROFILE)
  {
    const phase_profile::Snapshot measured = phase_profile::snapshot();
    const double total_ns = static_cast<double>(
        measured.nanoseconds[static_cast<unsigned>(phase_profile::Region::BacktestTotal)]);
    std::ofstream output(run_dir / "backtest_profile.tsv", std::ios::binary | std::ios::trunc);
    if (!output)
      return Err(ErrorCode::IoError, "cannot write backtest profile");
    output << "region\tcalls\ttotal_ms\tpct_backtest\tns_per_call\n" << std::setprecision(17);
    for (unsigned i = 0; i < phase_profile::kCount; ++i) {
      const double ns = static_cast<double>(measured.nanoseconds[i]);
      const double calls = static_cast<double>(measured.calls[i]);
      output << phase_profile::kNames[i] << '\t' << measured.calls[i] << '\t' << ns / 1.0e6 << '\t'
             << (total_ns > 0.0 ? 100.0 * ns / total_ns : 0.0) << '\t'
             << (calls > 0.0 ? ns / calls : 0.0) << '\n';
    }
    if (!output)
      return Err(ErrorCode::IoError, "cannot flush backtest profile");
  }
#endif
#if defined(ATX_VOL_COUNTERS)
  {
    const counters::Snapshot measured = counters::snapshot();
    std::ofstream output(run_dir / "backtest_counters.tsv", std::ios::binary | std::ios::trunc);
    if (!output)
      return Err(ErrorCode::IoError, "cannot write backtest counters");
    output << "counter\tvalue\n";
    for (unsigned i = 0; i < counters::kCount; ++i)
      output << counters::kNames[i] << '\t' << measured.values[i] << '\n';
    if (!output)
      return Err(ErrorCode::IoError, "cannot flush backtest counters");
  }
#endif
  ATX_TRY_VOID(write_backtest_tsv(backtest, (run_dir / "surface_backtest.tsv").string()));
  std::printf("surface-only projected backtest complete: dates=%zu final_nav=%.10g\n",
              backtest.size(), backtest.nav.back());
  return Ok();
}

Status run_projected_var_command(const fs::path &run_dir) {
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  if (clock.size() == 0u)
    return Err(ErrorCode::Unavailable, "projected VaR: empty qualified clock");

  std::vector<std::unique_ptr<MarketSnapshot>> snapshots;
  std::vector<HistoricalProjectionScenario> scenarios;
  snapshots.reserve(clock.size());
  scenarios.reserve(clock.size());
  for (const SnapshotRef &ref : clock.refs()) {
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(ref.archive_path));
    snapshots.push_back(std::make_unique<MarketSnapshot>(std::move(snapshot)));
    scenarios.push_back({snapshots.back()->ts_ns(), &snapshots.back()->set()});
  }

  ATX_TRY(DispersionUniverse authored,
          universe_at(universe_rows, clock.refs().front().date, spec.index_symbol));
  ATX_TRY(ResolvedUniverse resolved,
          resolve_universe_uids(
              authored, [&](std::string_view symbol) { return snapshots.front()->uid_of(symbol); },
              MissingNameSpec{MissingNamePolicy::DropRenormalize, spec.min_names}));
  DispersionConfig dispersion;
  dispersion.target_T = spec.target_dte_days / 365.25;
  // spec.gross_index_vega is dollars vega per VOL POINT per side; the library
  // dispersion configs take dollars vega per UNIT vol (a unit vol is 100 vol
  // points), so scale by the per-vol-point -> per-unit-vol factor at this boundary
  // (M9/I4: the named constant replaces the hand-applied * 100.0 literal). The scaled
  // book is what dispersion_book_var re-projects — the library applies no further x100.
  dispersion.target_vega = spec.gross_index_vega * kVegaVolPointToUnitVol;
  dispersion.side = DispersionSide::ShortIndexLongNames;
  dispersion.multiplier = 100.0;
  dispersion.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, spec.min_names};
  dispersion.projected_maturity =
      ProjectedMaturitySpec::days(static_cast<std::int32_t>(std::llround(spec.target_dte_days)));
  ATX_TRY(DispersionBook initial,
          build_dispersion_book(resolved.universe, snapshots.front()->set(), dispersion));

  // The relative-template synthesis + prepare + evaluate + per-confidence VaR split
  // now live in the library (dispersion_book_var). The CLI keeps the three bespoke
  // loose-TSV emissions (out-of-archive per the design partition rule — no schema
  // bump this wave). `maturity` MUST stay the relative days(N) template, NOT the
  // book's absolute expiry, or per-scenario aging would change. elapsed_seconds now
  // spans the whole call (prepare + evaluate + risk) — projections_per_second is
  // non-deterministic wall-clock telemetry, unaffected economically.
  const std::vector<double> confidences = {0.95, 0.99};
  HistoricalProjectionConfig config;
  config.n_threads = spec.fit_workers;
  const auto started = std::chrono::steady_clock::now();
  ATX_TRY(DispersionBookVar var,
          dispersion_book_var(initial, *dispersion.projected_maturity, scenarios, confidences,
                              config));
  const double elapsed_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  std::ofstream frame_out(run_dir / "projected_risk_scenarios.tsv",
                          std::ios::binary | std::ios::trunc);
  std::ofstream leg_out(run_dir / "projected_risk_legs.tsv", std::ios::binary | std::ios::trunc);
  if (!frame_out || !leg_out)
    return Err(ErrorCode::IoError, "projected VaR: cannot open output");
  frame_out << std::setprecision(17)
            << "date\tts_ns\tvalue\tdelta\tgamma\tvega\ttheta\tn_ok\tn_failed\t"
               "definition_fingerprint\n";
  leg_out << std::setprecision(17)
          << "date\tleg\tuid\tside\texpiry_ts_ns\tstrike\tquantity\tmultiplier\tmark\t"
             "delta\tgamma\tvega\ttheta\tdefinition_fingerprint\tstatus\n";
  for (std::size_t scenario = 0; scenario < var.frames.size(); ++scenario) {
    const HistoricalProjectionFrame &frame = var.frames[scenario];
    frame_out << clock.refs()[scenario].date << '\t' << frame.ts_ns << '\t' << frame.value << '\t'
              << frame.delta << '\t' << frame.gamma << '\t' << frame.vega << '\t' << frame.theta
              << '\t' << frame.n_ok << '\t' << frame.n_failed << '\t'
              << frame.definition_fingerprint << '\n';
    for (std::size_t leg = 0; leg < var.n_positions; ++leg) {
      const ProjectedOption &projected = var.legs[scenario * var.n_positions + leg];
      leg_out << clock.refs()[scenario].date << '\t' << leg << '\t'
              << projected.definition.contract.uid << '\t'
              << (projected.definition.contract.side == Side::Call ? "Call" : "Put") << '\t'
              << projected.definition.expiry_ts_ns << '\t' << projected.definition.contract.K
              << '\t' << initial.positions[leg].qty << '\t' << projected.definition.multiplier
              << '\t' << projected.model_mark << '\t' << projected.greeks.delta << '\t'
              << projected.greeks.gamma << '\t' << projected.greeks.vega << '\t'
              << projected.greeks.theta << '\t' << projected.definition.fingerprint << '\t'
              << to_string(projected.status) << '\n';
    }
  }
  frame_out.close();
  leg_out.close();
  if (!frame_out || !leg_out)
    return Err(ErrorCode::IoError, "projected VaR: output write failed");

  std::ofstream summary(run_dir / "projected_var.tsv", std::ios::binary | std::ios::trunc);
  if (!summary)
    return Err(ErrorCode::IoError, "projected VaR: cannot open summary");
  summary << std::setprecision(17)
          << "confidence\treference_value\tvalue_at_risk\texpected_shortfall\tn_scenarios\t"
             "n_positions\tprojections_per_second\tprepared_fingerprint\n";
  for (const ProjectedHistoricalVar &risk : var.risks) {
    summary << risk.confidence << '\t' << risk.reference_value << '\t' << risk.value_at_risk << '\t'
            << risk.expected_shortfall << '\t' << risk.n_scenarios << '\t' << var.n_positions
            << '\t' << (static_cast<double>(var.legs.size()) / elapsed_seconds) << '\t'
            << var.prepared_fingerprint << '\n';
  }
  if (!summary)
    return Err(ErrorCode::IoError, "projected VaR: summary write failed");
  std::printf("projected relative-template VaR complete: scenarios=%zu positions=%zu rate=%.1f/s\n",
              var.frames.size(), var.n_positions,
              static_cast<double>(var.legs.size()) / elapsed_seconds);
  return Ok();
}

// runarchive dump <run_dir> <section> [--tsv]: the escape hatch. Opens
// <run_dir>/run.atxrun and either prints a one-line section summary (default) or,
// with --tsv, streams the section back out in a loose-TSV shape — columns in
// stored order, %.17g doubles / %lld i64 / %u u32 / decoded dict and enum
// strings. The byte-identical legacy TSV shape holds only for the backtest-schema
// sections (backtest / projected_cold / projected_nodiv): they reproduce
// write_backtest_tsv byte-for-byte (date + ts_ns + the 25 registry doubles + any
// per-signal series). Other sections (e.g. contract_marks / reconciliation) are
// NOT byte-identical to their legacy writers — an NA-able F64 stored as quiet NaN
// prints here as "nan", where those writers emit "NA".
// stdout is switched to binary so the emitted \n line endings are not translated.
Status runarchive_dump_command(const fs::path &run_dir, const std::string &section_name, bool tsv) {
  ATX_TRY(RunArchive archive,
          RunArchive::open_file((run_dir / std::string(kRunArchiveFile)).string()));
  ATX_TRY(RaSectionView view, archive.section(section_name));
  const std::span<const RaColumnDescriptor> cols = view.columns();

  if (!tsv) {
    std::printf("section %s: rows=%llu cols=%u\n", section_name.c_str(),
                static_cast<unsigned long long>(view.n_rows()), view.n_cols());
    for (const RaColumnDescriptor &cd : cols) {
      std::printf("  %.*s\n", static_cast<int>(cd.name_len), cd.name);
    }
    return Ok();
  }

  std::string out;
  for (std::size_t c = 0; c < cols.size(); ++c) {
    if (c != 0) {
      out += '\t';
    }
    out.append(cols[c].name, cols[c].name_len);
  }
  out += '\n';

  char buf[64];
  const std::uint64_t n = view.n_rows();
  for (std::uint64_t i = 0; i < n; ++i) {
    for (std::size_t c = 0; c < cols.size(); ++c) {
      if (c != 0) {
        out += '\t';
      }
      const std::string_view name(cols[c].name, cols[c].name_len);
      switch (cols[c].dtype) {
      case RaDType::F64: {
        const int len = std::snprintf(buf, sizeof buf, "%.17g", view.f64_col(name)[i]);
        out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
        break;
      }
      case RaDType::I64: {
        const int len =
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(view.i64_col(name)[i]));
        out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
        break;
      }
      case RaDType::U32: {
        const int len =
            std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(view.u32_col(name)[i]));
        out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
        break;
      }
      case RaDType::DictStr:
        out.append(view.dict_col(name).at(i));
        break;
      case RaDType::U8Enum:
        out.append(view.u8enum_labels(name).at(view.u8enum_col(name)[i]));
        break;
      }
    }
    out += '\n';
  }

#ifdef _WIN32
  std::fflush(stdout);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
  return Ok();
}

void usage() {
  std::fprintf(
      stderr, "usage:\n"
              "  atxvol_spy_dispersion_backtest build-corpus --spec FILE --out DIR\n"
              "  atxvol_spy_dispersion_backtest build-schedule --run DIR [--cache DIR]\n"
              "  atxvol_spy_dispersion_backtest run-backtest --run DIR [--cache DIR]\n"
              "  atxvol_spy_dispersion_backtest project-schedule --run DIR\n"
              "  atxvol_spy_dispersion_backtest run-projected-backtest --run DIR "
              "[--schedule FILE] [--execution cold|configured] [--out FILE] "
              "[--no-divergence]\n"
              "  atxvol_spy_dispersion_backtest run-surface-backtest --run DIR\n"
              "  atxvol_spy_dispersion_backtest run-projected-var --run DIR\n"
              "  atxvol_spy_dispersion_backtest verify --run DIR\n"
              "  atxvol_spy_dispersion_backtest runarchive dump DIR SECTION [--tsv]\n"
              "\n"
              "--cache DIR (build-schedule, run-backtest only): opt into the ATXDEFS1\n"
              "  pre-parsed definitions.tsv cache. Defaults to the ATX_VOL_CACHE\n"
              "  environment variable; DISABLED (today's behaviour, byte-for-byte) if\n"
              "  neither is set. Read this before turning it on:\n"
              "    * a HIT is a median 1.274x faster than not caching (n=3, sign 3/3,\n"
              "      p=0.125 on a one-sided sign test — WEAK, not a strong claim);\n"
              "    * the run that POPULATES the cache is a median 1.856x SLOWER than not\n"
              "      caching at all;\n"
              "    * peak working set on that populating run is roughly 3 GB against a\n"
              "      730 MB definitions.tsv;\n"
              "    * the cache directory has NO EVICTION POLICY and grows WITHOUT BOUND,\n"
              "      roughly 300 MB per distinct definitions.tsv ever seen.\n"
              "  See read_listed_definitions_cached's header comment (listed_definitions_"
              "cache.hpp) for the full disclosure.\n");
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];

  // runarchive dump <run_dir> <section> [--tsv]: positional args (not --flags),
  // so it is handled before the --flag parser below.
  if (command == "runarchive") {
    if (argc >= 5 && std::string_view(argv[2]) == "dump") {
      const bool tsv = argc >= 6 && std::string_view(argv[5]) == "--tsv";
      const Status st = runarchive_dump_command(argv[3], argv[4], tsv);
      if (!st) {
        std::fprintf(stderr, "%s\n", st.error().to_string().c_str());
        return 1;
      }
      return 0;
    }
    usage();
    return 2;
  }

  fs::path spec;
  fs::path run;
  fs::path out;
  fs::path schedule;
  std::string execution;
  bool no_divergence = false;
  // `--cache` default: the ATX_VOL_CACHE environment variable, itself
  // defaulting to empty (disabled). An explicit `--cache` below overrides
  // either. Cache-disabled-by-default is load-bearing (Task 8 ruling): no
  // existing invocation of this binary — including the controller's
  // parity_full_run.ps1, which never passes --cache — may change behaviour.
  fs::path cache = cache_dir_from_env();
  for (int i = 2; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--no-divergence") {  // value-less flag, checked before the value guard
      no_divergence = true;
      continue;
    }
    if (i + 1 >= argc) {
      usage();
      return 2;
    }
    if (argument == "--spec") {
      spec = argv[++i];
    } else if (argument == "--run") {
      run = argv[++i];
    } else if (argument == "--out") {
      out = argv[++i];
    } else if (argument == "--schedule") {
      schedule = argv[++i];
    } else if (argument == "--execution") {
      execution = argv[++i];
    } else if (argument == "--cache") {
      cache = argv[++i];
    } else {
      usage();
      return 2;
    }
  }
  Status status = Err(ErrorCode::InvalidArgument, "unknown command");
  if (command == "build-corpus" && !spec.empty() && !out.empty()) {
    status = build_corpus_command(spec, out);
  } else if (command == "build-schedule" && !run.empty()) {
    status = build_schedule_command(run, cache);
  } else if (command == "run-backtest" && !run.empty()) {
    status = run_backtest_command(run, cache);
  } else if (command == "project-schedule" && !run.empty()) {
    status = project_schedule_command(run);
  } else if (command == "run-projected-backtest" && !run.empty()) {
    status = run_projected_backtest_command(
        run, schedule.empty() ? fs::path("trade_schedule.tsv") : schedule,
        execution.empty() ? std::string("configured") : execution,
        out.empty() ? fs::path("projected_backtest.tsv") : out, no_divergence);
  } else if (command == "run-surface-backtest" && !run.empty()) {
    status = run_surface_backtest_command(run);
  } else if (command == "run-projected-var" && !run.empty()) {
    status = run_projected_var_command(run);
  } else if (command == "verify" && !run.empty()) {
    status = verify_command(run);
  } else {
    usage();
    return 2;
  }
  if (!status) {
    std::fprintf(stderr, "%s\n", status.error().to_string().c_str());
    return 1;
  }
  return 0;
}

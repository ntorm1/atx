// Real-data workflow for the traditional SPY listed-options dispersion proxy.
// Each command is a process boundary; no fitter/session object crosses it.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
#include "atx/vol/dispersion_run.hpp"
#include "atx/vol/dispersion_workflow.hpp"
#include "atx/vol/historical_projection.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_dispersion_pipeline.hpp"
#include "atx/vol/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/listed_dispersion_strategy.hpp"
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

// The archive-file fingerprint that build-schedule stamped as `surface_fingerprint`
// now lives in the library (listed_dispersion_pipeline.cpp `hash_archive_file`, an
// anonymous-namespace helper byte-for-byte identical to the old example `hash_file`).
// After the T9 build-schedule cutover the example had no remaining caller, so its
// duplicate `hash_file` was removed rather than exporting the library's internal
// helper (O5). `read_text` stays — `verify_occ_ess_evidence` still calls it.
//
// RECONCILE 1: `hash_text` went the same way. It existed only to seed build-corpus's
// input/policy fingerprints, and build-corpus is now a one-line dispatch into
// `dispersion_build_corpus`, which computes both from the NAMED constants on
// `DispersionCorpusPolicy` (dispersion_run.hpp) instead of from example literals.

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

// RECONCILE 1: `write_input_inventory` and `persist_occ_ess_evidence` were
// example-local duplicates of the library's own copies (src/dispersion_run.cpp:113
// and :135, the T-wave lift). Their only caller was `build_corpus_command`, which
// is now a dispatch into `dispersion_build_corpus`; the library writes both
// artifacts. `verify_occ_ess_evidence` below is NOT a duplicate in that sense —
// two shipped subcommands (`build-schedule`, `verify`) still call it here.

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

// RECONCILE 1 — `build-corpus` is a dispatch into `dispersion_build_corpus`, so the
// ~90-line body and `write_methodology_map` that used to live here are gone. The
// library entry point writes the same artifacts in the same order and adds the two
// things this copy could not reach:
//
//   * `persist_typed_spec_keys` (F4/BT-W). `write_resolved_spec` re-emits ONLY the
//     RunSpec vocabulary, so THIS copy silently ERASED every typed execution knob
//     (friction_*, financing_*, cost_*, provenance, unpriced, fill_policy, quote_*)
//     on the way into the run directory. A spec could declare frictions, be
//     accepted, and have them dropped before any later stage could read them.
//   * the pinned admission/fit constants as NAMED members of
//     `DispersionCorpusPolicy` rather than inline literals, which is what makes the
//     reproduction knobs greppable from one place.
//
// See the seam contract at the top of atx/vol/dispersion_run.hpp for why this
// subcommand dispatches and `build-schedule` / `run-backtest` / `verify` do not.

// The per-date OPRA quote join (formerly the example's `load_listed_quotes`) now
// lives in the library as `listed_quotes_for_date` (verbatim lift, T1). Both former
// consumers — build-schedule (via build_listed_dispersion_schedule) and run-backtest
// reconciliation — call the library seam, so the example's duplicate was removed (T9).

Status build_schedule_command(const fs::path &run_dir) {
  const auto cmd_start = PhaseTimer::now();
  PhaseTimer timer({"setup_read", "selection", "quote_join", "write_outputs"});

  auto phase = PhaseTimer::now();
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  // REV-FIXTAIL I-A: the SAME run_spec.tsv, also through the STRICT typed reader,
  // for the one thing the loose RunSpec cannot carry — F6's quote-quality
  // admission policy. `quote_min_bid` / `quote_max_age_ns` / `quote_reject_locked`
  // bound by name, survived `reject_unknown()` and were published into
  // `run_config.tsv` as EFFECTIVE, while their only consumer was the library-only
  // `dispersion_build_schedule`; on this route the declared policy reached no
  // selection at all. This is the same double read `run-backtest` already performs
  // (below, at the strict `read_dispersion_run_config`) and the same one the
  // library twin performs (dispersion_run.cpp's `dispersion_build_schedule`), so
  // the two bodies now agree on config construction. A malformed spec fails here
  // by name instead of two subcommands later.
  ATX_TRY(DispersionRunConfig run_config, read_dispersion_run_config(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  ATX_TRY(ListedDefinitionTable definitions,
          read_listed_definitions_file((run_dir / "definitions.tsv").string()));
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
  // REV-MTIDY I-1: the nine assignments that used to sit here — including
  // `sched_spec.quality = run_config.quote_quality`, the ONE line REV-FIXTAIL
  // I-A added to make the three `quote_*` keys reach a shipped selection — are
  // now `listed_schedule_spec_from` (dispersion_run.hpp). They were moved for a
  // measured reason: deleting that assignment from this file left the ENTIRE
  // gate green (2262/2262, 0 failed), because the gtests I-A shipped call
  // `listed_selection_config_from` one layer below it and the e2e CLI chain
  // drives only defaults, which equal the pre-fix behaviour. A knob repaired by
  // a line no test can observe can go inert again under a green gate.
  const ListedScheduleSpec sched_spec = listed_schedule_spec_from(spec, run_config);
  ATX_TRY(ListedDispersionSchedule schedule,
          build_listed_dispersion_schedule(clock, sched_spec, method, universe_rows, definitions,
                                           spec, &timer));

  const auto write_start = PhaseTimer::now();
  // trade_schedule.tsv stays a text INPUT: run-backtest / project-schedule /
  // run-projected-backtest read it back through read_listed_dispersion_schedule_
  // file (the retained input read path). The schedule is ALSO folded into
  // run.atxrun as the trade_schedule section (the result container). run-backtest
  // later republishes run.atxrun with the full economic result set.
  ATX_TRY_VOID(
      write_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string(), schedule));
  std::vector<RaSectionData> sections;
  sections.push_back(encode_schedule_section("trade_schedule", schedule));
  timer.add("write_outputs", write_start);
  sections.push_back(encode_diagnostics_section(timer, "build_schedule", schedule.rolls.size()));
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

Status run_backtest_command(const fs::path &run_dir) {
  const auto cmd_start = PhaseTimer::now();
  PhaseTimer timer({"setup_read", "engine_run", "reconciliation", "write_outputs"});

  auto phase = PhaseTimer::now();
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  ATX_TRY(ListedDefinitionTable definitions,
          read_listed_definitions_file((run_dir / "definitions.tsv").string()));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY(ListedDispersionSchedule schedule,
          read_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string()));
  // RECONCILE 2 (F4/BT-W + F5/BT-T2). This block used to be:
  //
  //     RunConfig config;
  //     config.unpriced = UnpricedLotPolicy::Error;
  //     config.snapshot_cache = std::make_shared<SnapshotCache>();
  //
  // i.e. exactly the pre-F4 defect WS-F closed and the main->pipeline-m merge
  // re-opened by taking main's example wholesale: `friction_*`, `financing_*`,
  // `cost_*`, `provenance`, `fill_policy`, `book_entry_fill_slippage` and
  // `reconcile_nav` were parsed, echoed into the run dir, and then had NO effect
  // on the headline listed artifact. Every published listed NAV was frictionless,
  // carry-free and provenance-permissive regardless of what the spec declared.
  //
  // The SAME run_spec.tsv is now also read through the STRICT typed reader, and
  // the engine config comes from the ONE named construction the F5 guard test
  // also calls (`make_listed_replay_run_config`), so a knob is either visible
  // there or provably dead, and the snapshot cache is subsetted to the schedule's
  // referenced uids. `read_dispersion_run_config` binds every RunSpec key too, so
  // a run dir written before this change reads identically; an unknown key now
  // fails BY NAME instead of being silently ignored.
  ATX_TRY(DispersionRunConfig run_config, read_dispersion_run_config(run_dir / "run_spec.tsv"));
  ATX_TRY(ListedDispersionStrategy strategy,
          ListedDispersionStrategy::create(schedule, spec.delta_band,
                                           ScheduleMarkPolicy::ExactArchive,
                                           run_config.fill_policy));
  RunConfig config = make_listed_replay_run_config(run_config, clock, strategy);
  // M4's reporting contract: the run records WHAT produced its numbers, regime
  // first, BEFORE the replay, so a failed run still leaves the evidence of what it
  // attempted. A published NAV that does not say which frictions produced it is
  // not a result — and until this line the listed route emitted no such record.
  ATX_TRY_VOID(write_dispersion_effective_config(run_dir / "run_config.tsv", run_config));
  timer.add("setup_read", phase);

  phase = PhaseTimer::now();
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
  if (!strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "backtest did not consume every scheduled roll");
  }
  timer.add("engine_run", phase, backtest.size());

  // reconciliation: the OPRA parquet join re-marking every session against the
  // exchange tape (load_listed_quotes per date), then folding those marks into
  // the reconciliation. This phase's cost dominating the subcommand is the claim
  // the diagnostics prove.
  phase = PhaseTimer::now();
  const std::vector<std::string> symbols = all_symbols(universe_rows);
  std::vector<std::shared_ptr<const MarketSnapshot>> snapshot_owners;
  std::vector<std::vector<ListedOptionQuote>> quote_owners;
  snapshot_owners.reserve(clock.size());
  quote_owners.reserve(clock.size());
  for (const SnapshotRef &ref : clock.refs()) {
    ATX_TRY(std::shared_ptr<const MarketSnapshot> snapshot,
            config.snapshot_cache->load(ref.archive_path, config.query_pricing_tier));
    snapshot_owners.push_back(std::move(snapshot));
    ATX_TRY(std::vector<ListedOptionQuote> quotes,
            listed_quotes_for_date(spec, definitions, symbols, ref.date));
    quote_owners.push_back(std::move(quotes));
  }
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
  timer.add("reconciliation", phase, clock.size());

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

// Drive the separate Record replay that produces the mark_divergence section: the
// engine's run_backtest loop hides per-step strategy state, so replay here and
// snapshot last_mark_divergences() after each roll step, collecting the rows into
// `arena` (the section is hand-built from it — mark_divergence is example-owned,
// with no library encoder). The per-session MarketSnapshot::load is accumulated
// into archive_load (count=loads), a measured subset of divergence_replay.
Status collect_mark_divergence_replay(const ListedDispersionSchedule &schedule, const Clock &clock,
                                      const RunConfig &config, double delta_band, PhaseTimer &timer,
                                      MarkDivergenceArena &arena) {
  const auto div_start = PhaseTimer::now();
  ATX_TRY(ListedDispersionStrategy divergence_strategy,
          ListedDispersionStrategy::create(schedule, delta_band, ScheduleMarkPolicy::Record));
  PortfolioState divergence_book;
  std::uint64_t divergence_next_id = 1;
  for (std::size_t i = 0; i < clock.size(); ++i) {
    const SnapshotRef &ref = clock.refs()[i];
    const auto load_start = PhaseTimer::now();
    ATX_TRY(MarketSnapshot snapshot,
            MarketSnapshot::load(ref.archive_path, config.query_pricing_tier));
    timer.add("archive_load", load_start, 1u);
    ATX_TRY_VOID(divergence_strategy.on_step(snapshot, i, divergence_book, divergence_next_id,
                                             config.price));
    const std::vector<MarkDivergence> &divergences = divergence_strategy.last_mark_divergences();
    if (divergences.empty()) {
      continue;
    }
    // Divergences are populated only on a roll step; the roll that just fired owns
    // the legs carrying each contract's symbol/raw_symbol.
    const ListedScheduleRoll &roll = schedule.rolls[divergence_strategy.next_roll_index() - 1u];
    for (const MarkDivergence &divergence : divergences) {
      const ListedScheduleLeg *matched = nullptr;
      for (const ListedScheduleLeg &leg : roll.legs) {
        if (leg.uid == divergence.uid && leg.strike == divergence.strike &&
            leg.expiry_ts_ns == divergence.expiry_ts_ns && leg.side == divergence.side) {
          matched = &leg;
          break;
        }
      }
      if (matched == nullptr) {
        return Err(ErrorCode::NotFound, "mark divergence leg not found in roll");
      }
      const double diff = divergence.live_mark - divergence.schedule_mark;
      const double denom = std::abs(divergence.schedule_mark);
      const double abs_diff_bps_of_mark = denom > 0.0 ? std::abs(diff) / denom * 1.0e4 : 0.0;
      arena.date_codes.push_back(dict_intern(arena.date_dict, ref.date));
      arena.symbol_codes.push_back(dict_intern(arena.symbol_dict, matched->symbol));
      arena.raw_symbol_codes.push_back(dict_intern(arena.raw_symbol_dict, matched->raw_symbol));
      arena.strike.push_back(divergence.strike);
      arena.expiry_ts_ns.push_back(divergence.expiry_ts_ns);
      arena.side_codes.push_back(divergence.side == Side::Call ? std::uint8_t{0} : std::uint8_t{1});
      arena.schedule_mark.push_back(divergence.schedule_mark);
      arena.live_mark.push_back(divergence.live_mark);
      arena.diff.push_back(diff);
      arena.abs_diff_bps_of_mark.push_back(abs_diff_bps_of_mark);
      ++arena.n_rows;
    }
  }
  // An empty mark_divergence section must mean "every roll fired and none
  // diverged", never "the replay silently skipped rolls" — it is the evidence
  // channel for the parity report's zero-divergence claim.
  if (!divergence_strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "divergence replay did not consume every scheduled roll");
  }
  timer.add("divergence_replay", div_start, clock.size());
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
// visible. `--no-divergence` skips the mark-divergence replay pass (and its
// `mark_divergence` section), leaving only the priced backtest — the
// bare-backtest wall-time path. The priced run is independent of the replay, so
// the backtest output is byte-identical either way.
Status run_projected_backtest_command(const fs::path &run_dir, const fs::path &schedule_file,
                                      const std::string &execution, const fs::path &out_file,
                                      bool skip_divergence) {
  const auto cmd_start = PhaseTimer::now();
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

  // Shared query route for the divergence replay and the priced run.
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

  // mark_divergence.tsv comes from a separate Record replay (write_mark_divergence_replay):
  // divergence_replay (count=sessions) times the whole replay and archive_load (count=loads)
  // is a measured subset of it. --no-divergence skips the replay entirely, leaving only the
  // priced backtest; the divergence_replay/archive_load phases then read 0 and the priced run
  // absorbs its own cold snapshot-cache loads.
  auto divergence_arena = std::make_shared<MarkDivergenceArena>();
  if (!skip_divergence) {
    ATX_TRY_VOID(collect_mark_divergence_replay(schedule, clock, config, spec.delta_band, timer,
                                                *divergence_arena));
  }

  // Primary priced run: the same strategy-aware engine as run-backtest, under the
  // Record policy so the interpolated live seed marks (not the frozen schedule
  // marks) seed each entry, and Configured economics end to end.
  const auto priced_start = PhaseTimer::now();
  ATX_TRY(ListedDispersionStrategy strategy,
          ListedDispersionStrategy::create(schedule, spec.delta_band, ScheduleMarkPolicy::Record));
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
  if (!strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "projected backtest did not consume every scheduled roll");
  }
  timer.add("priced_run", priced_start, backtest.size());

  // Hard cutover: the loose projected_backtest.tsv / mark_divergence.tsv result
  // files are replaced by the run.atxrun result container. The priced backtest is
  // named for the divergence-pass presence — the registry's two projected
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

// RECONCILE 1 -- `run-surface-backtest` and `run-projected-var` are dispatches into
// `dispersion_run_surface_backtest` / `dispersion_run_projected_var`. The ~185 lines
// that used to sit here were a SECOND, older implementation of both: same artifacts
// (`surface_backtest.tsv`, `projected_risk_scenarios.tsv`, `projected_risk_legs.tsv`,
// `projected_var.tsv`), same order, same `#if ATX_VOL_PROFILE / ATX_VOL_COUNTERS`
// probes, and NEITHER route touches `run.atxrun` on either side -- which is exactly
// why they can be unioned with main's RunArchive design without conflict.
//
// What this copy could not reach, and the library does. The list is PER ROUTE:
// it used to be written as one list for both, which was false for
// `run-projected-var` on two of its five entries (REV-TAIL I-2).
//
// `run-surface-backtest` (-> dispersion_run_surface_backtest):
//   * X1 strict typed spec -- a misspelled or unimplemented key fails BY NAME here
//     instead of being silently dropped by `read_run_spec`;
//   * X2/X6 frictions, financing, costs and X3 risk limits actually reaching the
//     engine (this copy hardcoded `config.run.unpriced = Error` and nothing else,
//     so a spec's declared frictions changed no number). Four of those keys bound
//     and then died in `dispersion_backtest_config_from`; closed at REV-TAIL I-3;
//   * X4 weighting / strike policies, and the previously-hardcoded multiplier;
//   * X5 `surface_tearsheet.tsv` + `surface_pnl_track.tsv`, regime FIRST, and the
//     regime named on the console line.
//
// `run-projected-var` (-> dispersion_run_projected_var): exactly ONE of the above.
//   * C1-ACTIVATE point-in-time universe resolution -- this copy froze the basket at
//     the first session date (`universe_at(universe_rows, clock.refs().front()
//     .date)`), so a mid-window reconstitution was silently ignored.
//   NOT X1: the library route reads the same loose `read_run_spec`
//   (dispersion_run.cpp:2474), because a projected-VaR run consumes no execution
//   knobs. NOT X4: it still hardcodes `side = ShortIndexLongNames` and
//   `multiplier = 100.0` (:2510-2511). Neither is a REGRESSION -- the copy this
//   dispatch replaced hardcoded both identically -- but neither was recovered, and
//   claiming otherwise is how a knob gets believed to be wired. X2/X3/X5/X6 do not
//   apply to this route at all: it runs no engine and writes no tearsheet.
//
// The E1 unit resolution the merge recorded at these two call sites is preserved,
// not lost: `dispersion_backtest_config_from` and `dispersion_run_projected_var`
// assign `gross_index_vega` / `target_vega` straight from the spec with no
// `kVegaVolPointToUnitVol` scaling, which is the same post-E1 assignment those
// comments described, and dispersion_run.hpp's header block states the contract.

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
  std::fprintf(stderr, "usage:\n"
                       "  atxvol_spy_dispersion_backtest build-corpus --spec FILE --out DIR\n"
                       "  atxvol_spy_dispersion_backtest build-schedule --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-backtest --run DIR\n"
                       "  atxvol_spy_dispersion_backtest project-schedule --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-projected-backtest --run DIR "
                       "[--schedule FILE] [--execution cold|configured] [--out FILE] "
                       "[--no-divergence]\n"
                       "  atxvol_spy_dispersion_backtest run-surface-backtest --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-projected-var --run DIR\n"
                       "  atxvol_spy_dispersion_backtest verify --run DIR\n"
                       "  atxvol_spy_dispersion_backtest runarchive dump DIR SECTION [--tsv]\n");
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
    } else {
      usage();
      return 2;
    }
  }
  // RECONCILE 1 — the CLI/library split, in one place. `dispersion_run.hpp` states
  // the same contract entry point by entry point and is the authority; this table
  // is what makes it observable from the command line:
  //
  //   build-corpus         -> atx::vol::dispersion_build_corpus        (library)
  //   run-surface-backtest -> atx::vol::dispersion_run_surface_backtest(library)
  //   run-projected-var    -> atx::vol::dispersion_run_projected_var   (library)
  //   build-schedule       -> local; publishes run.atxrun sections
  //   run-backtest         -> local; publishes run.atxrun sections
  //   project-schedule     -> local; RunArchive-era subcommand, no library twin
  //   run-projected-backtest -> local; RunArchive-era subcommand, no library twin
  //   verify               -> local; RunDir::verify over run.atxrun
  //   runarchive dump      -> local; RunArchive section reader, no library twin
  //
  // (REV-TAIL M-2: `runarchive dump` was missing from this table, which claims to
  // be the split "in one place" -- 8 of 9. It is easy to miss because it takes
  // POSITIONAL arguments and is therefore handled at :889, above the --flag
  // parser, instead of in the dispatch chain below. It is in `usage()` at :875.)
  //
  // The three that dispatch are the three where BOTH designs write only loose
  // TSVs, so the union is exact. The rest are on main's RunArchive cutover, whose
  // library twins still write the loose result files that cutover replaced —
  // dispatching them would break the archive the Python layer reads.
  Status status = Err(ErrorCode::InvalidArgument, "unknown command");
  if (command == "build-corpus" && !spec.empty() && !out.empty()) {
    status = atx::vol::dispersion_build_corpus(spec, out);
  } else if (command == "build-schedule" && !run.empty()) {
    status = build_schedule_command(run);
  } else if (command == "run-backtest" && !run.empty()) {
    status = run_backtest_command(run);
  } else if (command == "project-schedule" && !run.empty()) {
    status = project_schedule_command(run);
  } else if (command == "run-projected-backtest" && !run.empty()) {
    status = run_projected_backtest_command(
        run, schedule.empty() ? fs::path("trade_schedule.tsv") : schedule,
        execution.empty() ? std::string("configured") : execution,
        out.empty() ? fs::path("projected_backtest.tsv") : out, no_divergence);
  } else if (command == "run-surface-backtest" && !run.empty()) {
    status = atx::vol::dispersion_run_surface_backtest(run);
  } else if (command == "run-projected-var" && !run.empty()) {
    status = atx::vol::dispersion_run_projected_var(run);
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

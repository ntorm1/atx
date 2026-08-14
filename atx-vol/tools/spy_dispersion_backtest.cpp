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
#include "atx/vol/api/backtest/backtest.hpp"
#include "atx/vol/api/marketdata/corpus.hpp"
#include "atx/vol/research/run_archive.hpp"
#include "atx/vol/research/run_diagnostics.hpp"
#include "storage/track_key.hpp" // kBacktestEconomicsRev (E1 fix round)
#include "dispersion_backtest_regime.hpp" // friction_regime_text (E1 fix round)
#include "fitting/counters.hpp"
#include "atx/vol/api/backtest/dispersion.hpp"
#include "atx/vol/research/dispersion_backtest.hpp"
#include "atx/vol/research/dispersion_run.hpp"
#include "atx/vol/research/dispersion_workflow.hpp"
#include "analytics/historical_projection.hpp"
#include "atx/vol/api/backtest/listed_dispersion.hpp"
#include "atx/vol/research/listed_dispersion_pipeline.hpp"
#include "atx/vol/research/listed_dispersion_reconciliation.hpp"
#include "atx/vol/api/backtest/listed_dispersion_schedule.hpp"
#include "atx/vol/api/backtest/listed_dispersion_strategy.hpp"
#include "atx/vol/research/listed_definitions_cache.hpp"
#include "marketdata/listed_opra.hpp"
#include "marketdata/occ_ess.hpp"
#include "atx/vol/api/marketdata/opra_batch.hpp"
#include "core/phase_profile.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/backtest/strategy.hpp"
#include "atx/vol/tools/tearsheet.hpp"
#include "atx/vol/api/core/types.hpp"

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

// Snapshot look-ahead depth for the projected replay, overridable by
// ATX_VOL_PREFETCH_DEPTH for host-specific tuning and for reproducing a prior
// run's pipeline shape. The DEFAULT is 2, not 1: the projected replay's
// per-step economics are cheap next to deserializing that step's archive, so at
// depth 1 the run is load-bound and the loads serialize behind the economics
// one at a time. Measured through this tool on the 135-session corpus (v1
// closeout sprint Task 4.8, plan item 6.7): 1->2 is +15.2% median wall, 11/12
// rounds won; 2->4 and 4->8 are statistical washes (+1.9% 7/12, +1.6% 7/12)
// that also raise resident whole-board snapshots for no reliable win (depth 8
// holds 10 against depth 2's 4 — private_snapshot_cache_capacity, backtest.cpp),
// so the default stops at 2 rather than climbing further. Depth is a
// scheduling knob ONLY — the output is bit-identical at every depth (see
// RunConfig::prefetch_depth), so an override can cost throughput but can never
// change a number.
//
// An unparseable or out-of-range value falls back to the default rather than
// failing the run: this is a performance hint, and a typo in an env var must not
// break a production replay. Capped so a fat-fingered value cannot try to hold
// the entire clock's snapshots in memory at once.
std::size_t projected_prefetch_depth() {
  constexpr std::size_t kDefaultDepth = 2u;
  constexpr std::size_t kMaxDepth = 64u;
  std::string raw;
#if defined(_WIN32)
  char *env = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&env, &len, "ATX_VOL_PREFETCH_DEPTH") == 0 && env != nullptr) {
    raw.assign(env);
    std::free(env);
  }
#else
  if (const char *env = std::getenv("ATX_VOL_PREFETCH_DEPTH")) {
    raw.assign(env);
  }
#endif
  if (raw.empty()) {
    return kDefaultDepth;
  }
  unsigned long long parsed = 0ull;
  const char *first = raw.c_str();
  const auto [end, ec] = std::from_chars(first, first + raw.size(), parsed);
  if (ec != std::errc{} || end != first + raw.size() || parsed == 0ull || parsed > kMaxDepth) {
    std::fprintf(stderr,
                 "warning: ATX_VOL_PREFETCH_DEPTH=%s is not an integer in [1, %llu]; using %llu\n",
                 raw.c_str(), static_cast<unsigned long long>(kMaxDepth),
                 static_cast<unsigned long long>(kDefaultDepth));
    return kDefaultDepth;
  }
  return static_cast<std::size_t>(parsed);
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
// (atx/vol/research/run_diagnostics.hpp), so the binary result container measures phases
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
// example-local duplicates of the library's own copies (`write_input_inventory`
// and `persist_occ_ess_evidence`, src/dispersion_run.cpp:114 and :136, the
// T-wave lift). Their only caller was `build_corpus_command`, which
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
// See the seam contract at the top of atx/vol/research/dispersion_run.hpp for why this
// subcommand dispatches and `build-schedule` / `run-backtest` / `verify` do not.

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
  // REV-FIXTAIL I-A: the SAME run_spec.tsv, also through the STRICT typed reader,
  // for the one thing the loose RunSpec cannot carry — F6's quote-quality
  // admission policy. `quote_min_bid` / `quote_max_age_ns` / `quote_reject_locked`
  // bound by name, survived `reject_unknown()` and were published into
  // `run_config.tsv` as EFFECTIVE, while their only consumer was a library-only
  // twin (`dispersion_build_schedule`, deleted at S3-T17); on this route the
  // declared policy reached no selection at all. This is the same double read
  // `run-backtest` already performs (below, at the strict
  // `read_dispersion_run_config`), so both shipped listed routes agree on config
  // construction. A malformed spec fails here by name instead of two subcommands
  // later.
  //
  // REV-MTIDY M-2, on the SCOPE of that, because the shorter claim ("it cannot
  // break the pipeline, run-backtest already strict-parses the same file") is
  // narrower than the change. This read brings `reject_unknown()` and the whole
  // contract-validation block onto a route that previously ran only the tolerant
  // `read_run_spec`, which ignores unknown keys silently. The loose reader's key
  // vocabulary is a strict SUBSET of the strict reader's, so no ACCEPTED key is
  // now rejected; the exposure is an unrecognised or contract-violating key, and
  // every candidate in reach passes (all 20 published run dirs, both tracked
  // example specs, the paired fixture). What the shorter claim misses is the
  // chain `build-schedule -> project-schedule -> run-projected-backtest`:
  // `run_projected_backtest_command` reads the loose `read_run_spec` (below) and
  // never strict-reads, so after this change that chain has no lenient entry
  // point at all. Judged an improvement — a spec key that is a typo should not
  // reach a published NAV — but it is a behaviour change on that chain and not
  // merely a duplicate of a read run-backtest already performs.
  ATX_TRY(DispersionRunConfig run_config, read_dispersion_run_config(run_dir / "run_spec.tsv"));
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
  std::vector<QuoteRejectRow> quote_reject_rows;
  const ListedQuoteRejectSink quote_reject_sink =
      [&](std::string_view date, bool selected, const ListedQuoteRejectCounts &counts) {
        quote_reject_rows.push_back(QuoteRejectRow{std::string(date), selected, counts});
      };
  auto schedule_result = build_listed_dispersion_schedule_audited(
      clock, sched_spec, method, universe_rows, definitions, spec, &timer, quote_reject_sink);
  // Publish the audit artifact before propagating the acceptance error: failed
  // entry/roll selection is precisely when rejection counts are most useful.
  ATX_TRY_VOID(write_quote_reject_report(run_dir / "quote_rejects.tsv", quote_reject_rows));
  if (!schedule_result) {
    return Err(schedule_result.error());
  }
  ListedDispersionSchedule schedule = std::move(*schedule_result);

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

  // S3-T17: the M1 native reference reconciliation, wired into the one shipped
  // binary that can reach it. `reconcile_dispersion_reference` re-derives the
  // vega-flat schedule quantities from the persisted artifact and numerically
  // compares them against the recorded values — an arithmetic check independent
  // of the engine that produced them, which is a different question from the
  // structural + envelope one `RunDir::verify()` above answers. Until now its
  // only caller was the library-only `dispersion_verify`, so it reached no
  // shipped binary at all (dispersion_run.hpp said so, at length); S3-T17
  // deleted that caller, and this is the wire-in half of the same disposition.
  //
  // SCHEDULE-ONLY, and that is not a shortcut. The full mode additionally parses
  // `backtest.tsv`, `contract_marks.tsv` and `reconciliation.tsv`, and after
  // S3-T17 NOTHING writes those three into a run directory — the deleted library
  // twin was their last writer, and this binary has published its economics as
  // `run.atxrun` sections since the RunArchive cutover. Asking for the full mode
  // here would be a call that can only ever fail on a directory this pipeline
  // produced. `trade_schedule.tsv` is a retained text input and is always
  // present, so the schedule half is the half that is actually reachable.
  //
  // UNDER THE DIAGNOSTICS FLAG, for two reasons. (a) `reference_reconciliation
  // .tsv` is a loose diagnostic sidecar, the same class of artifact
  // `emit_tsv_diagnostics` governs everywhere else. (b) A verifier that gains a
  // new way to fail on every existing run directory is a behaviour change, and
  // every run directory published before this commit declares the flag nowhere,
  // so the default path is byte-for-byte what it was. The strict read is free of
  // new risk on this route: `verify` requires `trade_schedule.tsv`, which only
  // `build-schedule` writes, and that command already strict-reads this same file.
  ATX_TRY(DispersionRunConfig run_config, read_dispersion_run_config(run_dir / "run_spec.tsv"));
  if (run_config.emit_tsv_diagnostics) {
    ATX_TRY(std::vector<ReferenceReconRecord> records,
            reconcile_dispersion_reference(run_dir, /*schedule_only=*/true));
    ATX_TRY_VOID(
        write_reference_reconciliation_file(run_dir / "reference_reconciliation.tsv", records));
  }

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
  // atomically via RunDir.
  phase = PhaseTimer::now();
  // E1 fix round: this route's headline artifact -- the RunArchive `meta`
  // section -- carried neither `friction_regime` nor `economics_rev` (review
  // finding). `backtest.friction_regime` is B1's own field, stamped by the
  // engine that just ran, so this reads the ASSUMPTION THE ENGINE ACTUALLY
  // USED rather than re-deriving it; `economics_rev` names which build of
  // that engine's economics interpretation produced it (D1). `extra` becomes
  // ROWS in the `meta` ScalarKV section, not columns -- the RunArchive schema
  // hash (and therefore every existing byte-compat guarantee on this
  // artifact) is untouched (see `encode_meta_section`'s own header comment).
  const std::vector<std::pair<std::string, std::string>> meta_extra = {
      {"friction_regime", std::string(friction_regime_text(backtest.friction_regime))},
      {"economics_rev", std::to_string(kBacktestEconomicsRev)},
  };
  std::vector<RaSectionData> sections;
  sections.push_back(encode_schedule_section("trade_schedule", schedule));
  sections.push_back(encode_backtest_section("backtest", backtest));
  sections.push_back(encode_reconciliation_section(reconciliation));
  sections.push_back(encode_contract_marks_section(reconciliation));
  sections.push_back(encode_meta_section(spec, meta_extra));
  timer.add("write_outputs", phase);
  sections.push_back(encode_diagnostics_section(timer, "run_backtest", backtest.size()));
  ATX_TRY_VOID(RunDir(run_dir).write_run_archive(sections));

  std::printf("backtest complete: dates=%zu rolls=%zu final_nav=%.10g friction_regime=%s "
              "economics_rev=%d\n",
              backtest.size(), schedule.rolls.size(), backtest.nav.back(),
              std::string(friction_regime_text(backtest.friction_regime)).c_str(),
              kBacktestEconomicsRev);
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

// Projected replay of a listed-format schedule. `--execution cold` (DEFAULT) is route P
// canonical: no fast tier, QueryExecution::ColdReference with ScheduleMarkPolicy::Record
// (Configured-required economics permitted with a cold price execution while no fast tier
// is prepared). `--execution configured` is the Task 2 diagnostic: reprice through the
// fast cached-surrogate tier under QueryExecution::Configured (genuine interpolation).
//
// COLD IS THE DEFAULT BECAUSE CONFIGURED IS KNOWN TO BE WRONG ON THIS BOOK, not merely
// slower. `dispersion-parity` Task 2 diagnosed the fast tier's residual: with an
// identical book (22 lots, n_unpriced = 0) and the name legs interpolating cleanly under
// ~50 bps, the whole deviation came from the SPY index American PUT — fast mark 13.797
// vs cold 12.775, an 800 bps error — while the SPY CALL at the same strike and expiry
// matched cold to under 0.1 bps. A put-only error at the same node is the early-exercise
// boundary, which only binds for the put; CarryBank was cross-checked and is worse
// (~1054 bps). On a 49-session dispersion corpus that shows up as final_nav 25013.36865
// under configured against 18528.61666 under cold — a 35% overstatement on a book that
// is short index puts. Configured also costs ~21x more wall time (~4.3 s vs ~0.20 s),
// so the previous default was both the wrong number AND the slow one.
//
// The default was flipped only after confirming nothing in the repo depended on it: every
// in-tree invocation passes --execution explicitly, and the one test naming
// `projected_execution` (run_archive_test.cpp) writes "cold" into a synthetic archive
// rather than exercising this default. The chosen route is still recorded in the
// run.atxrun `meta` section, so provenance of which route produced a run is unchanged.
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
  config.prefetch_depth = projected_prefetch_depth();
  // Sized to the look-ahead window plus one spare slot, NOT unbounded. The
  // default-constructed SnapshotCache this replaces was unbounded, so a 135-session
  // replay retained all 135 reconstructed snapshots for the whole run and freed them
  // in one storm at teardown — peak memory and that teardown paid for a strictly
  // forward-only walk that never looks back.
  //
  // depth + 2 is the live working set (base + shifted + depth in flight), and it is
  // safe at that exact size only because bounded mode evicts in insertion order;
  // see private_snapshot_cache_capacity in backtest.cpp, which sizes run_backtest's
  // own private cache identically. This call site must not diverge from it, because
  // supplying a cache opts OUT of that sizing.
  config.snapshot_cache = std::make_shared<SnapshotCache>(config.prefetch_depth + 2u);
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
    // Say so, every time. This route has a diagnosed accuracy gap (see the doc block),
    // and it used to be the silent default — a caller could take its nav for the
    // canonical one with nothing in the output to suggest otherwise. The printed
    // `[configured]` tag alone did not carry that meaning to anyone who had not read
    // the source.
    std::fprintf(stderr,
                 "warning: --execution configured is a DIAGNOSTIC route with a known "
                 "fast-tier American-put accuracy gap; its nav is not the canonical "
                 "figure. Use --execution cold (the default) for route P economics.\n");
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
#if defined(ATX_VOL_PROFILE)
  phase_profile::reset();
#endif
#if defined(ATX_VOL_COUNTERS)
  counters::reset();
#endif
  ATX_TRY(ListedDispersionStrategy strategy,
          ListedDispersionStrategy::create(schedule, spec.delta_band, ScheduleMarkPolicy::Record));
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
#if defined(ATX_VOL_PROFILE)
  {
    // Same per-region dump run-surface-backtest already emitted, for the route the
    // `diagnostics` section cannot break down: its whole cost is one `priced_run`
    // row, so a phase list of five names says only "it is all inside run_backtest".
    // Written to a DISTINCT filename so a run directory can hold both routes'
    // profiles; the look-ahead depth is stamped in because it is the variable this
    // profile is usually being read to explain, and a file that does not name it is
    // unattributable after the fact.
    const phase_profile::Snapshot measured = phase_profile::snapshot();
    const double total_ns = static_cast<double>(
        measured.nanoseconds[static_cast<unsigned>(phase_profile::Region::BacktestTotal)]);
    std::ofstream output(run_dir / "projected_profile.tsv", std::ios::binary | std::ios::trunc);
    if (!output) {
      return Err(ErrorCode::IoError, "cannot write projected profile");
    }
    output << "# prefetch_depth=" << config.prefetch_depth << " sessions=" << backtest.size()
           << '\n';
    output << "region\tcalls\ttotal_ms\tpct_backtest\tns_per_call\n" << std::setprecision(17);
    for (unsigned i = 0; i < phase_profile::kCount; ++i) {
      const double ns = static_cast<double>(measured.nanoseconds[i]);
      const double calls = static_cast<double>(measured.calls[i]);
      output << phase_profile::kNames[i] << '\t' << measured.calls[i] << '\t' << ns / 1.0e6 << '\t'
             << (total_ns > 0.0 ? 100.0 * ns / total_ns : 0.0) << '\t'
             << (calls > 0.0 ? ns / calls : 0.0) << '\n';
    }
    if (!output) {
      return Err(ErrorCode::IoError, "cannot flush projected profile");
    }
  }
#endif
#if defined(ATX_VOL_COUNTERS)
  {
    // The algorithm counters for the SAME window the profile above times. Read
    // together they answer a question neither answers alone: the profile says which
    // phase costs the wall time, and these say whether that phase is doing redundant
    // work (DuplicateMarkSolves) or irreducible work.
    const counters::Snapshot measured = counters::snapshot();
    std::ofstream output(run_dir / "projected_counters.tsv", std::ios::binary | std::ios::trunc);
    if (!output) {
      return Err(ErrorCode::IoError, "cannot write projected counters");
    }
    output << "counter\tvalue\n";
    for (unsigned i = 0; i < counters::kCount; ++i) {
      output << counters::kNames[i] << '\t' << measured.values[i] << '\n';
    }
    if (!output) {
      return Err(ErrorCode::IoError, "cannot flush projected counters");
    }
  }
#endif
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
  // in meta so --out/--execution stay provenance-visible.
  const auto write_start = PhaseTimer::now();
  const std::string projected_section = skip_divergence ? "projected_nodiv" : "projected_cold";
  // E1 fix round: same addition as run_backtest_command above -- this route's
  // `meta` section already carried an `extra` block, it just never named the
  // regime the engine actually classified `backtest` under, nor the
  // economics revision. `config.frictions`/`config.financing` are always this
  // route's RunConfig{} defaults (a diagnostic priced-mark-divergence replay,
  // never spec-driven), so `friction_regime` here is a real, if invariant,
  // classification of what actually ran -- not a placeholder.
  const std::vector<std::pair<std::string, std::string>> meta_extra = {
      {"projected_execution", execution},
      {"skip_divergence", skip_divergence ? "1" : "0"},
      {"requested_out", out_file.string()},
      {"friction_regime", std::string(friction_regime_text(backtest.friction_regime))},
      {"economics_rev", std::to_string(kBacktestEconomicsRev)},
  };
  std::vector<RaSectionData> sections;
  sections.push_back(encode_backtest_section(projected_section, backtest));
  if (!skip_divergence) {
    sections.push_back(build_mark_divergence_section(divergence_arena));
  }
  sections.push_back(encode_meta_section(spec, meta_extra));
  timer.add("write_outputs", write_start);
  sections.push_back(encode_diagnostics_section(timer, "run_projected_backtest", backtest.size()));
  ATX_TRY_VOID(RunDir(run_dir).write_run_archive(sections));
  std::printf("projected backtest complete [%s]: dates=%zu rolls=%zu final_nav=%.10g "
              "friction_regime=%s economics_rev=%d\n",
              execution.c_str(), backtest.size(), schedule.rolls.size(), backtest.nav.back(),
              std::string(friction_regime_text(backtest.friction_regime)).c_str(),
              kBacktestEconomicsRev);
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
//   * REVIEW C-1 (2026-07-26): the ANCHOR is now the LAST qualified snapshot, not
//     the first. The VaR reference is `frames.back().value`, so the book being
//     re-valued has to be the book held at THAT session; anchoring it on the
//     oldest session published the risk of a portfolio nobody holds. The as-of
//     session, its timestamp and a book fingerprint are now columns of
//     `projected_var.tsv` and are part of the verified header contract.
//   * X1 strict typed spec, and X4 weighting / strike / side / multiplier.
//     REVIEW C-15 (2026-07-26). This block used to say NOT X1 and NOT X4, on the
//     grounds that "a projected-VaR run consumes no execution knobs". That was
//     true and beside the point: the route consumes CONSTRUCTION knobs, and the
//     loose `read_run_spec` has no field for any of them, so it hardcoded
//     `side = ShortIndexLongNames` and `multiplier = 100.0` and never saw
//     `weighting` or `strike` at all. Neither was a REGRESSION -- the copy this
//     dispatch replaced hardcoded both identically -- but the consequence was
//     that ONE spec built one book in `run-surface-backtest` and a DIFFERENT
//     book here, silently. The route now reads the strict typed config and
//     builds its book through `dispersion_config_from`, the same builder the
//     surface route uses; `index_symbol` reaches the universe resolver too.
//   X2/X3/X5/X6 still do not apply to this route at all: it runs no engine and
//   writes no tearsheet.
//
// The E1 unit resolution the merge recorded at these two call sites is preserved,
// not lost: `dispersion_backtest_config_from` and `dispersion_run_projected_var`
// assign `gross_index_vega` / `target_vega` straight from the spec with no
// per-vol-point -> per-unit-vol scaling at all, which is the same post-E1
// assignment those comments described, and dispersion_run.hpp's header block
// states the contract. (The `kVegaVolPointToUnitVol` constant this note used to
// name was deleted as dead in the C-2 follow-up: no call site remained.)

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

// S3-T16 CONCERN 1, dispositioned at plan 5.6. `run-surface-backtest` and
// `run-projected-var` are the two subcommands whose ONLY outputs are loose TSV
// tables, and S3-T16 put those tables behind `emit_tsv_diagnostics`, which
// defaults OFF. A default run of either therefore computes, prints its console
// line, and leaves the run directory unchanged. That quiet default is the
// intended behaviour — forcing the flag on would re-create the artifact sprawl
// S3 removed — but at a terminal it is indistinguishable from a run that failed
// to write something, and the operator has no way to learn the flag's name.
//
// So the seam says so, and does nothing else. The note is STDERR-ONLY (every
// route's result line goes to stdout, and anything parsing that keeps parsing
// exactly what it did), it is emitted only AFTER the route has already
// succeeded, and it cannot change an exit code.
//
// The flag is re-read here instead of being threaded out of the library because
// both routes are one-line dispatches that return a Status, not a config. That
// read introduces no new failure mode: it happens only on the success path, and
// both entry points strict-read this same file through this same reader before
// doing any work (the X1 "strict typed spec" property described above), so a
// spec that got this far parses. A read that somehow fails is swallowed — an
// advisory note is never worth failing a successful run over.
void note_if_diagnostics_disabled(const fs::path &run_dir, const char *subcommand) {
  const fs::path spec_path = run_dir / "run_spec.tsv";
  const auto run_config = read_dispersion_run_config(spec_path);
  if (!run_config || run_config->emit_tsv_diagnostics) {
    return;
  }
  std::fprintf(stderr,
               "note: %s published no loose diagnostic tables -- `emit_tsv_diagnostics` is "
               "disabled (the default) in %s. Declare `emit_tsv_diagnostics true` there to "
               "publish them; the run's economics are the same either way.\n",
               subcommand, spec_path.string().c_str());
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
              "      --execution defaults to 'cold' (route P canonical). 'configured' is\n"
              "      a diagnostic with a known American-put accuracy gap; see below.\n"
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
              "cache.hpp) for the full disclosure.\n"
              "\n"
              "--execution cold|configured (run-projected-backtest): DEFAULTS TO 'cold',\n"
              "  route P canonical -- QueryExecution::ColdReference, no fast tier.\n"
              "  'configured' reprices through the fast cached-surrogate tier and is a\n"
              "  DIAGNOSTIC, not an alternative:\n"
              "    * it carries a diagnosed American-put accuracy gap. With an identical\n"
              "      book and the name legs clean under ~50 bps, the SPY index PUT marked\n"
              "      800 bps off cold while the CALL at the same strike/expiry matched to\n"
              "      under 0.1 bps -- the early-exercise boundary, which only binds for\n"
              "      the put. CarryBank is worse (~1054 bps);\n"
              "    * on a 49-session dispersion corpus that is final_nav 25013.36865\n"
              "      against cold's 18528.61666 -- a 35%% overstatement on a book that is\n"
              "      short index puts;\n"
              "    * it also costs ~21x the wall time (~4.3 s vs ~0.20 s).\n"
              "  It warns on stderr when selected. The route used is recorded in the\n"
              "  run.atxrun 'meta' section as projected_execution.\n");
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
  // The three that dispatch are the three where BOTH designs wrote only loose
  // TSVs, so the union was exact (RECONCILE 1). The rest are RunArchive-era
  // bodies with NO library twin at all: S3-T17 deleted the three that existed
  // (`dispersion_build_schedule`, `dispersion_run_backtest`, `dispersion_verify`)
  // rather than dispatch into them, because these bodies are already the
  // collapsed form — each is a composition of named library seams
  // (`build_listed_dispersion_schedule_audited`, `make_listed_replay_run_config`,
  // `reconcile_listed_schedule`, `RunDir::write_run_archive`, `RunDir::verify`)
  // and the twins had drifted away from them. See the seam contract at the top of
  // dispersion_run.hpp, which is the authority.
  Status status = Err(ErrorCode::InvalidArgument, "unknown command");
  if (command == "build-corpus" && !spec.empty() && !out.empty()) {
    status = atx::vol::dispersion_build_corpus(spec, out);
  } else if (command == "build-schedule" && !run.empty()) {
    status = build_schedule_command(run, cache);
  } else if (command == "run-backtest" && !run.empty()) {
    status = run_backtest_command(run, cache);
  } else if (command == "project-schedule" && !run.empty()) {
    status = project_schedule_command(run);
  } else if (command == "run-projected-backtest" && !run.empty()) {
    status = run_projected_backtest_command(
        run, schedule.empty() ? fs::path("trade_schedule.tsv") : schedule,
        // Default `cold`, not `configured` — the diagnostic route is both wrong on this
        // book and ~21x slower. Rationale in run_projected_backtest_command's doc block.
        execution.empty() ? std::string("cold") : execution,
        out.empty() ? fs::path("projected_backtest.tsv") : out, no_divergence);
  } else if (command == "run-surface-backtest" && !run.empty()) {
    status = atx::vol::dispersion_run_surface_backtest(run);
    if (status) {
      note_if_diagnostics_disabled(run, "run-surface-backtest");
    }
  } else if (command == "run-projected-var" && !run.empty()) {
    status = atx::vol::dispersion_run_projected_var(run);
    if (status) {
      note_if_diagnostics_disabled(run, "run-projected-var");
    }
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

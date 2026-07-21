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
#include "atx/vol/counters.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/dispersion_workflow.hpp"
#include "atx/vol/historical_projection.hpp"
#include "atx/vol/listed_dispersion.hpp"
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

Result<std::uint64_t> hash_file(const fs::path &path) {
  ATX_TRY(std::string bytes, read_text(path));
  return Ok(hash_text(bytes));
}

// ── Runtime diagnostics ──────────────────────────────────────────────────────
// Local phase-timing helper for the instrumented subcommands. Accumulates named
// phases (wall time plus a unit count) in a fixed, pre-declared order and — via
// write_diagnostics — emits diagnostics_<subcommand>.tsv (truncated per
// invocation) alongside a one-line stderr summary. steady_clock only; always on,
// since the overhead is negligible next to the surface solves it measures. It
// writes a NEW file and touches only stderr: no economic output, no existing
// artifact, and no stdout line is affected (tooling may parse stdout).
class PhaseTimer {
public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;

  struct Phase {
    std::string name;
    Duration elapsed{Duration::zero()};
    std::uint64_t count{0u};
  };

  // Pre-declaring the phase order fixes the output row order regardless of when
  // each region is first timed. Some phases accumulate across a loop, and one
  // (archive_load) is timed inside a region also charged to another phase, so
  // first-seen order would not match the intended reading order otherwise.
  explicit PhaseTimer(std::initializer_list<std::string_view> order) {
    for (std::string_view name : order) {
      phases_.push_back(Phase{std::string(name), Duration::zero(), 0u});
    }
  }

  static Clock::time_point now() { return Clock::now(); }

  // Charge (now - start) wall time and `count` units to `phase` (created if it
  // was not pre-declared). Repeated calls to the same phase accumulate.
  void add(std::string_view phase, Clock::time_point start, std::uint64_t count = 0u) {
    const Duration elapsed = Clock::now() - start;
    for (Phase &entry : phases_) {
      if (entry.name == phase) {
        entry.elapsed += elapsed;
        entry.count += count;
        return;
      }
    }
    phases_.push_back(Phase{std::string(phase), elapsed, count});
  }

  const std::vector<Phase> &phases() const { return phases_; }

private:
  std::vector<Phase> phases_;
};

double phase_ms(PhaseTimer::Duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

// Write diagnostics_<subcommand>.tsv and print the one-line stderr summary. The
// `total` wall time is measured over the whole command independently of the
// phase sum (which carries per-region slack); `unit`/`units` name the natural
// count denominator ("session"/"sessions" for backtests, "roll"/"rolls" for the
// schedule builders).
Status write_diagnostics(const fs::path &run_dir, const char *subcommand,
                         const PhaseTimer &timer, PhaseTimer::Duration total,
                         std::uint64_t total_count, const char *unit, const char *units) {
  const fs::path path = run_dir / (std::string("diagnostics_") + subcommand + ".tsv");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot write diagnostics");
  }
  out << "ATX_DISPERSION_DIAGNOSTICS\t1\n";
  out << "subcommand\tphase\twall_ms\tcount\n";
  out << std::fixed << std::setprecision(3);
  for (const PhaseTimer::Phase &phase : timer.phases()) {
    out << subcommand << '\t' << phase.name << '\t' << phase_ms(phase.elapsed) << '\t'
        << phase.count << '\n';
  }
  const double total_ms = phase_ms(total);
  out << subcommand << "\ttotal\t" << total_ms << '\t' << total_count << '\n';
  if (!out) {
    return Err(ErrorCode::IoError, "cannot flush diagnostics");
  }
  const double per_unit = total_count > 0u ? total_ms / static_cast<double>(total_count) : 0.0;
  std::fprintf(stderr, "diag %s: total=%.3fms %s=%llu (%.3f ms/%s)\n", subcommand, total_ms, units,
               static_cast<unsigned long long>(total_count), per_unit, unit);
  return Ok();
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
  const std::vector<std::string> symbols = all_symbols(universe_rows);
  if (spec.core_mode && symbols.size() < 51u) {
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

Result<std::vector<ListedOptionQuote>> load_listed_quotes(const RunSpec &spec,
                                                          const ListedDefinitionTable &definitions,
                                                          std::span<const std::string> symbols,
                                                          std::string_view date) {
  ATX_TRY(OpraBatchResult batch, load_opra_daterange(batch_spec(spec, symbols, date, date)));
  std::vector<ListedOptionQuote> quotes;
  for (const OpraBatchEntry &entry : batch.entries) {
    if (!entry.panel) {
      continue;
    }
    // SkipUnlisted: both consumers of this helper (build-schedule roll-date
    // selection and run-backtest reconciliation) only ever act on defined,
    // standard-monthly 21-60 DTE contracts. A quote with no point-in-time
    // definition is an intraday-listed contract outside that universe on its
    // listing day; dropping it is a no-op on every date where the join already
    // succeeds (the skip can only fire where the strict Error policy would have
    // hard-failed), so currently-passing runs stay bit-for-bit unchanged.
    ATX_TRY(std::vector<ListedOptionQuote> joined,
            listed_quotes_from_opra(date, entry.panel->frame.snapshot_ts_ns, *entry.panel,
                                    definitions, MissingDefinitionPolicy::SkipUnlisted));
    quotes.insert(quotes.end(), std::make_move_iterator(joined.begin()),
                  std::make_move_iterator(joined.end()));
  }
  return Ok(std::move(quotes));
}

Status build_schedule_command(const fs::path &run_dir) {
  const auto cmd_start = PhaseTimer::now();
  PhaseTimer timer({"setup_read", "selection", "quote_join", "write_outputs"});

  auto phase = PhaseTimer::now();
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  ATX_TRY(ListedDefinitionTable definitions,
          read_listed_definitions_file((run_dir / "definitions.tsv").string()));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY_VOID(verify_occ_ess_evidence(run_dir, clock));
  if (spec.core_mode && clock.size() < 60u) {
    return Err(ErrorCode::Unavailable, "core mode requires at least 60 admitted dates");
  }
  const std::vector<std::string> symbols = all_symbols(universe_rows);
  ListedDispersionSchedule schedule;
  std::int64_t active_expiry = 0;
  timer.add("setup_read", phase);
  for (const SnapshotRef &ref : clock.refs()) {
    // selection: snapshot load + universe resolve. count=1 is charged once per
    // evaluated roll date (a date that reaches selection, deferrals included);
    // DTE-skip dates below charge only their load time, with no evaluation count.
    const auto sel_start = PhaseTimer::now();
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(ref.archive_path));
    const double active_dte =
        active_expiry == 0
            ? 0.0
            : static_cast<double>(active_expiry - snapshot.ts_ns()) / kListedNsPerDay;
    if (active_expiry != 0 && active_dte > spec.roll_dte_days) {
      timer.add("selection", sel_start);
      continue;
    }
    ATX_TRY(DispersionUniverse authored, universe_at(universe_rows, ref.date));
    MissingNameSpec missing{MissingNamePolicy::DropRenormalize, spec.min_names};
    ATX_TRY(
        ResolvedUniverse resolved,
        resolve_universe_uids(
            authored, [&](std::string_view symbol) { return snapshot.uid_of(symbol); }, missing));
    timer.add("selection", sel_start, 1u);

    // quote_join: the OPRA parquet join re-marking the roll-date universe.
    const auto join_start = PhaseTimer::now();
    ATX_TRY(std::vector<ListedOptionQuote> quotes,
            load_listed_quotes(spec, definitions, symbols, ref.date));
    timer.add("quote_join", join_start, 1u);

    const auto eval_start = PhaseTimer::now();
    ListedDispersionSelectionConfig selection_config;
    selection_config.target_dte_days = spec.target_dte_days;
    selection_config.min_dte_days = spec.min_dte_days;
    selection_config.max_dte_days = spec.max_dte_days;
    selection_config.min_names = spec.min_names;
    const ListedForwardLookup forward = [&](const DispersionMember &member,
                                            std::int64_t expiry) -> Result<double> {
      const PricedSurface *surface = snapshot.find(member.uid);
      if (surface == nullptr) {
        return Err(ErrorCode::NotFound, "surface missing");
      }
      const double term = static_cast<double>(expiry - snapshot.ts_ns()) / kNsPerYear;
      const double value = surface->forward_at(term);
      return std::isfinite(value) && value > 0.0
                 ? Ok(value)
                 : Err(ErrorCode::Unavailable, "forward unavailable");
    };
    const auto selected = select_listed_dispersion(ref.date, snapshot.ts_ns(), resolved.universe,
                                                   quotes, forward, selection_config);
    timer.add("selection", eval_start);
    if (!selected) {
      if (active_expiry == 0) {
        continue;
      }
      std::fprintf(stderr, "roll deferred on %s: %s\n", ref.date.c_str(),
                   selected.error().to_string().c_str());
      continue;
    }
    double requested_weight = 0.0;
    for (const DispersionMember &name : authored.names) {
      requested_weight += name.weight;
    }
    double traded_weight = 0.0;
    for (const ListedStraddle &name : selected->names) {
      traded_weight += name.raw_weight;
    }
    const double coverage = traded_weight / requested_weight;
    if (coverage < spec.min_weight_coverage) {
      if (active_expiry == 0) {
        continue;
      }
      std::fprintf(stderr, "roll deferred on %s: weight coverage %.6f\n", ref.date.c_str(),
                   coverage);
      continue;
    }
    const auto build_start = PhaseTimer::now();
    ListedScheduleBuildConfig build;
    build.gross_index_vega_target_per_vol_point = spec.gross_index_vega;
    build.cohort = static_cast<std::uint32_t>(schedule.rolls.size() + 1u);
    ATX_TRY(const std::uint64_t archive_fingerprint, hash_file(ref.archive_path));
    build.surface_fingerprint = archive_fingerprint;
    ATX_TRY(ListedScheduleRoll roll,
            build_listed_dispersion_roll(*selected, snapshot.set(), build));
    active_expiry = roll.expiry_ts_ns;
    schedule.rolls.push_back(std::move(roll));
    timer.add("selection", build_start);
  }
  if (schedule.rolls.empty() || (spec.core_mode && schedule.rolls.size() < 3u)) {
    return Err(ErrorCode::Unavailable,
               "schedule does not satisfy entry/three-roll acceptance gate");
  }
  const auto write_start = PhaseTimer::now();
  ATX_TRY_VOID(
      write_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string(), schedule));
  timer.add("write_outputs", write_start);
  std::printf("built immutable schedule: rolls=%zu\n", schedule.rolls.size());
  ATX_TRY_VOID(write_diagnostics(run_dir, "build_schedule", timer,
                                 PhaseTimer::now() - cmd_start, schedule.rolls.size(), "roll",
                                 "rolls"));
  return Ok();
}

Status verify_command(const fs::path &run_dir) {
  ATX_TRY(RunSpec spec, read_run_spec(run_dir / "run_spec.tsv"));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(CorpusQualityReport quality,
          read_quality_report_file((run_dir / "quality.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  ATX_TRY_VOID(verify_occ_ess_evidence(run_dir, clock));
  ATX_TRY(ListedDispersionSchedule schedule,
          read_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string()));
  ATX_TRY_VOID(validate_listed_dispersion_schedule(schedule));
  for (const fs::path &required :
       {run_dir / "input_inventory.tsv", run_dir / "methodology_map.tsv", run_dir / "backtest.tsv",
        run_dir / "occ_ess_inventory.tsv", run_dir / "contract_marks.tsv",
        run_dir / "reconciliation.tsv", run_dir / "reference_reconciliation.tsv"}) {
    std::error_code error;
    if (!fs::is_regular_file(required, error) || fs::file_size(required, error) == 0u) {
      return Err(ErrorCode::NotFound, "missing final artifact " + required.string());
    }
  }
  if (quality.n_admitted != manifest.n_ok) {
    return Err(ErrorCode::InvalidArgument, "quality/manifest admitted count mismatch");
  }
  if (spec.core_mode) {
    if (clock.size() < 60u || schedule.rolls.size() < 3u) {
      return Err(ErrorCode::Unavailable, "core date/roll acceptance gate failed");
    }
    for (const ListedScheduleRoll &roll : schedule.rolls) {
      if (roll.n_names < 40u) {
        return Err(ErrorCode::Unavailable, "core roll breadth acceptance gate failed");
      }
    }
  }
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

  phase = PhaseTimer::now();
  ATX_TRY_VOID(write_backtest_tsv(backtest, (run_dir / "backtest.tsv").string()));
  timer.add("write_outputs", phase);

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
            load_listed_quotes(spec, definitions, symbols, ref.date));
    quote_owners.push_back(std::move(quotes));
  }
  std::vector<ListedReconciliationSnapshot> reconciliation_snapshots;
  reconciliation_snapshots.reserve(clock.size());
  for (std::size_t i = 0; i < clock.size(); ++i) {
    reconciliation_snapshots.push_back(
        ListedReconciliationSnapshot{clock.refs()[i].date, snapshot_owners[i]->ts_ns(),
                                     &snapshot_owners[i]->set(), quote_owners[i]});
  }
  ATX_TRY(ListedDispersionReconciliation reconciliation,
          reconcile_listed_dispersion(schedule, reconciliation_snapshots));
  ATX_TRY_VOID(validate_listed_reconciliation_backtest(reconciliation, backtest));
  timer.add("reconciliation", phase, clock.size());

  phase = PhaseTimer::now();
  ATX_TRY_VOID(
      write_listed_contract_marks_file((run_dir / "contract_marks.tsv").string(), reconciliation));
  ATX_TRY_VOID(
      write_listed_reconciliation_file((run_dir / "reconciliation.tsv").string(), reconciliation));
  timer.add("write_outputs", phase);

  std::printf("backtest complete: dates=%zu rolls=%zu final_nav=%.10g\n", backtest.size(),
              schedule.rolls.size(), backtest.nav.back());
  ATX_TRY_VOID(write_diagnostics(run_dir, "run_backtest", timer,
                                 PhaseTimer::now() - cmd_start, backtest.size(), "session",
                                 "sessions"));
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

  // Cold certified economics on both sides, matching the run-projected-backtest
  // --execution cold replay route (RunConfig default analytic AL greeks +
  // ColdReference), so the persisted schedule marks equal the live cold seed marks
  // that replay recomputes.
  const bool analytic = true;
  const QueryExecution execution = QueryExecution::ColdReference;

  ListedDispersionSchedule projected;
  projected.rolls.reserve(listed.rolls.size());
  timer.add("setup_read", phase);
  for (const ListedScheduleRoll &roll : listed.rolls) {
    const auto archive = archive_of.find(roll.roll_date);
    if (archive == archive_of.end()) {
      return Err(ErrorCode::NotFound,
                 "project-schedule: no qualified archive for roll date " + roll.roll_date);
    }
    // archive_load: one snapshot deserialize per roll (count=archives loaded).
    const auto load_start = PhaseTimer::now();
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(archive->second));
    timer.add("archive_load", load_start, 1u);
    // cold_solve: cold per-leg pricing + straddle rebuild + vega sizing for this
    // roll (count=legs solved).
    const auto solve_start = PhaseTimer::now();
    if (snapshot.ts_ns() != roll.valuation_ts_ns) {
      return Err(ErrorCode::InvalidArgument,
                 "project-schedule: archive valuation timestamp differs from roll");
    }
    const double residual_T =
        static_cast<double>(roll.expiry_ts_ns - roll.valuation_ts_ns) / kNsPerYear;
    if (!(residual_T > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "project-schedule: nonpositive residual tenor");
    }
    if (roll.legs.size() != 2u * (1u + roll.n_names) || roll.legs.size() < 2u) {
      return Err(ErrorCode::InvalidArgument, "project-schedule: malformed frozen roll");
    }

    // Cold per-share greeks at (uid, projected strike, residual T, side) for sizing.
    const ListedRiskLookup cold_lookup =
        [&](std::uint32_t uid, const ListedOptionQuote &quote) -> Result<ListedOptionRisk> {
      const PricedSurface *surface = snapshot.find(uid);
      if (surface == nullptr) {
        return Err(ErrorCode::NotFound, "project-schedule: projected surface unavailable");
      }
      ATX_TRY(FullGreekSeed seed,
              surface->full_greek_seed(quote.strike, residual_T, quote.side, analytic, execution));
      return Ok(ListedOptionRisk{seed.greeks().price, seed.greeks().delta, seed.greeks().vega});
    };

    // Rebuild one member straddle from its frozen call/put legs, replacing the listed
    // strike with the surface ATM forward at residual T. forward_at is the same accessor
    // the synthetic dispersion route (resolve_leg / resolve_atm_iv) uses for its
    // ATM-forward strike. The synthetic raw quote is priced at the cold model value
    // (zero synthetic spread — there is no listed market at the interpolated strike);
    // raw_symbol / instrument_id / source_fingerprint retain the listed contract each
    // projected straddle idealizes (provenance + a unique per-leg source key).
    const auto make_straddle =
        [&](const ListedScheduleLeg &call_leg,
            const ListedScheduleLeg &put_leg) -> Result<ListedStraddle> {
      const PricedSurface *surface = snapshot.find(call_leg.uid);
      if (surface == nullptr) {
        return Err(ErrorCode::NotFound, "project-schedule: projected surface unavailable");
      }
      const double K = surface->forward_at(residual_T);
      if (!(K > 0.0)) {
        return Err(ErrorCode::Unavailable, "project-schedule: no ATM forward at residual tenor");
      }
      const auto make_quote = [&](const ListedScheduleLeg &leg,
                                  Side side) -> Result<ListedOptionQuote> {
        ATX_TRY(FullGreekSeed seed,
                surface->full_greek_seed(K, residual_T, side, analytic, execution));
        ListedOptionQuote quote;
        quote.trade_date = roll.roll_date;
        quote.symbol = leg.symbol;
        quote.instrument_id = leg.instrument_id;
        quote.raw_symbol = leg.raw_symbol;
        quote.expiry_ts_ns = leg.expiry_ts_ns;
        quote.strike = K;
        quote.side = side;
        quote.bid = seed.greeks().price;
        quote.ask = seed.greeks().price;
        quote.quote_ts_ns = roll.valuation_ts_ns;
        quote.multiplier = leg.multiplier;
        quote.standard_monthly = true;
        quote.standard_deliverable = true;
        quote.source_fingerprint = leg.source_fingerprint;
        return Ok(std::move(quote));
      };
      ListedStraddle straddle;
      straddle.symbol = call_leg.symbol;
      straddle.uid = call_leg.uid;
      straddle.expiry_ts_ns = call_leg.expiry_ts_ns;
      straddle.strike = K;
      ATX_TRY(straddle.call, make_quote(call_leg, Side::Call));
      ATX_TRY(straddle.put, make_quote(put_leg, Side::Put));
      straddle.raw_weight = call_leg.normalized_weight;
      straddle.normalized_weight = call_leg.normalized_weight;
      return Ok(std::move(straddle));
    };

    // Frozen roll legs are call/put pairs, index pair first.
    ListedDispersionSelection selection;
    selection.trade_date = roll.roll_date;
    selection.valuation_ts_ns = roll.valuation_ts_ns;
    selection.expiry_ts_ns = roll.expiry_ts_ns;
    selection.dte_days =
        static_cast<double>(roll.expiry_ts_ns - roll.valuation_ts_ns) / kListedNsPerDay;
    ATX_TRY(selection.index, make_straddle(roll.legs[0], roll.legs[1]));
    selection.names.reserve(roll.n_names);
    for (std::size_t i = 2u; i + 1u < roll.legs.size(); i += 2u) {
      ATX_TRY(ListedStraddle name, make_straddle(roll.legs[i], roll.legs[i + 1u]));
      selection.names.push_back(std::move(name));
    }

    ListedScheduleBuildConfig build_cfg;
    build_cfg.gross_index_vega_target_per_vol_point = roll.gross_index_vega_target_per_vol_point;
    build_cfg.side = DispersionSide::ShortIndexLongNames;
    build_cfg.cohort = roll.cohort;
    build_cfg.surface_fingerprint = roll.legs.front().surface_fingerprint;
    ATX_TRY(ListedScheduleRoll projected_roll,
            build_listed_dispersion_roll(selection, cold_lookup, build_cfg));
    std::printf("  roll %u %s: net_vega=%.10g gross_vega=%.10g index_K=%.6f (listed %.6f)\n",
                projected_roll.cohort, projected_roll.roll_date.c_str(),
                projected_roll.net_vega_per_vol_point, projected_roll.gross_vega_per_vol_point,
                projected_roll.legs.front().strike, roll.legs.front().strike);
    projected.rolls.push_back(std::move(projected_roll));
    timer.add("cold_solve", solve_start, roll.legs.size());
  }

  const auto write_start = PhaseTimer::now();
  ATX_TRY_VOID(validate_listed_dispersion_schedule(projected));
  ATX_TRY_VOID(write_listed_dispersion_schedule_file(
      (run_dir / "projected_schedule.tsv").string(), projected));
  timer.add("validate_write", write_start);
  std::printf("projected schedule built: rolls=%zu\n", projected.rolls.size());
  ATX_TRY_VOID(write_diagnostics(run_dir, "project_schedule", timer,
                                 PhaseTimer::now() - cmd_start, listed.rolls.size(), "roll",
                                 "rolls"));
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
// `--out` the backtest output (default projected_backtest.tsv).
Status run_projected_backtest_command(const fs::path &run_dir, const fs::path &schedule_file,
                                      const std::string &execution, const fs::path &out_file) {
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
    // Route P canonical: cold certified economics both sides, no fast tier attached.
    // Record policy reprices the projected definitions through the ColdReference route.
    // required_economic_execution() == Configured means "no cold requirement": the
    // engine gate only enforces anything when a strategy requires ColdReference, so a
    // Record strategy runs under any execution, including this explicit cold override.
    config.price.query_execution = QueryExecution::ColdReference;
  } else {
    // Attaching the prepared fast tier (with_query_pricing, propagated by
    // MarketSnapshot::load with no silent cold fallback) makes the Configured queries
    // genuinely interpolate the cached surrogate instead of reproducing the cold
    // archive marks.
    config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
    config.price.query_execution = QueryExecution::Configured;
  }
  timer.add("setup_read", phase);

  // mark_divergence.tsv: last_mark_divergences() is cleared every step and the
  // engine's run_backtest loop hides per-step strategy state, so drive a separate
  // Record replay here and snapshot the record after each roll step.
  //
  // divergence_replay (count=sessions) times this whole replay; the per-session
  // MarketSnapshot::load inside it is ALSO accumulated into archive_load
  // (count=loads), so archive_load is a measured subset of divergence_replay. The
  // priced run's loads go through the snapshot cache inside run_backtest and are
  // not separately measurable here.
  const auto div_start = PhaseTimer::now();
  ATX_TRY(ListedDispersionStrategy divergence_strategy,
          ListedDispersionStrategy::create(schedule, spec.delta_band, ScheduleMarkPolicy::Record));
  PortfolioState divergence_book;
  std::uint64_t divergence_next_id = 1;
  std::ofstream div_out(run_dir / "mark_divergence.tsv", std::ios::binary | std::ios::trunc);
  if (!div_out) {
    return Err(ErrorCode::IoError, "cannot write mark divergence");
  }
  div_out << std::setprecision(17)
          << "date\tsymbol\traw_symbol\tstrike\texpiry_ts_ns\tside\tschedule_mark\tlive_mark\t"
             "diff\tabs_diff_bps_of_mark\n";
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
      div_out << ref.date << '\t' << matched->symbol << '\t' << matched->raw_symbol << '\t'
              << divergence.strike << '\t' << divergence.expiry_ts_ns << '\t'
              << (divergence.side == Side::Call ? "Call" : "Put") << '\t' << divergence.schedule_mark
              << '\t' << divergence.live_mark << '\t' << diff << '\t' << abs_diff_bps_of_mark
              << '\n';
    }
  }
  if (!div_out) {
    return Err(ErrorCode::IoError, "cannot flush mark divergence");
  }
  // An empty mark_divergence.tsv must mean "every roll fired and none diverged",
  // never "the replay silently skipped rolls" — this file is the evidence channel
  // for the parity report's zero-divergence claim.
  if (!divergence_strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "divergence replay did not consume every scheduled roll");
  }
  timer.add("divergence_replay", div_start, clock.size());

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

  const auto write_start = PhaseTimer::now();
  ATX_TRY_VOID(write_backtest_tsv(backtest, (run_dir / out_file).string()));
  timer.add("write_outputs", write_start);
  std::printf("projected backtest complete [%s]: dates=%zu rolls=%zu final_nav=%.10g\n",
              execution.c_str(), backtest.size(), schedule.rolls.size(), backtest.nav.back());
  ATX_TRY_VOID(write_diagnostics(run_dir, "run_projected_backtest", timer,
                                 PhaseTimer::now() - cmd_start, backtest.size(), "session",
                                 "sessions"));
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
  ATX_TRY(DispersionUniverse universe, universe_at(universe_rows, clock.refs().front().date));

  DispersionBacktestConfig config;
  config.target_dte_days = spec.target_dte_days;
  config.roll_dte_days = spec.roll_dte_days;
  // spec.gross_index_vega is dollars vega per VOL POINT per side; the library
  // dispersion configs take dollars vega per UNIT vol (a unit vol is 100 vol
  // points), so scale by 100 at this boundary.
  config.gross_index_vega = spec.gross_index_vega * 100.0;
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

  ATX_TRY(DispersionUniverse authored, universe_at(universe_rows, clock.refs().front().date));
  ATX_TRY(ResolvedUniverse resolved,
          resolve_universe_uids(
              authored, [&](std::string_view symbol) { return snapshots.front()->uid_of(symbol); },
              MissingNameSpec{MissingNamePolicy::DropRenormalize, spec.min_names}));
  DispersionConfig dispersion;
  dispersion.target_T = spec.target_dte_days / 365.25;
  // spec.gross_index_vega is dollars vega per VOL POINT per side; the library
  // dispersion configs take dollars vega per UNIT vol (a unit vol is 100 vol
  // points), so scale by 100 at this boundary.
  dispersion.target_vega = spec.gross_index_vega * 100.0;
  dispersion.side = DispersionSide::ShortIndexLongNames;
  dispersion.multiplier = 100.0;
  dispersion.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, spec.min_names};
  dispersion.projected_maturity =
      ProjectedMaturitySpec::days(static_cast<std::int32_t>(std::llround(spec.target_dte_days)));
  ATX_TRY(DispersionBook initial,
          build_dispersion_book(resolved.universe, snapshots.front()->set(), dispersion));

  std::vector<RelativeOptionPosition> relative_positions;
  relative_positions.reserve(initial.positions.size());
  for (const Position &position : initial.positions) {
    OptionProjectionSpec option;
    option.uid = position.contract.uid;
    option.maturity = *dispersion.projected_maturity;
    option.strike = ProjectedStrikeSpec::atm_forward();
    option.side = position.contract.side;
    option.multiplier = position.multiplier;
    relative_positions.push_back({option, position.qty});
  }
  ATX_TRY(PreparedHistoricalProjection prepared,
          PreparedHistoricalProjection::create(relative_positions));
  std::vector<HistoricalProjectionFrame> frames(scenarios.size());
  std::vector<ProjectedOption> legs(scenarios.size() * relative_positions.size());
  HistoricalProjectionConfig config;
  config.n_threads = spec.fit_workers;
  const auto started = std::chrono::steady_clock::now();
  ATX_TRY_VOID(prepared.evaluate_into(scenarios, frames, legs, config));
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
  for (std::size_t scenario = 0; scenario < frames.size(); ++scenario) {
    const HistoricalProjectionFrame &frame = frames[scenario];
    frame_out << clock.refs()[scenario].date << '\t' << frame.ts_ns << '\t' << frame.value << '\t'
              << frame.delta << '\t' << frame.gamma << '\t' << frame.vega << '\t' << frame.theta
              << '\t' << frame.n_ok << '\t' << frame.n_failed << '\t'
              << frame.definition_fingerprint << '\n';
    for (std::size_t leg = 0; leg < relative_positions.size(); ++leg) {
      const ProjectedOption &projected = legs[scenario * relative_positions.size() + leg];
      leg_out << clock.refs()[scenario].date << '\t' << leg << '\t'
              << projected.definition.contract.uid << '\t'
              << (projected.definition.contract.side == Side::Call ? "Call" : "Put") << '\t'
              << projected.definition.expiry_ts_ns << '\t' << projected.definition.contract.K
              << '\t' << relative_positions[leg].quantity << '\t' << projected.definition.multiplier
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
  for (const HistoricalProjectionFrame &frame : frames) {
    if (frame.n_failed != 0u)
      return Err(ErrorCode::Unavailable, "projected VaR: incomplete scenario projection");
  }

  std::ofstream summary(run_dir / "projected_var.tsv", std::ios::binary | std::ios::trunc);
  if (!summary)
    return Err(ErrorCode::IoError, "projected VaR: cannot open summary");
  summary << std::setprecision(17)
          << "confidence\treference_value\tvalue_at_risk\texpected_shortfall\tn_scenarios\t"
             "n_positions\tprojections_per_second\tprepared_fingerprint\n";
  for (const double confidence : {0.95, 0.99}) {
    ATX_TRY(ProjectedHistoricalVar risk,
            projected_historical_var(frames, frames.back().value, confidence));
    summary << risk.confidence << '\t' << risk.reference_value << '\t' << risk.value_at_risk << '\t'
            << risk.expected_shortfall << '\t' << risk.n_scenarios << '\t'
            << relative_positions.size() << '\t'
            << (static_cast<double>(legs.size()) / elapsed_seconds) << '\t'
            << prepared.fingerprint() << '\n';
  }
  if (!summary)
    return Err(ErrorCode::IoError, "projected VaR: summary write failed");
  std::printf("projected relative-template VaR complete: scenarios=%zu positions=%zu rate=%.1f/s\n",
              frames.size(), relative_positions.size(),
              static_cast<double>(legs.size()) / elapsed_seconds);
  return Ok();
}

void usage() {
  std::fprintf(stderr, "usage:\n"
                       "  atxvol_spy_dispersion_backtest build-corpus --spec FILE --out DIR\n"
                       "  atxvol_spy_dispersion_backtest build-schedule --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-backtest --run DIR\n"
                       "  atxvol_spy_dispersion_backtest project-schedule --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-projected-backtest --run DIR "
                       "[--schedule FILE] [--execution cold|configured] [--out FILE]\n"
                       "  atxvol_spy_dispersion_backtest run-surface-backtest --run DIR\n"
                       "  atxvol_spy_dispersion_backtest run-projected-var --run DIR\n"
                       "  atxvol_spy_dispersion_backtest verify --run DIR\n");
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  fs::path spec;
  fs::path run;
  fs::path out;
  fs::path schedule;
  std::string execution;
  for (int i = 2; i < argc; ++i) {
    const std::string_view argument = argv[i];
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
  Status status = Err(ErrorCode::InvalidArgument, "unknown command");
  if (command == "build-corpus" && !spec.empty() && !out.empty()) {
    status = build_corpus_command(spec, out);
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
        out.empty() ? fs::path("projected_backtest.tsv") : out);
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

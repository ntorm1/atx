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

struct RunSpec {
  std::string label{"SPY listed-options dispersion proxy"};
  std::string date_lo{};
  std::string date_hi{};
  std::string snapshot_suffix{"T19:55:00Z"};
  fs::path opra_root{};
  std::string path_template{"{symbol}/{date}.parquet"};
  fs::path universe_path{};
  fs::path definitions_path{};
  fs::path occ_ess_root{};
  double flat_rate{0.0};
  std::size_t min_names{10};
  double min_weight_coverage{0.8};
  double target_dte_days{30.0};
  double min_dte_days{21.0};
  double max_dte_days{60.0};
  double roll_dte_days{7.0};
  double gross_index_vega{10000.0};
  double delta_band{0.0};
  unsigned fit_workers{0};
  bool core_mode{false};
};

struct UniverseRow {
  std::string effective_date{};
  std::string symbol{};
  double raw_weight{0.0};
  std::string source{};
  std::string as_of{};
};

template <class T> bool parse_number(std::string_view text, T &value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

bool parse_double(std::string_view text, double &value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
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

fs::path resolve_path(const fs::path &base, std::string_view value) {
  fs::path path{value};
  return path.is_absolute() ? path.lexically_normal() : (base / path).lexically_normal();
}

Result<RunSpec> read_run_spec(const fs::path &path) {
  ATX_TRY(std::string text, read_text(path));
  std::map<std::string, std::string> values;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string_view line{text.data() + start,
                          (end == std::string::npos ? text.size() : end) - start};
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    start = end == std::string::npos ? text.size() : end + 1;
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    const auto fields = split(line, '\t');
    if (fields.size() != 2 || fields[0] == "key") {
      if (fields.size() == 2 && fields[0] == "key") {
        continue;
      }
      return Err(ErrorCode::ParseError, "run spec must contain key/value TSV rows");
    }
    if (!values.emplace(std::string(fields[0]), std::string(fields[1])).second) {
      return Err(ErrorCode::AlreadyExists, "duplicate run spec key");
    }
  }
  const auto required = [&](std::string_view key) -> Result<std::string> {
    const auto found = values.find(std::string(key));
    if (found == values.end() || found->second.empty()) {
      return Err(ErrorCode::ParseError, "missing run spec key " + std::string(key));
    }
    return Ok(found->second);
  };

  RunSpec spec;
  ATX_TRY(spec.date_lo, required("date_lo"));
  ATX_TRY(spec.date_hi, required("date_hi"));
  ATX_TRY(std::string opra, required("opra_root"));
  ATX_TRY(std::string universe, required("universe_schedule"));
  const fs::path base = path.parent_path();
  spec.opra_root = resolve_path(base, opra);
  spec.universe_path = resolve_path(base, universe);
  const auto optional_text = [&](std::string_view key, std::string &value) {
    const auto found = values.find(std::string(key));
    if (found != values.end()) {
      value = found->second;
    }
  };
  optional_text("label", spec.label);
  optional_text("snapshot_suffix", spec.snapshot_suffix);
  optional_text("path_template", spec.path_template);
  std::string definitions;
  std::string occ_ess;
  optional_text("definitions", definitions);
  optional_text("occ_ess_root", occ_ess);
  if (!definitions.empty())
    spec.definitions_path = resolve_path(base, definitions);
  if (!occ_ess.empty())
    spec.occ_ess_root = resolve_path(base, occ_ess);
  const auto number = [&](std::string_view key, auto &value) -> Status {
    const auto found = values.find(std::string(key));
    if (found == values.end()) {
      return Ok();
    }
    using Value = std::remove_reference_t<decltype(value)>;
    bool parsed = false;
    if constexpr (std::is_same_v<Value, double>) {
      parsed = parse_double(found->second, value);
    } else {
      parsed = parse_number(found->second, value);
    }
    return parsed ? Ok() : Err(ErrorCode::ParseError, "invalid run spec number");
  };
  ATX_TRY_VOID(number("flat_rate", spec.flat_rate));
  ATX_TRY_VOID(number("min_names", spec.min_names));
  ATX_TRY_VOID(number("min_weight_coverage", spec.min_weight_coverage));
  ATX_TRY_VOID(number("target_dte_days", spec.target_dte_days));
  ATX_TRY_VOID(number("min_dte_days", spec.min_dte_days));
  ATX_TRY_VOID(number("max_dte_days", spec.max_dte_days));
  ATX_TRY_VOID(number("roll_dte_days", spec.roll_dte_days));
  ATX_TRY_VOID(number("gross_index_vega", spec.gross_index_vega));
  ATX_TRY_VOID(number("delta_band", spec.delta_band));
  ATX_TRY_VOID(number("fit_workers", spec.fit_workers));
  unsigned core = 0;
  ATX_TRY_VOID(number("core_mode", core));
  spec.core_mode = core != 0;
  if (spec.date_lo > spec.date_hi || spec.min_names == 0 || spec.min_weight_coverage <= 0.0 ||
      spec.min_weight_coverage > 1.0 || spec.min_dte_days <= 0.0 ||
      spec.target_dte_days < spec.min_dte_days || spec.max_dte_days < spec.target_dte_days ||
      spec.roll_dte_days < 0.0 || spec.gross_index_vega <= 0.0 || spec.delta_band < 0.0) {
    return Err(ErrorCode::InvalidArgument, "invalid run spec contract");
  }
  if (spec.core_mode && (spec.min_names < 40 || spec.min_weight_coverage < 0.8)) {
    return Err(ErrorCode::InvalidArgument, "core mode requires >=40 names and >=80% weight");
  }
  return Ok(std::move(spec));
}

Status write_resolved_spec(const fs::path &path, const RunSpec &spec) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot write resolved run spec");
  }
  out << "key\tvalue\n"
      << "label\t" << spec.label << '\n'
      << "date_lo\t" << spec.date_lo << '\n'
      << "date_hi\t" << spec.date_hi << '\n'
      << "snapshot_suffix\t" << spec.snapshot_suffix << '\n'
      << "opra_root\t" << spec.opra_root.string() << '\n'
      << "path_template\t" << spec.path_template << '\n'
      << "universe_schedule\t" << spec.universe_path.string() << '\n';
  if (!spec.definitions_path.empty())
    out << "definitions\t" << spec.definitions_path.string() << '\n';
  if (!spec.occ_ess_root.empty())
    out << "occ_ess_root\t" << spec.occ_ess_root.string() << '\n';
  out
      << "flat_rate\t" << spec.flat_rate << '\n'
      << "min_names\t" << spec.min_names << '\n'
      << "min_weight_coverage\t" << spec.min_weight_coverage << '\n'
      << "target_dte_days\t" << spec.target_dte_days << '\n'
      << "min_dte_days\t" << spec.min_dte_days << '\n'
      << "max_dte_days\t" << spec.max_dte_days << '\n'
      << "roll_dte_days\t" << spec.roll_dte_days << '\n'
      << "gross_index_vega\t" << spec.gross_index_vega << '\n'
      << "delta_band\t" << spec.delta_band << '\n'
      << "fit_workers\t" << spec.fit_workers << '\n'
      << "core_mode\t" << (spec.core_mode ? 1 : 0) << '\n';
  return out ? Ok() : Err(ErrorCode::IoError, "cannot flush resolved run spec");
}

Result<std::vector<UniverseRow>> read_universe(const fs::path &path) {
  ATX_TRY(std::string text, read_text(path));
  const std::string_view header = "effective_date\tsymbol\traw_weight\tsource\tas_of";
  const std::size_t first_end = text.find('\n');
  if (first_end == std::string::npos ||
      std::string_view(text.data(), first_end).substr(0, header.size()) != header) {
    return Err(ErrorCode::ParseError, "bad universe schedule header");
  }
  std::vector<UniverseRow> rows;
  std::size_t start = first_end + 1;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string_view line{text.data() + start,
                          (end == std::string::npos ? text.size() : end) - start};
    start = end == std::string::npos ? text.size() : end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      continue;
    }
    const auto fields = split(line, '\t');
    UniverseRow row;
    if (fields.size() != 5 || !parse_double(fields[2], row.raw_weight) || !(row.raw_weight > 0.0)) {
      return Err(ErrorCode::ParseError, "bad universe schedule row");
    }
    row.effective_date = fields[0];
    row.symbol = fields[1];
    row.source = fields[3];
    row.as_of = fields[4];
    if (row.symbol.empty() || row.source.empty() || row.as_of > row.effective_date) {
      return Err(ErrorCode::InvalidArgument, "invalid point-in-time universe row");
    }
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
    return std::tie(a.effective_date, a.symbol) < std::tie(b.effective_date, b.symbol);
  });
  if (rows.empty()) {
    return Err(ErrorCode::InvalidArgument, "empty universe schedule");
  }
  return Ok(std::move(rows));
}

std::vector<std::string> all_symbols(std::span<const UniverseRow> rows) {
  std::vector<std::string> symbols{"SPY"};
  for (const UniverseRow &row : rows) {
    if (std::find(symbols.begin(), symbols.end(), row.symbol) == symbols.end()) {
      symbols.push_back(row.symbol);
    }
  }
  std::sort(symbols.begin(), symbols.end());
  return symbols;
}

Result<DispersionUniverse> universe_at(std::span<const UniverseRow> rows, std::string_view date) {
  std::map<std::string, const UniverseRow *> active;
  for (const UniverseRow &row : rows) {
    if (row.effective_date <= date) {
      active[row.symbol] = &row;
    }
  }
  DispersionUniverse universe;
  universe.index = DispersionMember{"SPY", 0u, 0.0};
  for (const auto &[symbol, row] : active) {
    if (symbol != "SPY") {
      universe.names.push_back(DispersionMember{symbol, 0u, row->raw_weight});
    }
  }
  if (universe.names.empty()) {
    return Err(ErrorCode::Unavailable, "no effective constituent schedule for date");
  }
  return Ok(std::move(universe));
}

OpraBatchSpec batch_spec(const RunSpec &spec, std::span<const std::string> symbols,
                         std::string_view date_lo, std::string_view date_hi) {
  OpraBatchSpec batch;
  batch.symbols.assign(symbols.begin(), symbols.end());
  batch.date_lo = date_lo;
  batch.date_hi = date_hi;
  batch.root_dir = spec.opra_root.string();
  batch.path_template = spec.path_template;
  batch.snapshot_suffix = spec.snapshot_suffix;
  batch.r = spec.flat_rate;
  batch.provenance_mode = OpraProvenanceMode::Strict;
  return batch;
}

std::uint64_t hash_text(std::string_view text) {
  const std::uint64_t hash = atx::core::hash_bytes(text.data(), text.size());
  return hash == 0u ? 1u : hash;
}

Result<std::uint64_t> hash_file(const fs::path &path) {
  ATX_TRY(std::string bytes, read_text(path));
  return Ok(hash_text(bytes));
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
  std::ofstream inventory(run_dir / "occ_ess_inventory.tsv",
                          std::ios::binary | std::ios::trunc);
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
    const fs::path expected = (run_dir / "occ_ess" / (std::string(row[0]) + ".txt")).lexically_normal();
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
    ATX_TRY(std::vector<ListedOptionQuote> joined,
            listed_quotes_from_opra(date, entry.panel->frame.snapshot_ts_ns, *entry.panel,
                                    definitions));
    quotes.insert(quotes.end(), std::make_move_iterator(joined.begin()),
                  std::make_move_iterator(joined.end()));
  }
  return Ok(std::move(quotes));
}

Status build_schedule_command(const fs::path &run_dir) {
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
  for (const SnapshotRef &ref : clock.refs()) {
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(ref.archive_path));
    const double active_dte =
        active_expiry == 0
            ? 0.0
            : static_cast<double>(active_expiry - snapshot.ts_ns()) / kListedNsPerDay;
    if (active_expiry != 0 && active_dte > spec.roll_dte_days) {
      continue;
    }
    ATX_TRY(DispersionUniverse authored, universe_at(universe_rows, ref.date));
    MissingNameSpec missing{MissingNamePolicy::DropRenormalize, spec.min_names};
    ATX_TRY(
        ResolvedUniverse resolved,
        resolve_universe_uids(
            authored, [&](std::string_view symbol) { return snapshot.uid_of(symbol); }, missing));
    ATX_TRY(std::vector<ListedOptionQuote> quotes,
            load_listed_quotes(spec, definitions, symbols, ref.date));
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
    ListedScheduleBuildConfig build;
    build.gross_index_vega_target_per_vol_point = spec.gross_index_vega;
    build.cohort = static_cast<std::uint32_t>(schedule.rolls.size() + 1u);
    ATX_TRY(const std::uint64_t archive_fingerprint, hash_file(ref.archive_path));
    build.surface_fingerprint = archive_fingerprint;
    ATX_TRY(ListedScheduleRoll roll,
            build_listed_dispersion_roll(*selected, snapshot.set(), build));
    active_expiry = roll.expiry_ts_ns;
    schedule.rolls.push_back(std::move(roll));
  }
  if (schedule.rolls.empty() || (spec.core_mode && schedule.rolls.size() < 3u)) {
    return Err(ErrorCode::Unavailable,
               "schedule does not satisfy entry/three-roll acceptance gate");
  }
  ATX_TRY_VOID(
      write_listed_dispersion_schedule_file((run_dir / "trade_schedule.tsv").string(), schedule));
  std::printf("built immutable schedule: rolls=%zu\n", schedule.rolls.size());
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
        run_dir / "occ_ess_inventory.tsv",
         run_dir / "contract_marks.tsv", run_dir / "reconciliation.tsv",
        run_dir / "reference_reconciliation.tsv"}) {
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
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
  if (!strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "backtest did not consume every scheduled roll");
  }
  ATX_TRY_VOID(write_backtest_tsv(backtest, (run_dir / "backtest.tsv").string()));

  const std::vector<std::string> symbols = all_symbols(universe_rows);
  std::vector<std::unique_ptr<MarketSnapshot>> snapshot_owners;
  std::vector<std::vector<ListedOptionQuote>> quote_owners;
  snapshot_owners.reserve(clock.size());
  quote_owners.reserve(clock.size());
  for (const SnapshotRef &ref : clock.refs()) {
    ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(ref.archive_path));
    snapshot_owners.push_back(std::make_unique<MarketSnapshot>(std::move(snapshot)));
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
  ATX_TRY_VOID(
      write_listed_contract_marks_file((run_dir / "contract_marks.tsv").string(), reconciliation));
  ATX_TRY_VOID(
      write_listed_reconciliation_file((run_dir / "reconciliation.tsv").string(), reconciliation));
  std::printf("backtest complete: dates=%zu rolls=%zu final_nav=%.10g\n", backtest.size(),
              schedule.rolls.size(), backtest.nav.back());
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

  DispersionConfig dispersion;
  dispersion.target_T = spec.target_dte_days / 365.25;
  dispersion.target_vega = spec.gross_index_vega;
  dispersion.side = DispersionSide::ShortIndexLongNames;
  dispersion.multiplier = 100.0;
  dispersion.missing = MissingNameSpec{MissingNamePolicy::DropRenormalize, spec.min_names};
  dispersion.projected_maturity =
      ProjectedMaturitySpec::days(static_cast<std::int32_t>(std::llround(spec.target_dte_days)));

  LifecycleSpec lifecycle;
  lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  lifecycle.entry_every_n = 21u;
  lifecycle.roll_at_T = spec.roll_dte_days / 365.25;
  HedgeSpec hedge;
  hedge.kind = HedgeSpec::Kind::DeltaToZero;
  hedge.cadence = HedgeSpec::Cadence::Daily;
  hedge.band = spec.delta_band;

  DispersionStrategy strategy{std::move(universe), dispersion, lifecycle, hedge};
  RunConfig config;
  config.unpriced = UnpricedLotPolicy::Error;
#if defined(ATX_VOL_PROFILE)
  phase_profile::reset();
#endif
#if defined(ATX_VOL_COUNTERS)
  counters::reset();
#endif
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
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
      output << phase_profile::kNames[i] << '\t' << measured.calls[i] << '\t' << ns / 1.0e6
             << '\t' << (total_ns > 0.0 ? 100.0 * ns / total_ns : 0.0) << '\t'
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
  ATX_TRY(std::vector<UniverseRow> universe_rows,
          read_universe(run_dir / "universe_schedule.tsv"));
  ATX_TRY(CorpusManifest manifest,
          read_manifest_file((run_dir / "surface_manifest.tsv").string()));
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
          universe_at(universe_rows, clock.refs().front().date));
  ATX_TRY(ResolvedUniverse resolved,
          resolve_universe_uids(authored,
                                [&](std::string_view symbol) {
                                  return snapshots.front()->uid_of(symbol);
                                },
                                MissingNameSpec{MissingNamePolicy::DropRenormalize,
                                                spec.min_names}));
  DispersionConfig dispersion;
  dispersion.target_T = spec.target_dte_days / 365.25;
  dispersion.target_vega = spec.gross_index_vega;
  dispersion.side = DispersionSide::ShortIndexLongNames;
  dispersion.multiplier = 100.0;
  dispersion.missing =
      MissingNameSpec{MissingNamePolicy::DropRenormalize, spec.min_names};
  dispersion.projected_maturity = ProjectedMaturitySpec::days(
      static_cast<std::int32_t>(std::llround(spec.target_dte_days)));
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
  std::ofstream leg_out(run_dir / "projected_risk_legs.tsv",
                        std::ios::binary | std::ios::trunc);
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
              << projected.definition.expiry_ts_ns << '\t'
              << projected.definition.contract.K << '\t' << relative_positions[leg].quantity
              << '\t' << projected.definition.multiplier << '\t' << projected.model_mark << '\t'
              << projected.greeks.delta << '\t' << projected.greeks.gamma << '\t'
              << projected.greeks.vega << '\t' << projected.greeks.theta << '\t'
              << projected.definition.fingerprint << '\t' << to_string(projected.status) << '\n';
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
    summary << risk.confidence << '\t' << risk.reference_value << '\t' << risk.value_at_risk
            << '\t' << risk.expected_shortfall << '\t' << risk.n_scenarios << '\t'
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
  for (int i = 2; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (i + 1 >= argc) {
      usage();
      return 2;
    }
    if (argument == "--spec") {
      spec = argv[++i];
    } else if (argument == "--out" || argument == "--run") {
      run = argv[++i];
    } else {
      usage();
      return 2;
    }
  }
  Status status = Err(ErrorCode::InvalidArgument, "unknown command");
  if (command == "build-corpus" && !spec.empty() && !run.empty()) {
    status = build_corpus_command(spec, run);
  } else if (command == "build-schedule" && !run.empty()) {
    status = build_schedule_command(run);
  } else if (command == "run-backtest" && !run.empty()) {
    status = run_backtest_command(run);
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

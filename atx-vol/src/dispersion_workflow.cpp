#include "atx/vol/dispersion_workflow.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
namespace fs = std::filesystem;

namespace {

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
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return fields;
}

Result<std::string> read_text(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return Err(ErrorCode::NotFound, "cannot open " + path.string());
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return (!stream.good() && !stream.eof()) ? Err(ErrorCode::IoError, "cannot read " + path.string())
                                           : Ok(std::move(text));
}

fs::path resolve_path(const fs::path &base, std::string_view value) {
  fs::path path{value};
  return path.is_absolute() ? path.lexically_normal() : (base / path).lexically_normal();
}

} // namespace

Result<RunSpec> read_run_spec(const fs::path &path) {
  ATX_TRY(std::string text, read_text(path));
  std::map<std::string, std::string> values;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string_view line{text.data() + start,
                          (end == std::string::npos ? text.size() : end) - start};
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    start = end == std::string::npos ? text.size() : end + 1;
    if (line.empty() || line.starts_with('#'))
      continue;
    const auto fields = split(line, '\t');
    if (fields.size() == 2 && fields[0] == "key")
      continue;
    if (fields.size() != 2)
      return Err(ErrorCode::ParseError, "run spec must contain key/value TSV rows");
    if (!values.emplace(std::string(fields[0]), std::string(fields[1])).second)
      return Err(ErrorCode::AlreadyExists, "duplicate run spec key");
  }
  const auto required = [&](std::string_view key) -> Result<std::string> {
    const auto found = values.find(std::string(key));
    if (found == values.end() || found->second.empty())
      return Err(ErrorCode::ParseError, "missing run spec key " + std::string(key));
    return found->second;
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
    if (found != values.end())
      value = found->second;
  };
  optional_text("label", spec.label);
  optional_text("snapshot_suffix", spec.snapshot_suffix);
  optional_text("path_template", spec.path_template);
  optional_text("index_symbol", spec.index_symbol);
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
    if (found == values.end())
      return Ok();
    using Value = std::remove_reference_t<decltype(value)>;
    const bool parsed = [&] {
      if constexpr (std::is_same_v<Value, double>)
        return parse_double(found->second, value);
      return parse_number(found->second, value);
    }();
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
      spec.roll_dte_days < 0.0 || spec.gross_index_vega <= 0.0 || spec.delta_band < 0.0)
    return Err(ErrorCode::InvalidArgument, "invalid run spec contract");
  if (spec.core_mode && (spec.min_names < 40 || spec.min_weight_coverage < 0.8))
    return Err(ErrorCode::InvalidArgument, "core mode requires >=40 names and >=80% weight");
  // Present-but-empty is an authoring error, not "use the default": an empty
  // index leg would poison universe_at's index member and all_symbols' seed.
  // Safe by construction — the key did not exist before L12, so no spec already
  // on disk can reach this rejection.
  if (spec.index_symbol.empty())
    return Err(ErrorCode::InvalidArgument, "run spec index_symbol must be non-empty");
  return spec;
}

Status write_resolved_spec(const fs::path &path, const RunSpec &spec) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return Err(ErrorCode::IoError, "cannot write resolved run spec");
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
  out << "flat_rate\t" << spec.flat_rate << '\n'
      << "min_names\t" << spec.min_names << '\n'
      << "min_weight_coverage\t" << spec.min_weight_coverage << '\n'
      << "target_dte_days\t" << spec.target_dte_days << '\n'
      << "min_dte_days\t" << spec.min_dte_days << '\n'
      << "max_dte_days\t" << spec.max_dte_days << '\n'
      << "roll_dte_days\t" << spec.roll_dte_days << '\n'
      << "gross_index_vega\t" << spec.gross_index_vega << '\n'
      << "delta_band\t" << spec.delta_band << '\n'
      << "fit_workers\t" << spec.fit_workers << '\n'
      << "core_mode\t" << (spec.core_mode ? 1 : 0) << '\n'
      // Appended LAST, after core_mode: an old-vs-new resolved spec then differs
      // by exactly one trailing line, which is trivially auditable.
      // encode_meta_section mirrors this key vocabulary and order.
      << "index_symbol\t" << spec.index_symbol << '\n';
  return out ? Ok() : Err(ErrorCode::IoError, "cannot flush resolved run spec");
}

Result<std::vector<UniverseRow>> read_universe(const fs::path &path) {
  ATX_TRY(std::string text, read_text(path));
  constexpr std::string_view header = "effective_date\tsymbol\traw_weight\tsource\tas_of";
  const std::size_t first_end = text.find('\n');
  if (first_end == std::string::npos)
    return Err(ErrorCode::ParseError, "bad universe schedule header");
  std::string_view header_line = std::string_view{text}.substr(0, first_end);
  if (!header_line.empty() && header_line.back() == '\r')
    header_line.remove_suffix(1);  // tolerate CRLF headers (data rows already CR-stripped below)
  if (header_line != header)
    return Err(ErrorCode::ParseError, "bad universe schedule header");
  std::vector<UniverseRow> rows;
  std::size_t start = first_end + 1;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string_view line{text.data() + start,
                          (end == std::string::npos ? text.size() : end) - start};
    start = end == std::string::npos ? text.size() : end + 1;
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (line.empty())
      continue;
    const auto fields = split(line, '\t');
    UniverseRow row;
    if (fields.size() != 5 || !parse_double(fields[2], row.raw_weight) || row.raw_weight <= 0.0)
      return Err(ErrorCode::ParseError, "bad universe schedule row");
    row.effective_date = fields[0];
    row.symbol = fields[1];
    row.source = fields[3];
    row.as_of = fields[4];
    if (row.symbol.empty() || row.source.empty() || row.as_of > row.effective_date)
      return Err(ErrorCode::InvalidArgument, "invalid point-in-time universe row");
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
    return std::tie(a.effective_date, a.symbol) < std::tie(b.effective_date, b.symbol);
  });
  if (rows.empty())
    return Err(ErrorCode::InvalidArgument, "empty universe schedule");
  return Ok(std::move(rows));
}

std::vector<std::string> all_symbols(std::span<const UniverseRow> rows,
                                     std::string_view index_symbol) {
  // The index leg is always fetched, whether or not it is also a constituent
  // row; the dedup below keeps it single.
  std::vector<std::string> symbols{std::string(index_symbol)};
  for (const UniverseRow &row : rows)
    if (std::find(symbols.begin(), symbols.end(), row.symbol) == symbols.end())
      symbols.push_back(row.symbol);
  std::sort(symbols.begin(), symbols.end());
  return symbols;
}

Result<DispersionUniverse> universe_at(std::span<const UniverseRow> rows, std::string_view date,
                                       std::string_view index_symbol) {
  std::map<std::string, const UniverseRow *> active;
  for (const UniverseRow &row : rows)
    if (row.effective_date <= date)
      active[row.symbol] = &row;
  DispersionUniverse universe;
  universe.index = DispersionMember{std::string(index_symbol), 0u, 0.0};
  for (const auto &[symbol, row] : active)
    if (symbol != index_symbol)
      universe.names.push_back({symbol, 0u, row->raw_weight});
  if (universe.names.empty())
    return Err(ErrorCode::Unavailable, "no effective constituent schedule for date");
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

} // namespace atx::vol

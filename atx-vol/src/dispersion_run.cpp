// Library seam for the traditional SPY listed-options dispersion proxy.
//
// The command workflow that used to live in examples/spy_dispersion_backtest.cpp
// lives here. Each stage is a plain function so it can be driven from a unit test
// off the filesystem, and the reproduction-critical admission constants are named
// on DispersionCorpusPolicy (see dispersion_run.hpp). This is a behavior-preserving
// extraction: the dispersion golden (final_nav = 247.4065016443293) is unchanged.

#include "atx/vol/dispersion_run.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <initializer_list>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/vol/counters.hpp"
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
#include "atx/vol/strategy.hpp"

namespace atx::vol {
namespace fs = std::filesystem;

namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

template <class T> bool parse_number(std::string_view text, T &value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

bool parse_double(std::string_view text, double &value) {
  if (text.empty()) {
    return false;
  }
  const std::string tmp(text);
  const char *begin = tmp.c_str();
  char *end = nullptr;
  errno = 0;
  const double parsed = std::strtod(begin, &end);
  if (end != begin + tmp.size() || !std::isfinite(parsed)) {
    return false;
  }
  value = parsed;
  return true;
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

Result<std::uint64_t> hash_file(const fs::path &path) {
  ATX_TRY(std::string bytes, read_text(path));
  return Ok(dispersion_hash_text(bytes));
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

// ── Native reference reconciliation (ported from tools/reference_spy_dispersion.py)

constexpr double kVegaScale = 0.01;
constexpr double kVegaRelTol = 1e-10;
constexpr double kFloatRecomputeTol = 1e-12;
constexpr double kPnlAbsTol = 1e-7;

// Returns the bare unexpected so it converts into ANY Result<T> return type
// (Result<void>, Result<vector<...>>, ...), matching the Python VerificationError.
[[nodiscard]] tl::unexpected<atx::core::Error> recon_fail(const std::string &message) {
  return Err(ErrorCode::InvalidArgument, "reference verification: " + message);
}

// A header-indexed TSV view (csv.DictReader analogue). Rows aligned to header.
struct DictTsv {
  std::vector<std::string> header;
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::vector<std::string>> rows;

  [[nodiscard]] const std::string *cell(std::size_t row, std::string_view name) const {
    const auto it = index.find(std::string(name));
    if (it == index.end() || row >= rows.size() || it->second >= rows[row].size()) {
      return nullptr;
    }
    return &rows[row][it->second];
  }
};

Result<DictTsv> read_dict_tsv(const fs::path &path, std::optional<std::string_view> magic) {
  ATX_TRY(std::string text, read_text(path));
  std::vector<std::string_view> lines = split(text, '\n');
  // Drop a trailing empty line produced by the final '\n'.
  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  auto strip_cr = [](std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1u);
    }
    return line;
  };
  std::size_t cursor = 0;
  if (magic) {
    if (lines.empty() || strip_cr(lines[0]) != *magic) {
      return recon_fail("bad magic in " + path.filename().string());
    }
    cursor = 1;
  }
  if (cursor >= lines.size()) {
    return recon_fail("missing header in " + path.filename().string());
  }
  DictTsv tsv;
  for (std::string_view field : split(strip_cr(lines[cursor]), '\t')) {
    tsv.index.emplace(std::string(field), tsv.header.size());
    tsv.header.emplace_back(field);
  }
  ++cursor;
  for (; cursor < lines.size(); ++cursor) {
    const std::string_view line = strip_cr(lines[cursor]);
    std::vector<std::string> cells;
    for (std::string_view field : split(line, '\t')) {
      cells.emplace_back(field);
    }
    if (cells.size() != tsv.header.size()) {
      return recon_fail("ragged row in " + path.filename().string());
    }
    tsv.rows.push_back(std::move(cells));
  }
  if (tsv.rows.empty()) {
    return recon_fail("empty artifact: " + path.filename().string());
  }
  return Ok(std::move(tsv));
}

Result<double> dec(const DictTsv &tsv, std::size_t row, std::string_view name) {
  const std::string *value = tsv.cell(row, name);
  double parsed = 0.0;
  if (value == nullptr || !parse_double(*value, parsed)) {
    return recon_fail("invalid decimal column " + std::string(name));
  }
  return Ok(parsed);
}

Result<std::int64_t> intcol(const DictTsv &tsv, std::size_t row, std::string_view name) {
  const std::string *value = tsv.cell(row, name);
  std::int64_t parsed = 0;
  if (value == nullptr || !parse_number(std::string_view(*value), parsed)) {
    return recon_fail("invalid integer column " + std::string(name));
  }
  return Ok(parsed);
}

const std::string &str(const DictTsv &tsv, std::size_t row, std::string_view name) {
  static const std::string kEmpty;
  const std::string *value = tsv.cell(row, name);
  return value == nullptr ? kEmpty : *value;
}

Status close_to(double actual, double expected, double tolerance, const char *label) {
  if (std::abs(actual - expected) > tolerance) {
    return recon_fail(std::string(label) + " out of tolerance");
  }
  return Ok();
}

Result<std::vector<ReferenceReconRecord>> verify_schedule(const fs::path &path) {
  ATX_TRY(DictTsv tsv, read_dict_tsv(path, std::string_view("ATX_LISTED_DISPERSION_SCHEDULE\t1")));

  std::set<std::array<std::string, 4>> seen_contracts;
  std::map<std::pair<std::string, std::int64_t>, std::vector<std::size_t>> grouped;
  std::vector<std::pair<std::string, std::int64_t>> ordered_keys;
  for (std::size_t r = 0; r < tsv.rows.size(); ++r) {
    ATX_TRY(std::int64_t cohort, intcol(tsv, r, "cohort"));
    const std::pair<std::string, std::int64_t> key{str(tsv, r, "roll_date"), cohort};
    if (grouped.find(key) == grouped.end()) {
      if (!ordered_keys.empty()) {
        const auto &last = ordered_keys.back();
        const bool strictly_greater =
            key.first > last.first || (key.first == last.first && key.second > last.second);
        if (!strictly_greater) {
          return recon_fail("schedule rolls are not strictly ordered");
        }
      }
      ordered_keys.push_back(key);
      grouped.emplace(key, std::vector<std::size_t>{});
    }
    const std::array<std::string, 4> contract_key{str(tsv, r, "roll_date"), str(tsv, r, "symbol"),
                                                  str(tsv, r, "raw_symbol"), str(tsv, r, "side")};
    if (!seen_contracts.insert(contract_key).second) {
      return recon_fail("duplicate schedule contract");
    }
    grouped[key].push_back(r);
  }

  std::vector<ReferenceReconRecord> output;
  for (const auto &key : ordered_keys) {
    const std::vector<std::size_t> &legs = grouped[key];
    if (legs.size() < 4 || (legs.size() % 2) != 0) {
      return recon_fail("invalid leg count for roll");
    }
    ATX_TRY(double target, dec(tsv, legs[0], "gross_index_vega_target"));
    if (target <= 0.0) {
      return recon_fail("nonpositive gross vega target for roll");
    }
    double computed_net = 0.0;
    double computed_gross = 0.0;
    double weight_sum = 0.0;
    double basket_target = 0.0;
    std::int64_t name_count = 0;
    for (std::size_t pair_index = 0; pair_index < legs.size(); pair_index += 2) {
      const std::size_t call = legs[pair_index];
      const std::size_t put = legs[pair_index + 1];
      static constexpr std::string_view kPairFields[] = {
          "roll_date", "cohort", "expiry_ts_ns",       "is_index",         "symbol",
          "uid",       "strike", "quantity",           "multiplier",       "normalized_weight",
          "target_straddle_vega"};
      bool pair_ok = str(tsv, call, "side") == "C" && str(tsv, put, "side") == "P";
      for (const std::string_view field : kPairFields) {
        if (str(tsv, call, field) != str(tsv, put, field)) {
          pair_ok = false;
        }
      }
      if (!pair_ok) {
        return recon_fail("invalid call/put pair for roll");
      }
      const bool is_index = str(tsv, call, "is_index") == "1";
      if (is_index != (pair_index == 0)) {
        return recon_fail("index pair ordering mismatch for roll");
      }

      ATX_TRY(double quantity, dec(tsv, call, "quantity"));
      double pair_vega = 0.0;
      double pair_achieved = 0.0;
      for (const std::size_t leg : {call, put}) {
        ATX_TRY(double multiplier, dec(tsv, leg, "multiplier"));
        ATX_TRY(double unit_vega, dec(tsv, leg, "vega_per_unit_vol"));
        const double contract_vega = unit_vega * multiplier * kVegaScale;
        ATX_TRY(double persisted_contract_vega, dec(tsv, leg, "vega_per_contract_per_vol_point"));
        ATX_TRY_VOID(close_to(persisted_contract_vega, contract_vega,
                              std::max(1.0, std::abs(contract_vega)) * kFloatRecomputeTol,
                              "per-contract vega"));
        ATX_TRY(double leg_quantity, dec(tsv, leg, "quantity"));
        const double achieved = leg_quantity * contract_vega;
        ATX_TRY(double persisted_achieved, dec(tsv, leg, "achieved_leg_vega"));
        ATX_TRY_VOID(close_to(persisted_achieved, achieved,
                              std::max(1.0, std::abs(achieved)) * kFloatRecomputeTol,
                              "achieved leg vega"));
        ATX_TRY(double raw_mid, dec(tsv, leg, "raw_mid"));
        ATX_TRY(double raw_bid, dec(tsv, leg, "raw_bid"));
        ATX_TRY(double raw_ask, dec(tsv, leg, "raw_ask"));
        ATX_TRY_VOID(close_to(raw_mid, (raw_bid + raw_ask) / 2.0,
                              std::max(1.0, std::abs(raw_mid)) * kFloatRecomputeTol,
                              "raw midpoint"));
        pair_vega += contract_vega;
        pair_achieved += achieved;
        computed_net += achieved;
        computed_gross += std::abs(achieved);
      }

      ATX_TRY(double pair_target, dec(tsv, call, "target_straddle_vega"));
      const double expected_quantity = pair_target / pair_vega;
      ATX_TRY_VOID(close_to(quantity, expected_quantity, std::abs(expected_quantity) * kVegaRelTol,
                            "vega-flat quantity"));
      ATX_TRY_VOID(close_to(pair_achieved, pair_target,
                            std::max(1.0, std::abs(pair_target)) * kVegaRelTol, "straddle target"));
      if (is_index) {
        ATX_TRY(double normalized_weight, dec(tsv, call, "normalized_weight"));
        if (normalized_weight != 0.0 || std::abs(pair_target) != target) {
          return recon_fail("invalid index target for roll");
        }
      } else {
        ATX_TRY(double weight, dec(tsv, call, "normalized_weight"));
        if (weight <= 0.0) {
          return recon_fail("nonpositive basket weight for roll");
        }
        weight_sum += weight;
        basket_target += pair_target;
        ++name_count;
      }
    }

    ATX_TRY_VOID(close_to(weight_sum, 1.0, kVegaRelTol, "normalized basket weight"));
    ATX_TRY(double index_target, dec(tsv, legs[0], "target_straddle_vega"));
    ATX_TRY_VOID(close_to(basket_target, -index_target, target * kVegaRelTol, "basket/index target"));
    ATX_TRY(double persisted_net, dec(tsv, legs[0], "net_vega"));
    ATX_TRY_VOID(close_to(computed_net, persisted_net, target * kFloatRecomputeTol,
                          "persisted net vega"));
    ATX_TRY(double persisted_gross, dec(tsv, legs[0], "gross_vega"));
    ATX_TRY_VOID(close_to(computed_gross, persisted_gross,
                          std::max(1.0, computed_gross) * kFloatRecomputeTol, "persisted gross vega"));
    ATX_TRY(std::int64_t n_names, intcol(tsv, legs[0], "n_names"));
    if (name_count != n_names) {
      return recon_fail("name count mismatch for roll");
    }
    const double relative = std::abs(computed_net) / target;
    if (relative > kVegaRelTol) {
      return recon_fail("vega residual exceeds tolerance for roll");
    }

    ReferenceReconRecord record;
    record.record_type = "roll";
    record.date = key.first;
    record.cohort = key.second;
    record.computed_net_vega = computed_net;
    record.computed_gross_vega = computed_gross;
    record.relative_vega_residual = relative;
    record.is_roll = true;
    output.push_back(std::move(record));
  }
  return Ok(std::move(output));
}

Result<std::vector<ReferenceReconRecord>>
verify_marks_and_reconciliation(const fs::path &marks_path, const fs::path &reconciliation_path) {
  ATX_TRY(DictTsv marks, read_dict_tsv(marks_path, std::nullopt));
  ATX_TRY(DictTsv expected, read_dict_tsv(reconciliation_path, std::nullopt));

  auto mark_key = [&](std::size_t row) -> Result<std::tuple<std::int64_t, std::string, std::string>> {
    ATX_TRY(std::int64_t cohort, intcol(marks, row, "cohort"));
    return Ok(std::make_tuple(cohort, str(marks, row, "raw_symbol"), str(marks, row, "side")));
  };
  auto raw_ok = [&](std::size_t row) { return str(marks, row, "status") == "Ok"; };

  std::map<std::string, std::vector<std::size_t>> by_date;
  std::vector<std::string> dates;
  std::set<std::array<std::string, 5>> seen_marks;
  for (std::size_t r = 0; r < marks.rows.size(); ++r) {
    ATX_TRY(std::int64_t cohort, intcol(marks, r, "cohort"));
    const std::array<std::string, 5> key{str(marks, r, "date"), str(marks, r, "role"),
                                         std::to_string(cohort), str(marks, r, "raw_symbol"),
                                         str(marks, r, "side")};
    if (!seen_marks.insert(key).second) {
      return recon_fail("duplicate contract mark");
    }
    const std::string date = str(marks, r, "date");
    if (by_date.find(date) == by_date.end()) {
      if (!dates.empty() && date <= dates.back()) {
        return recon_fail("contract mark dates are not ordered");
      }
      dates.push_back(date);
      by_date.emplace(date, std::vector<std::size_t>{});
    }
    by_date[date].push_back(r);
  }

  std::unordered_map<std::string, std::size_t> expected_by_date;
  std::vector<std::string> expected_dates;
  for (std::size_t r = 0; r < expected.rows.size(); ++r) {
    const std::string date = str(expected, r, "date");
    if (expected_by_date.emplace(date, r).second) {
      expected_dates.push_back(date);
    } else {
      expected_by_date[date] = r;
    }
  }
  if (dates != expected_dates) {
    return recon_fail("contract mark/reconciliation dates disagree");
  }

  std::map<std::tuple<std::int64_t, std::string, std::string>, std::size_t> previous;
  double model_nav = 0.0;
  double quote_nav = 0.0;
  std::vector<ReferenceReconRecord> output;
  for (std::size_t date_index = 0; date_index < dates.size(); ++date_index) {
    const std::string &date = dates[date_index];
    const std::vector<std::size_t> &daily = by_date[date];
    std::vector<std::size_t> entries;
    std::vector<std::size_t> held;
    for (const std::size_t row : daily) {
      if (str(marks, row, "role") == "Entry") {
        entries.push_back(row);
      } else if (str(marks, row, "role") == "Held") {
        held.push_back(row);
      }
    }
    double model_pnl = 0.0;
    double quote_pnl = 0.0;
    std::int64_t quote_count = 0;
    std::int64_t held_count = 0;
    std::int64_t held_cohort = 0;
    if (date_index == 0) {
      if (!held.empty() || entries.empty()) {
        return recon_fail("inception must contain entry marks only");
      }
      previous.clear();
      for (const std::size_t row : entries) {
        ATX_TRY(auto key, mark_key(row));
        previous[key] = row;
      }
      held_count = static_cast<std::int64_t>(entries.size());
      for (const std::size_t row : entries) {
        quote_count += raw_ok(row) ? 1 : 0;
      }
      ATX_TRY(held_cohort, intcol(marks, entries.front(), "cohort"));
    } else {
      if (held.empty()) {
        return recon_fail("date has no held marks");
      }
      std::map<std::tuple<std::int64_t, std::string, std::string>, std::size_t> current;
      for (const std::size_t row : held) {
        ATX_TRY(auto key, mark_key(row));
        const auto it = previous.find(key);
        if (it == previous.end()) {
          return recon_fail("missing previous endpoint");
        }
        const std::size_t prior = it->second;
        ATX_TRY(double quantity, dec(marks, row, "quantity"));
        ATX_TRY(double multiplier, dec(marks, row, "multiplier"));
        const double scale = quantity * multiplier;
        ATX_TRY(double model_mark, dec(marks, row, "model_mark"));
        ATX_TRY(double prior_model_mark, dec(marks, prior, "model_mark"));
        model_pnl += scale * (model_mark - prior_model_mark);
        if (raw_ok(row) && raw_ok(prior)) {
          ATX_TRY(double raw_mid, dec(marks, row, "raw_mid"));
          ATX_TRY(double prior_raw_mid, dec(marks, prior, "raw_mid"));
          quote_pnl += scale * (raw_mid - prior_raw_mid);
          ++quote_count;
        }
        current[key] = row;
      }
      held_count = static_cast<std::int64_t>(held.size());
      ATX_TRY(held_cohort, intcol(marks, held.front(), "cohort"));
      previous.clear();
      if (!entries.empty()) {
        for (const std::size_t row : entries) {
          ATX_TRY(auto key, mark_key(row));
          previous[key] = row;
        }
      } else {
        previous = current;
      }
    }
    model_nav += model_pnl;
    quote_nav += quote_pnl;
    const double coverage = static_cast<double>(quote_count) / static_cast<double>(held_count);
    const std::size_t exp_row = expected_by_date[date];
    ATX_TRY(double exp_model_pnl, dec(expected, exp_row, "model_option_pnl"));
    ATX_TRY_VOID(close_to(exp_model_pnl, model_pnl, kPnlAbsTol, "model option P&L"));
    ATX_TRY(double exp_quote_pnl, dec(expected, exp_row, "quote_mid_pnl"));
    ATX_TRY_VOID(close_to(exp_quote_pnl, quote_pnl, kPnlAbsTol, "quote-mid P&L"));
    ATX_TRY(double exp_mmq, dec(expected, exp_row, "model_minus_quote_pnl"));
    ATX_TRY_VOID(close_to(exp_mmq, model_pnl - quote_pnl, kPnlAbsTol, "model-minus-quote P&L"));
    ATX_TRY(double exp_model_nav, dec(expected, exp_row, "model_nav"));
    ATX_TRY_VOID(close_to(exp_model_nav, model_nav, kPnlAbsTol, "model NAV"));
    ATX_TRY(double exp_quote_nav, dec(expected, exp_row, "quote_mid_nav"));
    ATX_TRY_VOID(close_to(exp_quote_nav, quote_nav, kPnlAbsTol, "quote NAV"));
    ATX_TRY(double exp_coverage, dec(expected, exp_row, "quote_mid_coverage"));
    ATX_TRY_VOID(close_to(exp_coverage, coverage, kPnlAbsTol, "quote coverage"));
    ATX_TRY(std::int64_t exp_held_lots, intcol(expected, exp_row, "n_held_lots"));
    ATX_TRY(std::int64_t exp_quote_lots, intcol(expected, exp_row, "n_quote_mid_lots"));
    if (exp_held_lots != held_count || exp_quote_lots != quote_count) {
      return recon_fail("coverage counts disagree");
    }

    ReferenceReconRecord record;
    record.record_type = "date";
    record.date = date;
    record.cohort = held_cohort;
    record.computed_model_option_pnl = model_pnl;
    record.computed_quote_mid_pnl = quote_pnl;
    record.computed_model_nav = model_nav;
    record.computed_quote_mid_nav = quote_nav;
    record.quote_mid_coverage = coverage;
    record.is_roll = false;
    output.push_back(std::move(record));
  }
  return Ok(std::move(output));
}

Status verify_backtest(const fs::path &backtest_path, const fs::path &reconciliation_path) {
  ATX_TRY(DictTsv rows, read_dict_tsv(backtest_path, std::nullopt));
  ATX_TRY(DictTsv reconciliation, read_dict_tsv(reconciliation_path, std::nullopt));
  if (rows.rows.size() != reconciliation.rows.size()) {
    return recon_fail("backtest/reconciliation dates disagree");
  }
  for (std::size_t r = 0; r < rows.rows.size(); ++r) {
    if (str(rows, r, "date") != str(reconciliation, r, "date")) {
      return recon_fail("backtest/reconciliation dates disagree");
    }
  }
  static constexpr std::string_view kAxes[] = {
      "pnl_delta", "pnl_gamma",       "pnl_vega",       "pnl_vanna",   "pnl_volga", "pnl_theta",
      "pnl_rho",   "pnl_charm",       "pnl_unexplained", "pnl_settlement", "pnl_shares", "financing"};
  double nav = 0.0;
  for (std::size_t r = 0; r < rows.rows.size(); ++r) {
    ATX_TRY(double total, dec(rows, r, "pnl_total"));
    double closure = 0.0;
    for (const std::string_view axis : kAxes) {
      ATX_TRY(double value, dec(rows, r, axis));
      closure += value;
    }
    ATX_TRY(double cost, dec(rows, r, "cost"));
    closure -= cost;
    ATX_TRY_VOID(close_to(total, closure, kPnlAbsTol, "backtest P&L closure"));
    nav += total;
    ATX_TRY(double nav_col, dec(rows, r, "nav"));
    ATX_TRY_VOID(close_to(nav_col, nav, kPnlAbsTol, "backtest NAV"));
    ATX_TRY(double settlement, dec(rows, r, "pnl_settlement"));
    ATX_TRY(double shares, dec(rows, r, "pnl_shares"));
    ATX_TRY(double financing, dec(rows, r, "financing"));
    const double option_pnl = total - settlement - shares - financing + cost;
    ATX_TRY(double ref_model_pnl, dec(reconciliation, r, "model_option_pnl"));
    ATX_TRY_VOID(close_to(option_pnl, ref_model_pnl, kPnlAbsTol, "backtest/model-mark option P&L"));
    ATX_TRY(double unpriced_lots, dec(rows, r, "n_unpriced_lots"));
    ATX_TRY(double unpriced_greeks, dec(rows, r, "n_unpriced_greeks"));
    if (unpriced_lots != 0.0 || unpriced_greeks != 0.0) {
      return recon_fail("backtest contains unpriced lots");
    }
  }
  return Ok();
}

} // namespace

// ── Public: fingerprints + corpus config ────────────────────────────────────

std::uint64_t dispersion_hash_text(std::string_view text) {
  const std::uint64_t hash = atx::core::hash_bytes(text.data(), text.size());
  return hash == 0u ? 1u : hash;
}

std::uint64_t dispersion_input_fingerprint(std::string_view date_lo, std::string_view date_hi,
                                           std::size_t n_symbols) {
  return dispersion_hash_text(std::string(date_lo) + "|" + std::string(date_hi) + "|" +
                              std::to_string(n_symbols));
}

QualifiedCorpusConfig dispersion_corpus_config(const DispersionCorpusPolicy &policy,
                                               unsigned fit_workers,
                                               std::uint64_t input_fingerprint) {
  QualifiedCorpusConfig config;
  config.build.n_threads = fit_workers;
  config.build.fit_template.preset = policy.fit_preset;
  CurveConfig direct_curve;
  direct_curve.kind = policy.fit_curve_kind;
  config.build.fit_template.curve = direct_curve;
  config.build.fit_template.enforce_calendar_floor = policy.fit_enforce_calendar_floor;
  config.admission.enabled = true;
  CorpusAdmissionRule rule;
  rule.min_quotes = policy.admission_min_quotes;
  rule.min_slices = policy.admission_min_slices;
  rule.require_calendar_arb_free = policy.admission_require_calendar_arb_free;
  rule.calendar_abs_k = policy.admission_calendar_abs_k;
  rule.require_source_provenance = policy.admission_require_source_provenance;
  for (CorpusAdmissionRule &profile_rule : config.admission.by_profile) {
    profile_rule = rule;
  }
  config.input_fingerprint = input_fingerprint;
  config.policy_fingerprint = dispersion_hash_text(policy.policy_fingerprint_material);
  return config;
}

// ── Public: surface-only backtest compute seam ──────────────────────────────

DispersionBacktestConfig dispersion_backtest_config_from_run_spec(const RunSpec &spec) {
  DispersionBacktestConfig config;
  config.target_dte_days = spec.target_dte_days;
  config.roll_dte_days = spec.roll_dte_days;
  config.gross_index_vega = spec.gross_index_vega;
  config.delta_band = spec.delta_band;
  config.min_names = spec.min_names;
  config.run.unpriced = UnpricedLotPolicy::Error;
  return config;
}

Result<DispersionBacktestOutcome>
run_dispersion_surface_backtest(const Clock &clock, DispersionUniverse universe,
                                const DispersionBacktestConfig &config) {
  ATX_TRY(BacktestResult backtest, run_dispersion_backtest(clock, std::move(universe), config));
  DispersionBacktestOutcome outcome;
  outcome.track = std::move(backtest);
  outcome.sheet = tearsheet(outcome.track);
  return Ok(std::move(outcome));
}

namespace {

// X3 DRAWDOWN STOP. The engine never shows a strategy its NAV, so this limit
// cannot be enforced inside on_step like the sizing limits are. Instead: find the
// first step whose peak-to-trough loss breaches the stop.
//
// MEASURED AGAINST CAPITAL, not against peak NAV. `BacktestResult::nav` is
// cumulative P&L from an inception of ZERO, not an equity curve, so
// "fraction of peak NAV" is degenerate here — peak NAV is 0 on a losing run and
// the ratio is meaningless. A capital base is also what a real risk system
// stops on ("halt after losing 20% of capital"), so `read_dispersion_run_config`
// requires `limit_capital` whenever `limit_drawdown_stop` is set.
//
// Enforcing it needs exactly ONE replay, not a fixed point. Halting only ever
// suppresses entries at or AFTER the breach step, and NAV up to that step is a
// function of trades made strictly before it — so the first breach index is
// invariant under the halt. Any later breach is moot: there is no new risk left
// to stop.
[[nodiscard]] std::optional<std::size_t> first_drawdown_breach(const BacktestResult &track,
                                                               const DispersionRiskLimits &limits) {
  if (!(limits.drawdown_stop > 0.0) || !(limits.capital > 0.0)) {
    return std::nullopt;
  }
  const double allowed = limits.drawdown_stop * limits.capital;
  double peak = 0.0; // inception NAV
  for (std::size_t i = 0; i < track.nav.size(); ++i) {
    const double nav = track.nav[i];
    peak = std::max(peak, nav);
    if (peak - nav > allowed) {
      return i;
    }
  }
  return std::nullopt;
}

} // namespace

Result<DispersionBacktestOutcome>
run_dispersion_surface_backtest(const Clock &clock, std::vector<UniverseRow> schedule,
                                const DispersionBacktestConfig &config,
                                std::string_view index_symbol) {
  DispersionStrategy strategy =
      make_dispersion_backtest_strategy(schedule, config, index_symbol);
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config.run));

  if (config.limits.drawdown_stop > 0.0) {
    if (const std::optional<std::size_t> breach = first_drawdown_breach(backtest, config.limits)) {
      DispersionStrategy stopped =
          make_dispersion_backtest_strategy(std::move(schedule), config, index_symbol);
      stopped.halt_from_step(*breach);
      ATX_TRY(BacktestResult halted, run_backtest(clock, stopped, config.run));
      backtest = std::move(halted);
    }
  }

  DispersionBacktestOutcome outcome;
  outcome.track = std::move(backtest);
  outcome.sheet = tearsheet(outcome.track);
  return Ok(std::move(outcome));
}

// ── Public: X1 strict typed run config ──────────────────────────────────────

namespace {

// Alias so the comma in the template argument list does not split the ATX_TRY
// macro's argument list at the call site.
using KvMap = std::map<std::string, std::string>;

// A key/value TSV read into an ordered map, with duplicate keys rejected. Shared
// shape with read_run_spec, but this reader OWNS the key vocabulary: anything it
// does not consume is an error, so the "silently ignored key" class of bug is
// structurally impossible rather than merely discouraged.
Result<KvMap> read_kv_tsv(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::NotFound, "cannot open " + path.string());
  }
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    return Err(ErrorCode::IoError, "cannot read " + path.string());
  }
  KvMap values;
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
    const std::size_t tab = line.find('\t');
    if (tab == std::string_view::npos || line.find('\t', tab + 1) != std::string_view::npos) {
      return Err(ErrorCode::ParseError, "run config must contain key/value TSV rows");
    }
    const std::string key{line.substr(0, tab)};
    if (key == "key") { // the header row
      continue;
    }
    if (!values.emplace(key, std::string(line.substr(tab + 1))).second) {
      return Err(ErrorCode::AlreadyExists, "duplicate run config key '" + key + "'");
    }
  }
  return Ok(std::move(values));
}

// Binds keys to typed fields and tracks which were consumed, so the leftovers can
// be reported BY NAME.
class StrictBinder {
public:
  StrictBinder(KvMap values, fs::path base)
      : values_{std::move(values)}, base_{std::move(base)} {}

  [[nodiscard]] const std::string *find(std::string_view key) {
    const auto found = values_.find(std::string(key));
    if (found == values_.end()) {
      return nullptr;
    }
    consumed_.insert(found->first);
    return &found->second;
  }

  Status text(std::string_view key, std::string &out) {
    if (const std::string *value = find(key)) {
      out = *value;
    }
    return Ok();
  }

  Status path_key(std::string_view key, fs::path &out) {
    if (const std::string *value = find(key)) {
      if (!value->empty()) {
        fs::path candidate{*value};
        out = candidate.is_absolute() ? candidate.lexically_normal()
                                      : (base_ / candidate).lexically_normal();
      }
    }
    return Ok();
  }

  template <class T> Status number(std::string_view key, T &out) {
    const std::string *value = find(key);
    if (value == nullptr) {
      return Ok();
    }
    T parsed{};
    const char *first = value->data();
    const char *last = first + value->size();
    std::from_chars_result result{};
    if constexpr (std::is_floating_point_v<T>) {
      result = std::from_chars(first, last, parsed, std::chars_format::general);
    } else {
      result = std::from_chars(first, last, parsed);
    }
    if (result.ec != std::errc{} || result.ptr != last) {
      return Err(ErrorCode::ParseError, "run config key '" + std::string(key) +
                                            "' is not a valid number: '" + *value + "'");
    }
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(parsed)) {
        return Err(ErrorCode::ParseError,
                   "run config key '" + std::string(key) + "' must be finite");
      }
    }
    out = parsed;
    return Ok();
  }

  Status boolean(std::string_view key, bool &out) {
    const std::string *value = find(key);
    if (value == nullptr) {
      return Ok();
    }
    if (*value == "1" || *value == "true") {
      out = true;
    } else if (*value == "0" || *value == "false") {
      out = false;
    } else {
      return Err(ErrorCode::ParseError, "run config key '" + std::string(key) +
                                            "' must be 0/1/true/false, got '" + *value + "'");
    }
    return Ok();
  }

  // Enumerated key: only the listed spellings are accepted, and the error lists
  // them, so an unimplemented mode fails loudly instead of being ignored.
  template <class T>
  Status enumerated(std::string_view key, T &out,
                    std::initializer_list<std::pair<std::string_view, T>> options) {
    const std::string *value = find(key);
    if (value == nullptr) {
      return Ok();
    }
    std::string allowed;
    for (const auto &option : options) {
      if (*value == option.first) {
        out = option.second;
        return Ok();
      }
      allowed += (allowed.empty() ? "" : ", ");
      allowed += option.first;
    }
    return Err(ErrorCode::InvalidArgument, "run config key '" + std::string(key) +
                                               "' has unsupported value '" + *value +
                                               "'; supported: " + allowed);
  }

  // THE strict check. Any key never bound above is a hard error naming the key.
  [[nodiscard]] Status reject_unknown() const {
    std::string unknown;
    for (const auto &entry : values_) {
      if (consumed_.find(entry.first) == consumed_.end()) {
        unknown += (unknown.empty() ? "" : ", ");
        unknown += entry.first;
      }
    }
    if (!unknown.empty()) {
      return Err(ErrorCode::InvalidArgument, "unknown run config key(s): " + unknown);
    }
    return Ok();
  }

private:
  KvMap values_;
  std::set<std::string> consumed_;
  fs::path base_;
};

} // namespace

FrictionModel dispersion_friction_preset(DispersionFrictionPreset preset) {
  FrictionModel model;
  switch (preset) {
  case DispersionFrictionPreset::None:
    return model; // frictionless mid — exactly the pinned golden
  case DispersionFrictionPreset::RetailListedOptions:
    // A documented, deliberately conservative listed-options execution setting:
    // 25 bps half-spread on the option premium (a ~0.5% round-trip, typical of a
    // liquid ATM single-name straddle), $0.65/contract commission, and 1 bp of
    // slippage on the delta-hedge shares. These are ILLUSTRATIVE opt-in defaults,
    // NOT a fitted calibration.
    model.spread_kind = FrictionModel::SpreadKind::PriceBps;
    model.half_spread_bps = 25.0;
    model.per_contract_cost = 0.65;
    model.hedge_slippage_bps = 1.0;
    return model;
  }
  return model;
}

Result<DispersionRunConfig> read_dispersion_run_config(const fs::path &path) {
  ATX_TRY(KvMap values, read_kv_tsv(path));
  StrictBinder binder{std::move(values), path.parent_path()};
  DispersionRunConfig config;

  const auto required_text = [&](std::string_view key, std::string &out) -> Status {
    const std::string *value = binder.find(key);
    if (value == nullptr || value->empty()) {
      return Err(ErrorCode::ParseError, "missing run config key '" + std::string(key) + "'");
    }
    out = *value;
    return Ok();
  };
  ATX_TRY_VOID(required_text("date_lo", config.dates.lo));
  ATX_TRY_VOID(required_text("date_hi", config.dates.hi));
  std::string opra_root_text;
  std::string universe_text;
  ATX_TRY_VOID(required_text("opra_root", opra_root_text));
  ATX_TRY_VOID(required_text("universe_schedule", universe_text));
  {
    const fs::path base = path.parent_path();
    const auto resolve = [&](const std::string &text) {
      fs::path candidate{text};
      return candidate.is_absolute() ? candidate.lexically_normal()
                                     : (base / candidate).lexically_normal();
    };
    config.opra_root = resolve(opra_root_text);
    config.universe.schedule_path = resolve(universe_text);
  }

  ATX_TRY_VOID(binder.text("label", config.label));
  ATX_TRY_VOID(binder.text("snapshot_suffix", config.snapshot_suffix));
  ATX_TRY_VOID(binder.text("path_template", config.path_template));
  ATX_TRY_VOID(binder.path_key("definitions", config.definitions));
  ATX_TRY_VOID(binder.path_key("occ_ess_root", config.occ_ess_root));

  ATX_TRY_VOID(binder.text("index_symbol", config.universe.index_symbol));
  ATX_TRY_VOID(binder.number("min_names", config.universe.min_names));
  ATX_TRY_VOID(binder.number("min_weight_coverage", config.universe.min_weight_coverage));

  ATX_TRY_VOID(binder.number("flat_rate", config.rate.flat_rate));
  ATX_TRY_VOID(binder.boolean("rate_applies_to_financing", config.rate.apply_to_financing));

  ATX_TRY_VOID(binder.enumerated("side", config.side,
                                 {{"short_index_long_names", DispersionSide::ShortIndexLongNames},
                                  {"long_index_short_names", DispersionSide::LongIndexShortNames}}));
  ATX_TRY_VOID(binder.enumerated("weighting", config.weighting,
                                 {{"vega_neutral", DispersionWeighting::VegaNeutral}}));
  ATX_TRY_VOID(binder.enumerated(
      "strike", config.strike,
      {{"atm_forward_straddle", DispersionStrikeRule::AtmForwardStraddle}}));

  ATX_TRY_VOID(binder.number("target_dte_days", config.dte.target_days));
  ATX_TRY_VOID(binder.number("min_dte_days", config.dte.min_days));
  ATX_TRY_VOID(binder.number("max_dte_days", config.dte.max_days));
  ATX_TRY_VOID(binder.number("roll_dte_days", config.roll_dte_days));
  ATX_TRY_VOID(binder.number("gross_index_vega", config.gross_index_vega));
  ATX_TRY_VOID(binder.number("multiplier", config.multiplier));
  ATX_TRY_VOID(binder.number("entry_every_n", config.entry_every_n));
  ATX_TRY_VOID(binder.boolean("record_diagnostics", config.record_diagnostics));

  ATX_TRY_VOID(binder.enumerated("hedge", config.hedge.kind,
                                 {{"none", HedgeSpec::Kind::None},
                                  {"delta_to_zero", HedgeSpec::Kind::DeltaToZero}}));
  ATX_TRY_VOID(binder.enumerated("hedge_cadence", config.hedge.cadence,
                                 {{"at_entry", HedgeSpec::Cadence::AtEntry},
                                  {"daily", HedgeSpec::Cadence::Daily}}));
  ATX_TRY_VOID(binder.number("delta_band", config.hedge.band));

  // X2 frictions. A preset is applied first so explicit keys can refine it.
  DispersionFrictionPreset preset = DispersionFrictionPreset::None;
  ATX_TRY_VOID(binder.enumerated(
      "friction_preset", preset,
      {{"none", DispersionFrictionPreset::None},
       {"retail_listed_options", DispersionFrictionPreset::RetailListedOptions}}));
  config.frictions = dispersion_friction_preset(preset);
  ATX_TRY_VOID(binder.enumerated("friction_spread_kind", config.frictions.spread_kind,
                                 {{"none", FrictionModel::SpreadKind::None},
                                  {"price_bps", FrictionModel::SpreadKind::PriceBps},
                                  {"vol_ticks", FrictionModel::SpreadKind::VolTicks}}));
  ATX_TRY_VOID(binder.number("friction_half_spread_bps", config.frictions.half_spread_bps));
  ATX_TRY_VOID(binder.number("friction_vol_tick", config.frictions.vol_tick));
  ATX_TRY_VOID(binder.number("friction_per_contract_cost", config.frictions.per_contract_cost));
  ATX_TRY_VOID(binder.number("friction_hedge_slippage_bps", config.frictions.hedge_slippage_bps));

  // X2 financing.
  ATX_TRY_VOID(binder.number("financing_borrow_rate", config.financing.borrow_rate));
  ATX_TRY_VOID(binder.boolean("financing_finance_premium", config.financing.finance_premium));
  ATX_TRY_VOID(binder.boolean("financing_shares_carry", config.financing.shares_carry));
  ATX_TRY_VOID(binder.number("financing_initial_cash", config.financing.initial_cash));

  // X6 costs.
  ATX_TRY_VOID(binder.number("cost_impact_k", config.costs.k));
  ATX_TRY_VOID(binder.number("cost_impact_beta", config.costs.beta));
  ATX_TRY_VOID(binder.number("cost_adv_fraction", config.costs.adv_fraction));

  // X3 limits.
  ATX_TRY_VOID(binder.number("limit_max_gross_vega", config.limits.max_gross_vega));
  ATX_TRY_VOID(binder.number("limit_max_gross_notional", config.limits.max_gross_notional));
  ATX_TRY_VOID(binder.number("limit_capital", config.limits.capital));
  ATX_TRY_VOID(binder.number("limit_drawdown_stop", config.limits.drawdown_stop));
  ATX_TRY_VOID(binder.enumerated(
      "limit_action", config.limits.action,
      {{"clamp", RiskBreachAction::Clamp}, {"halt", RiskBreachAction::Halt}}));

  ATX_TRY_VOID(binder.number("fit_workers", config.fit.workers));
  ATX_TRY_VOID(binder.boolean("core_mode", config.fit.core_mode));
  ATX_TRY_VOID(binder.enumerated(
      "provenance", config.provenance,
      {{"compatibility", SurfaceProvenancePolicy::Compatibility},
       {"require_admitted_risk", SurfaceProvenancePolicy::RequireAdmittedRisk}}));

  // Strictness: everything not bound above is rejected, by name.
  ATX_TRY_VOID(binder.reject_unknown());

  // Contract validation — the invariants read_run_spec enforced, plus the ones
  // the new knobs need.
  if (config.dates.lo > config.dates.hi || config.universe.min_names == 0 ||
      config.universe.min_weight_coverage <= 0.0 || config.universe.min_weight_coverage > 1.0 ||
      config.dte.min_days <= 0.0 || config.dte.target_days < config.dte.min_days ||
      config.dte.max_days < config.dte.target_days || config.roll_dte_days < 0.0 ||
      config.gross_index_vega <= 0.0 || config.hedge.band < 0.0) {
    return Err(ErrorCode::InvalidArgument, "invalid run config contract");
  }
  if (config.fit.core_mode &&
      (config.universe.min_names < 40 || config.universe.min_weight_coverage < 0.8)) {
    return Err(ErrorCode::InvalidArgument, "core mode requires >=40 names and >=80% weight");
  }
  if (config.multiplier <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "multiplier must be positive");
  }
  if (config.entry_every_n == 0u) {
    return Err(ErrorCode::InvalidArgument, "entry_every_n must be positive");
  }
  if (config.costs.k < 0.0 || config.costs.beta <= 0.0 || config.costs.adv_fraction < 0.0) {
    return Err(ErrorCode::InvalidArgument, "invalid transaction-cost model");
  }
  if (config.limits.max_gross_vega < 0.0 || config.limits.max_gross_notional < 0.0 ||
      config.limits.capital < 0.0 || config.limits.drawdown_stop < 0.0 ||
      config.limits.drawdown_stop >= 1.0) {
    return Err(ErrorCode::InvalidArgument,
               "invalid risk limits (drawdown_stop is a fraction in [0, 1))");
  }
  if (config.limits.drawdown_stop > 0.0 && !(config.limits.capital > 0.0)) {
    // The track's NAV is cumulative P&L from zero, not an equity curve, so a
    // drawdown stop is only well defined against a capital base. Refuse rather
    // than silently measuring against a meaningless peak.
    return Err(ErrorCode::InvalidArgument,
               "limit_drawdown_stop requires limit_capital (the drawdown base)");
  }
  if (config.universe.index_symbol.empty()) {
    return Err(ErrorCode::InvalidArgument, "index_symbol must not be empty");
  }
  return Ok(std::move(config));
}

RunSpec run_spec_from(const DispersionRunConfig &config) {
  RunSpec spec;
  spec.label = config.label;
  spec.date_lo = config.dates.lo;
  spec.date_hi = config.dates.hi;
  spec.snapshot_suffix = config.snapshot_suffix;
  spec.opra_root = config.opra_root;
  spec.path_template = config.path_template;
  spec.universe_path = config.universe.schedule_path;
  spec.definitions_path = config.definitions;
  spec.occ_ess_root = config.occ_ess_root;
  spec.flat_rate = config.rate.flat_rate;
  spec.min_names = config.universe.min_names;
  spec.min_weight_coverage = config.universe.min_weight_coverage;
  spec.target_dte_days = config.dte.target_days;
  spec.min_dte_days = config.dte.min_days;
  spec.max_dte_days = config.dte.max_days;
  spec.roll_dte_days = config.roll_dte_days;
  spec.gross_index_vega = config.gross_index_vega;
  spec.delta_band = config.hedge.band;
  spec.fit_workers = config.fit.workers;
  spec.core_mode = config.fit.core_mode;
  return spec;
}

DispersionBacktestConfig dispersion_backtest_config_from(const DispersionRunConfig &config) {
  DispersionBacktestConfig backtest;
  backtest.target_dte_days = config.dte.target_days;
  backtest.roll_dte_days = config.roll_dte_days;
  backtest.gross_index_vega = config.gross_index_vega;
  backtest.delta_band = config.hedge.band;
  backtest.min_names = config.universe.min_names;
  backtest.entry_every_n = config.entry_every_n;
  backtest.record_diagnostics = config.record_diagnostics;
  backtest.side = config.side;
  backtest.multiplier = config.multiplier;
  backtest.hedge_kind = config.hedge.kind;
  backtest.hedge_cadence = config.hedge.cadence;
  backtest.limits = config.limits;
  backtest.run.unpriced = UnpricedLotPolicy::Error;
  // X2/X6: the wiring that never existed. The dispersion path built a RunConfig
  // that left `frictions` and `financing` default-constructed, so every fill was a
  // frictionless mid and no carry ever accrued, regardless of the run spec.
  backtest.run.frictions = dispersion_effective_frictions(config.frictions, config.costs);
  backtest.run.financing = config.financing;
  if (config.rate.apply_to_financing) {
    // `flat_rate` previously reached the fit batch ONLY. Opting in routes the same
    // rate into the cash/borrow ledger so a declared r actually accrues carry.
    backtest.run.financing.borrow_rate = config.rate.flat_rate;
    backtest.run.financing.finance_premium = true;
  }
  return backtest;
}

Status verify_projected_var_artifacts(const fs::path &run_dir, std::size_t n_sessions) {
  const fs::path summary_path = run_dir / "projected_var.tsv";
  std::error_code error;
  if (!fs::is_regular_file(summary_path, error)) {
    return Ok(); // the stage was not run; nothing to gate
  }
  // Present => the whole triple must be present, non-empty and consistent.
  for (const char *leaf : {"projected_risk_scenarios.tsv", "projected_risk_legs.tsv"}) {
    const fs::path companion = run_dir / leaf;
    if (!fs::is_regular_file(companion, error) || fs::file_size(companion, error) == 0u) {
      return Err(ErrorCode::NotFound,
                 "projected VaR summary present but companion missing: " + companion.string());
    }
  }
  ATX_TRY(std::string summary_text, [&]() -> Result<std::string> {
    std::ifstream stream(summary_path, std::ios::binary);
    if (!stream) {
      return Err(ErrorCode::IoError, "cannot read projected_var.tsv");
    }
    return Ok(std::string((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>()));
  }());
  constexpr std::string_view kHeader =
      "confidence\treference_value\tvalue_at_risk\texpected_shortfall\tn_scenarios\t"
      "n_positions\tprojections_per_second\tprepared_fingerprint";
  std::size_t line_start = 0;
  std::size_t n_rows = 0;
  bool header_seen = false;
  while (line_start < summary_text.size()) {
    const std::size_t line_end = summary_text.find('\n', line_start);
    std::string_view line{summary_text.data() + line_start,
                          (line_end == std::string::npos ? summary_text.size() : line_end) -
                              line_start};
    line_start = line_end == std::string::npos ? summary_text.size() : line_end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      continue;
    }
    if (!header_seen) {
      if (line != kHeader) {
        return Err(ErrorCode::ParseError, "projected_var.tsv header does not match the contract");
      }
      header_seen = true;
      continue;
    }
    // Field 5 (0-based 4) is n_scenarios: every confidence row must cover the
    // whole clock, which is what catches a truncated or stale run.
    std::size_t field = 0;
    std::size_t cursor = 0;
    while (field < 4 && cursor != std::string_view::npos) {
      cursor = line.find('\t', cursor);
      if (cursor != std::string_view::npos) {
        ++cursor;
      }
      ++field;
    }
    if (cursor == std::string_view::npos) {
      return Err(ErrorCode::ParseError, "projected_var.tsv row is malformed");
    }
    const std::size_t end = line.find('\t', cursor);
    const std::string_view text =
        line.substr(cursor, end == std::string_view::npos ? std::string_view::npos : end - cursor);
    std::size_t n_scenarios = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), n_scenarios);
    if (parsed.ec != std::errc{}) {
      return Err(ErrorCode::ParseError, "projected_var.tsv n_scenarios is not a number");
    }
    if (n_sessions != 0 && n_scenarios != n_sessions) {
      return Err(ErrorCode::InvalidArgument,
                 "projected VaR covers " + std::to_string(n_scenarios) + " scenarios but the run has " +
                     std::to_string(n_sessions) + " sessions");
    }
    ++n_rows;
  }
  if (!header_seen || n_rows == 0) {
    return Err(ErrorCode::InvalidArgument, "projected_var.tsv has no rows");
  }
  return Ok();
}

// ── Public: native reference reconciliation (M1) ────────────────────────────

Result<std::vector<ReferenceReconRecord>>
reconcile_dispersion_reference(const fs::path &run_dir, bool schedule_only) {
  ATX_TRY(std::vector<ReferenceReconRecord> output, verify_schedule(run_dir / "trade_schedule.tsv"));
  if (!schedule_only) {
    ATX_TRY(std::vector<ReferenceReconRecord> reconciliation,
            verify_marks_and_reconciliation(run_dir / "contract_marks.tsv",
                                            run_dir / "reconciliation.tsv"));
    output.insert(output.end(), std::make_move_iterator(reconciliation.begin()),
                  std::make_move_iterator(reconciliation.end()));
    ATX_TRY_VOID(verify_backtest(run_dir / "backtest.tsv", run_dir / "reconciliation.tsv"));
  }
  return Ok(std::move(output));
}

Status write_reference_reconciliation_file(const fs::path &path,
                                           std::span<const ReferenceReconRecord> records) {
  const fs::path pending = path.string() + ".pending";
  {
    std::ofstream out(pending, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Err(ErrorCode::IoError, "cannot write reference reconciliation");
    }
    out << std::setprecision(17)
        << "record_type\tdate\tcohort\tcomputed_net_vega\tcomputed_gross_vega\t"
           "relative_vega_residual\tcomputed_model_option_pnl\tcomputed_quote_mid_pnl\t"
           "computed_model_nav\tcomputed_quote_mid_nav\tquote_mid_coverage\tstatus\n";
    for (const ReferenceReconRecord &record : records) {
      out << record.record_type << '\t' << record.date << '\t' << record.cohort << '\t';
      if (record.is_roll) {
        out << record.computed_net_vega << '\t' << record.computed_gross_vega << '\t'
            << record.relative_vega_residual << "\tNA\tNA\tNA\tNA\tNA";
      } else {
        out << "NA\tNA\tNA\t" << record.computed_model_option_pnl << '\t'
            << record.computed_quote_mid_pnl << '\t' << record.computed_model_nav << '\t'
            << record.computed_quote_mid_nav << '\t' << record.quote_mid_coverage;
      }
      out << "\tOk\n";
    }
    if (!out) {
      return Err(ErrorCode::IoError, "cannot flush reference reconciliation");
    }
  }
  std::error_code error;
  fs::rename(pending, path, error);
  if (error) {
    return Err(ErrorCode::IoError, "cannot publish reference reconciliation");
  }
  return Ok();
}

// ── Public: file-oriented workflow entry points ─────────────────────────────

Status dispersion_build_corpus(const fs::path &source_spec_path, const fs::path &run_dir,
                               const DispersionCorpusPolicy &policy) {
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
  const std::uint64_t input_fingerprint =
      dispersion_input_fingerprint(spec.date_lo, spec.date_hi, symbols.size());
  QualifiedCorpusConfig config =
      dispersion_corpus_config(policy, spec.fit_workers, input_fingerprint);
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

Status dispersion_build_schedule(const fs::path &run_dir) {
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
      const SurfaceRef surface = snapshot.find(member.uid);
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

Status dispersion_run_backtest(const fs::path &run_dir) {
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
  ATX_TRY(BacktestResult backtest, run_backtest(clock, strategy, config));
  if (!strategy.all_rolls_consumed()) {
    return Err(ErrorCode::Unavailable, "backtest did not consume every scheduled roll");
  }
  ATX_TRY_VOID(write_backtest_tsv(backtest, (run_dir / "backtest.tsv").string()));

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
  ATX_TRY_VOID(
      write_listed_contract_marks_file((run_dir / "contract_marks.tsv").string(), reconciliation));
  ATX_TRY_VOID(
      write_listed_reconciliation_file((run_dir / "reconciliation.tsv").string(), reconciliation));
  std::printf("backtest complete: dates=%zu rolls=%zu final_nav=%.10g\n", backtest.size(),
              schedule.rolls.size(), backtest.nav.back());
  return Ok();
}

Status dispersion_run_surface_backtest(const fs::path &run_dir) {
  // X1. The surface path now reads the STRICT typed config, so an unknown or
  // misspelled key fails the run by name instead of being silently dropped, and
  // every knob it declares (frictions, financing, limits, costs, multiplier,
  // side, hedge, entry cadence, diagnostics) actually reaches the engine.
  ATX_TRY(DispersionRunConfig run_config, read_dispersion_run_config(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  if (clock.size() == 0u) {
    return Err(ErrorCode::Unavailable, "surface backtest: empty qualified clock");
  }
  // C1-ACTIVATE. Validate that SOME block is effective on the first session (a
  // schedule that only starts mid-window is an authoring error we still want to
  // fail fast on), then hand the WHOLE schedule to the point-in-time overload
  // instead of freezing this first-day resolution for all 82 sessions. WS-C made
  // DispersionStrategy PIT-capable; this is the call site that switches it on.
  ATX_TRY_VOID(
      universe_at(universe_rows, clock.refs().front().date, run_config.universe.index_symbol));

  const DispersionBacktestConfig config = dispersion_backtest_config_from(run_config);
#if defined(ATX_VOL_PROFILE)
  phase_profile::reset();
#endif
#if defined(ATX_VOL_COUNTERS)
  counters::reset();
#endif
  ATX_TRY(DispersionBacktestOutcome outcome,
          run_dispersion_surface_backtest(clock, universe_rows, config,
                                          run_config.universe.index_symbol));
  const BacktestResult &backtest = outcome.track;
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

Status dispersion_run_projected_var(const fs::path &run_dir) {
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

  // C1-ACTIVATE (projected VaR). The book this VaR is measured on is built from
  // ONE anchor snapshot, so "point-in-time" here means resolving the basket from
  // the schedule at THAT snapshot's own timestamp rather than string-matching the
  // manifest's first session date. Routing it through the shared PIT resolver
  // removes the day-1 freeze and the manifest-string coupling in one move; with a
  // single-block schedule the resolved basket is identical to before.
  //
  // NOTE for the PM: the anchor remains the FIRST session while the VaR reference
  // value is `frames.back().value` (the LAST session). That first/last mismatch is
  // a separate modeling question from PIT membership and is left as-is here.
  const auto pit = make_pit_universe_resolver(universe_rows);
  ATX_TRY(DispersionUniverse authored, pit(snapshots.front()->ts_ns()));
  ATX_TRY(ResolvedUniverse resolved,
          resolve_universe_uids(
              authored, [&](std::string_view symbol) { return snapshots.front()->uid_of(symbol); },
              MissingNameSpec{MissingNamePolicy::DropRenormalize, spec.min_names}));
  DispersionConfig dispersion;
  dispersion.target_T = spec.target_dte_days / 365.25;
  dispersion.target_vega = spec.gross_index_vega;
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

Result<DispersionVerifyReport> dispersion_verify(const fs::path &run_dir) {
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
        run_dir / "reconciliation.tsv"}) {
    std::error_code error;
    if (!fs::is_regular_file(required, error) || fs::file_size(required, error) == 0u) {
      return Err(ErrorCode::NotFound, "missing final artifact " + required.string());
    }
  }
  if (quality.n_admitted != manifest.n_ok) {
    return Err(ErrorCode::InvalidArgument, "quality/manifest admitted count mismatch");
  }
  // `run-projected-var` is an OPTIONAL stage, so its artifacts are gated only
  // when present — but they ARE now gated. Previously the command could emit a
  // truncated or stale projected_var.tsv and `verify` would pass it silently.
  ATX_TRY_VOID(verify_projected_var_artifacts(run_dir, clock.size()));
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
  // M1: native reference reconciliation — recompute + numerically compare the
  // persisted arithmetic (independent of the engine), then publish the sidecar.
  ATX_TRY(std::vector<ReferenceReconRecord> records, reconcile_dispersion_reference(run_dir, false));
  ATX_TRY_VOID(
      write_reference_reconciliation_file(run_dir / "reference_reconciliation.tsv", records));
  std::printf("verified artifact envelope: dates=%zu admitted=%u rolls=%zu\n", clock.size(),
              quality.n_admitted, schedule.rolls.size());
  DispersionVerifyReport report;
  report.n_dates = clock.size();
  report.n_admitted = quality.n_admitted;
  report.n_rolls = schedule.rolls.size();
  return Ok(report);
}

} // namespace atx::vol

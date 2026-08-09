// Library seam for the traditional SPY listed-options dispersion proxy.
//
// The command workflow that used to live in tools/spy_dispersion_backtest.cpp
// lives here. Each stage is a plain function so it can be driven from a unit test
// off the filesystem, and the reproduction-critical admission constants are named
// on DispersionCorpusPolicy (see dispersion_run.hpp). This is a behavior-preserving
// extraction: the dispersion golden (final_nav = 24740.624124981368, post-E1) is
// unchanged by THIS extraction.

#include "atx/vol/research/dispersion_run.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include "atx/vol/detail/counters.hpp"
#include "atx/vol/detail/log_emit.hpp"
#include "atx/vol/detail/archive_util.hpp"
#include "atx/vol/dispersion.hpp" // contract_vega_per_vol_point (the ONE vol-point conversion)
#include "atx/vol/historical_projection.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/research/listed_dispersion_pipeline.hpp" // ListedDispersionMethodology (L9)
#include "atx/vol/research/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/listed_dispersion_strategy.hpp"
#include "atx/vol/listed_opra.hpp"
#include "atx/vol/occ_ess.hpp"
#include "atx/vol/opra_batch.hpp"
#include "atx/vol/detail/phase_profile.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/research/run_archive.hpp" // RunDir, encode_*_section (S3-T16 .atxrun default)
#include "atx/vol/strategy.hpp"
#include "atx/vol/track_key.hpp" // kBacktestEconomicsRev (E1 fix round: artifact-level economics rev)
#include "atx/vol/universe.hpp"

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

Status append_input_inventory(std::ostream &out, const OpraBatchResult &batch) {
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

Status persist_occ_ess_evidence_window(const fs::path &run_dir, const RunSpec &spec,
                                       const OpraBatchResult &batch,
                                       std::ostream &inventory,
                                       std::set<std::string> &persisted_dates) {
  std::set<std::string> loaded_dates;
  for (const OpraBatchEntry &entry : batch.entries) {
    if (entry.panel) {
      loaded_dates.insert(entry.date);
    }
  }
  const fs::path evidence_dir = run_dir / "occ_ess";
  std::error_code error;
  for (const std::string &date : loaded_dates) {
    if (!persisted_dates.emplace(date).second) {
      continue;
    }
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

// â”€â”€ Native reference reconciliation (ported from tools/reference_spy_dispersion.py)

// REVIEW C-2 follow-up. `kVegaScale = 0.01` used to live here â€” a FOURTH private
// copy of the per-contract vol-point conversion, in the one place a drift would
// be MASKED rather than caught, since this validator is what re-derives the
// persisted `vega_per_contract_per_vol_point` column. It is gone; the site below
// calls `contract_vega_per_vol_point` (dispersion.hpp) like the other three.
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

// MINORS M10: the third accessor now fails on the same condition as the other
// two. It used to answer a reference to a function-local static empty string
// when the column was absent, so a missing STRING column degraded into "" and
// flowed on while a missing NUMERIC column stopped the run. Two silent
// outcomes, both demonstrated in dispersion_run_test.cpp: a trade_schedule.tsv
// that had lost `roll_date` reconciled clean and published roll records with an
// empty date, and contract marks that had lost `status` scored every lot "no
// usable quote" and agreed with a reconciliation file that recorded the same
// zero coverage for the same reason. Returned BY VALUE rather than by reference
// so no caller can hold a pointer into a row: the strings are short artifact
// cells and every existing caller already copied them.
Result<std::string> str(const DictTsv &tsv, std::size_t row, std::string_view name) {
  const std::string *value = tsv.cell(row, name);
  if (value == nullptr) {
    return recon_fail("missing string column " + std::string(name));
  }
  return Ok(*value);
}

// Every comparison involving a NaN is false, so `|actual - expected| > tolerance`
// used to PASS whenever either side had gone non-finite â€” this validator agreed
// with the artifact it exists to check, and the caller published the NaN. An Inf
// TOLERANCE is the same failure from the other side: it admits every value.
// `dec()` already refuses a literal "nan"/"inf" cell, so a non-finite number
// reaches here only by arithmetic (an overflowing quantity x multiplier, a
// division by a zero pair vega), which is exactly when the recomputation is
// worthless and must be reported rather than accepted.
Status close_to(double actual, double expected, double tolerance, const char *label) {
  if (!std::isfinite(actual) || !std::isfinite(expected) || !std::isfinite(tolerance)) {
    return recon_fail(std::string(label) + " is not a finite number");
  }
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
    ATX_TRY(std::string roll_date, str(tsv, r, "roll_date"));
    const std::pair<std::string, std::int64_t> key{roll_date, cohort};
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
    ATX_TRY(std::string symbol, str(tsv, r, "symbol"));
    ATX_TRY(std::string raw_symbol, str(tsv, r, "raw_symbol"));
    ATX_TRY(std::string side, str(tsv, r, "side"));
    const std::array<std::string, 4> contract_key{roll_date, std::move(symbol),
                                                  std::move(raw_symbol), std::move(side)};
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
      ATX_TRY(std::string call_side, str(tsv, call, "side"));
      ATX_TRY(std::string put_side, str(tsv, put, "side"));
      bool pair_ok = call_side == "C" && put_side == "P";
      for (const std::string_view field : kPairFields) {
        ATX_TRY(std::string call_value, str(tsv, call, field));
        ATX_TRY(std::string put_value, str(tsv, put, field));
        if (call_value != put_value) {
          pair_ok = false;
        }
      }
      if (!pair_ok) {
        return recon_fail("invalid call/put pair for roll");
      }
      ATX_TRY(std::string index_flag, str(tsv, call, "is_index"));
      const bool is_index = index_flag == "1";
      if (is_index != (pair_index == 0)) {
        return recon_fail("index pair ordering mismatch for roll");
      }

      ATX_TRY(double quantity, dec(tsv, call, "quantity"));
      double pair_vega = 0.0;
      double pair_achieved = 0.0;
      for (const std::size_t leg : {call, put}) {
        ATX_TRY(double multiplier, dec(tsv, leg, "multiplier"));
        ATX_TRY(double unit_vega, dec(tsv, leg, "vega_per_unit_vol"));
        // C-2 follow-up: THE one conversion, not a local restatement of it.
        // `contract_vega_per_vol_point` is `(v * m) * kVegaPerVolPoint`, the exact
        // association this line already used, so the number is bit-identical.
        const double contract_vega = contract_vega_per_vol_point(unit_vega, multiplier);
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
      // A straddle pair with no usable vega cannot have been sized vega-flat, and
      // dividing by it yields an Inf (nonzero target) or NaN (zero target)
      // expected quantity. `close_to` now refuses a non-finite side, so this
      // would be caught either way â€” but as a "vega-flat quantity" tolerance
      // failure, which names the symptom rather than the unusable divisor the
      // artifact actually carries. Fail here, the way every other malformed-roll
      // condition in this validator does.
      if (!std::isfinite(pair_vega) || pair_vega == 0.0) {
        return recon_fail("non-finite or zero pair vega for roll");
      }
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
    ATX_TRY(std::string raw_symbol, str(marks, row, "raw_symbol"));
    ATX_TRY(std::string side, str(marks, row, "side"));
    return Ok(std::make_tuple(cohort, std::move(raw_symbol), std::move(side)));
  };
  auto raw_ok = [&](std::size_t row) -> Result<bool> {
    ATX_TRY(std::string status, str(marks, row, "status"));
    return Ok(status == "Ok");
  };

  std::map<std::string, std::vector<std::size_t>> by_date;
  std::vector<std::string> dates;
  std::set<std::array<std::string, 5>> seen_marks;
  for (std::size_t r = 0; r < marks.rows.size(); ++r) {
    ATX_TRY(std::int64_t cohort, intcol(marks, r, "cohort"));
    ATX_TRY(std::string date, str(marks, r, "date"));
    ATX_TRY(std::string role, str(marks, r, "role"));
    ATX_TRY(std::string raw_symbol, str(marks, r, "raw_symbol"));
    ATX_TRY(std::string side, str(marks, r, "side"));
    const std::array<std::string, 5> key{date, std::move(role), std::to_string(cohort),
                                         std::move(raw_symbol), std::move(side)};
    if (!seen_marks.insert(key).second) {
      return recon_fail("duplicate contract mark");
    }
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
    ATX_TRY(std::string date, str(expected, r, "date"));
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
      ATX_TRY(std::string role, str(marks, row, "role"));
      if (role == "Entry") {
        entries.push_back(row);
      } else if (role == "Held") {
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
        ATX_TRY(bool quote_usable, raw_ok(row));
        quote_count += quote_usable ? 1 : 0;
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
        ATX_TRY(bool row_quote_usable, raw_ok(row));
        ATX_TRY(bool prior_quote_usable, raw_ok(prior));
        if (row_quote_usable && prior_quote_usable) {
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
    ATX_TRY(std::string backtest_date, str(rows, r, "date"));
    ATX_TRY(std::string reconciliation_date, str(reconciliation, r, "date"));
    if (backtest_date != reconciliation_date) {
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

// B1 (perf): how many dates share ONE corpus fit fan-out. Read once.
//
// 1 reproduces the historical per-date behaviour exactly (one pool per date).
// The default is a compromise: large enough that the pool always has several
// dates' worth of big boards to overlap across a date boundary, small enough
// that peak live fitted surfaces -- and the work a crash between checkpoint
// commits discards -- stay bounded. Output bytes do NOT depend on this value;
// it is a scheduling knob, which is exactly what makes it safe to tune.
// B1: print the phase split when ATX_VOL_CORPUS_PHASE_TIMING is set to anything
// other than "0". Collection is unconditional and cheap; only the report is gated.
[[nodiscard]] bool corpus_phase_timing_enabled() noexcept {
  static const bool value = []() noexcept -> bool {
#if defined(_MSC_VER)
    char *raw = nullptr;
    std::size_t size = 0;
    if (::_dupenv_s(&raw, &size, "ATX_VOL_CORPUS_PHASE_TIMING") != 0 || raw == nullptr) {
      return false;
    }
    const bool on = raw[0] != '\0' && raw[0] != '0';
    std::free(raw);
    return on;
#else
    const char *raw = std::getenv("ATX_VOL_CORPUS_PHASE_TIMING");
    return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
#endif
  }();
  return value;
}

[[nodiscard]] std::size_t corpus_date_batch_size() noexcept {
  static const std::size_t value = []() noexcept -> std::size_t {
    constexpr std::size_t kDefault = 8u;
#if defined(_MSC_VER)
    char *raw = nullptr;
    std::size_t size = 0;
    if (::_dupenv_s(&raw, &size, "ATX_VOL_CORPUS_DATE_BATCH") != 0 || raw == nullptr) {
      return kDefault;
    }
    const unsigned long parsed = std::strtoul(raw, nullptr, 10);
    std::free(raw);
#else
    const char *raw = std::getenv("ATX_VOL_CORPUS_DATE_BATCH");
    if (raw == nullptr) {
      return kDefault;
    }
    const unsigned long parsed = std::strtoul(raw, nullptr, 10);
#endif
    return parsed == 0u ? kDefault : static_cast<std::size_t>(parsed);
  }();
  return value;
}

} // namespace

Result<CorpusMarketInputTable>
read_corpus_dividend_inputs(const fs::path &path) {
  ATX_TRY(std::string text, read_text(path));
  const std::vector<std::string_view> lines = split(text, '\n');
  if (lines.size() < 2u) {
    return Err(ErrorCode::ParseError, "dividend inputs are missing magic/header");
  }
  const auto clean = [](std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1u);
    }
    return line;
  };
  if (clean(lines[0]) != "ATX_CORPUS_DIVIDENDS\t1" ||
      clean(lines[1]) != "date\tsymbol\tex_ts_ns\tamount\tsource\tas_of") {
    return Err(ErrorCode::ParseError, "dividend inputs magic/header mismatch");
  }

  std::map<std::pair<std::string, std::string>, CorpusMarketInputCell> cells;
  std::set<std::tuple<std::string, std::string, std::int64_t>> observations;
  for (std::size_t i = 2u; i < lines.size(); ++i) {
    const std::string_view line = clean(lines[i]);
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string_view> fields = split(line, '\t');
    std::int64_t ex_ts_ns = 0;
    double amount = 0.0;
    if (fields.size() != 6u || fields[0].empty() || fields[1].empty() ||
        !parse_number(fields[2], ex_ts_ns) || !parse_double(fields[3], amount) ||
        ex_ts_ns <= 0 || amount < 0.0 || fields[4].empty() || fields[5].empty()) {
      return Err(ErrorCode::ParseError, "malformed dividend input row");
    }
    const auto observation =
        std::tuple{std::string(fields[0]), std::string(fields[1]), ex_ts_ns};
    if (!observations.emplace(observation).second) {
      return Err(ErrorCode::AlreadyExists, "duplicate dividend input observation");
    }
    const auto key = std::pair{std::string(fields[0]), std::string(fields[1])};
    auto [it, inserted] = cells.try_emplace(key);
    CorpusMarketInputCell &cell = it->second;
    if (inserted) {
      cell.date = key.first;
      cell.symbol = key.second;
      const std::string as_of_date = cell.date + "T00:00:00Z";
      cell.provenance.spot = {"OPRA put-call parity", as_of_date};
      cell.provenance.rates = {"run_spec.flat_rate", as_of_date};
      cell.provenance.fit_context = {"run_spec.default", as_of_date};
      cell.provenance.dividends = {std::string(fields[4]), std::string(fields[5])};
    } else if (cell.provenance.dividends.source != fields[4] ||
               cell.provenance.dividends.as_of != fields[5]) {
      return Err(ErrorCode::InvalidArgument,
                 "one dividend input cell has inconsistent source/as_of provenance");
    }
    cell.cash_divs.push_back(DividendEvent{ex_ts_ns, amount});
  }
  std::vector<CorpusMarketInputCell> rows;
  rows.reserve(cells.size());
  for (auto &[key, cell] : cells) {
    (void)key;
    rows.push_back(std::move(cell));
  }
  return CorpusMarketInputTable::create(std::move(rows));
}

Status write_share_dividend_artifact(
    const fs::path &path, std::span<const ShareDividendObservation> observations) {
  std::vector<ShareDividendObservation> ordered(observations.begin(), observations.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const ShareDividendObservation &lhs, const ShareDividendObservation &rhs) {
              if (lhs.observed_date != rhs.observed_date) {
                return lhs.observed_date < rhs.observed_date;
              }
              if (lhs.symbol != rhs.symbol) {
                return lhs.symbol < rhs.symbol;
              }
              return lhs.ex_ts_ns != rhs.ex_ts_ns ? lhs.ex_ts_ns < rhs.ex_ts_ns
                                                  : lhs.amount < rhs.amount;
            });
  auto reserved = detail::reserve_unique_publish_temp_file(path.generic_string());
  if (!reserved) {
    return tl::unexpected<atx::core::Error>(std::move(reserved).error());
  }
  const fs::path pending{*reserved};
  {
    std::ofstream out(pending, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::error_code ignored;
      fs::remove(pending, ignored);
      return Err(ErrorCode::IoError, "cannot write share-dividend artifact");
    }
    out << "ATX_SHARE_DIVIDENDS\t1\n"
        << "observed_date\tsymbol\tuid\tex_ts_ns\tamount\tsource\tas_of\t"
           "source_fingerprint\tmarket_input_fingerprint\n"
        << std::setprecision(17);
    std::set<std::tuple<std::string, std::string, std::int64_t>> seen;
    for (const ShareDividendObservation &row : ordered) {
      const auto key = std::tuple{row.observed_date, row.symbol, row.ex_ts_ns};
      if (row.observed_date.empty() || row.symbol.empty() ||
          row.uid != uid_for_symbol(row.symbol) || row.ex_ts_ns <= 0 ||
          !std::isfinite(row.amount) || row.amount < 0.0 || row.source.empty() ||
          row.as_of.empty() || row.source_fingerprint == 0u ||
          row.market_input_fingerprint == 0u || !seen.emplace(key).second) {
        out.close();
        std::error_code ignored;
        fs::remove(pending, ignored);
        return Err(ErrorCode::InvalidArgument,
                   "invalid or duplicate share-dividend observation");
      }
      out << row.observed_date << '\t' << row.symbol << '\t' << row.uid << '\t'
          << row.ex_ts_ns << '\t' << row.amount << '\t' << row.source << '\t'
          << row.as_of << '\t' << row.source_fingerprint << '\t'
          << row.market_input_fingerprint << '\n';
    }
    if (!out) {
      out.close();
      std::error_code ignored;
      fs::remove(pending, ignored);
      return Err(ErrorCode::IoError, "cannot flush share-dividend artifact");
    }
  }
  return detail::flush_and_publish_file(pending.generic_string(), path.generic_string());
}

Result<std::vector<ShareDividend>>
read_share_dividend_artifact(const fs::path &path) {
  ATX_TRY(std::string text, read_text(path));
  const std::vector<std::string_view> lines = split(text, '\n');
  if (lines.size() < 2u) {
    return Err(ErrorCode::ParseError, "share-dividend artifact is missing magic/header");
  }
  const auto clean = [](std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1u);
    }
    return line;
  };
  if (clean(lines[0]) != "ATX_SHARE_DIVIDENDS\t1" ||
      clean(lines[1]) !=
          "observed_date\tsymbol\tuid\tex_ts_ns\tamount\tsource\tas_of\t"
          "source_fingerprint\tmarket_input_fingerprint") {
    return Err(ErrorCode::ParseError, "share-dividend artifact magic/header mismatch");
  }

  std::map<std::pair<std::uint32_t, std::int64_t>, double> events;
  std::set<std::tuple<std::string, std::string, std::int64_t>> observations;
  for (std::size_t i = 2u; i < lines.size(); ++i) {
    const std::string_view line = clean(lines[i]);
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string_view> fields = split(line, '\t');
    std::uint32_t uid = 0u;
    std::int64_t ex_ts_ns = 0;
    double amount = 0.0;
    std::uint64_t source_fingerprint = 0u;
    std::uint64_t market_input_fingerprint = 0u;
    if (fields.size() != 9u || fields[0].empty() || fields[1].empty() ||
        !parse_number(fields[2], uid) || !parse_number(fields[3], ex_ts_ns) ||
        !parse_double(fields[4], amount) || fields[5].empty() || fields[6].empty() ||
        !parse_number(fields[7], source_fingerprint) ||
        !parse_number(fields[8], market_input_fingerprint) || uid == 0u ||
        uid != uid_for_symbol(fields[1]) || ex_ts_ns <= 0 || amount < 0.0 ||
        source_fingerprint == 0u || market_input_fingerprint == 0u) {
      return Err(ErrorCode::ParseError, "malformed share-dividend artifact row");
    }
    const auto observation =
        std::tuple{std::string(fields[0]), std::string(fields[1]), ex_ts_ns};
    if (!observations.emplace(observation).second) {
      return Err(ErrorCode::AlreadyExists,
                 "duplicate share-dividend artifact observation");
    }
    const auto key = std::pair{uid, ex_ts_ns};
    const auto [it, inserted] = events.emplace(key, amount);
    if (!inserted && it->second != amount) {
      return Err(ErrorCode::InvalidArgument,
                 "share-dividend amount changes across corpus observations");
    }
  }
  std::vector<ShareDividend> schedule;
  schedule.reserve(events.size());
  for (const auto &[key, amount] : events) {
    schedule.push_back(ShareDividend{key.first, key.second, amount});
  }
  return Ok(std::move(schedule));
}

// â”€â”€ Public: corpus phase-split line (B1 / T1) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// FIELD LAYOUT â€” this is a contract. The line goes to stdout under
// `ATX_VOL_CORPUS_PHASE_TIMING` and is read by operator scripts OUTSIDE this
// repo, which no change made here can update.
//
//   1  PHASE            record tag (the only field that is not name=value)
//   2  ingest_s         wall in the OPRA ingest phase
//   3  build_s          wall in the build phase
//   4  fit_fanout_s     build-phase wall inside the fit fan-out
//   5  archive_write_s  build-phase wall writing surface archives
//   6  checkpoint_s     build-phase wall writing checkpoints
//   7  other_s          build_s minus the three named phases above
//   8  fanout_calls     fan-out invocations
//   9  boards           boards fitted
//  10  date_batch       dates per ingest batch (ATX_VOL_CORPUS_DATE_BATCH)
//  11  reclaimed        T1/T-I4: distinct BOARDS that picked up inner workers a
//                       draining pool could no longer place
//  12  inner_slots      T1/T-I4: raw SUM of every inner budget offered â€” many
//                       per board, NOT a per-board mean. Without 11 and 12 the
//                       phase-timing probe cannot report whether the reclaim
//                       fired at all.
//
// TWO RULES, both gated by `CorpusPhaseLine.
// FieldLayoutIsAppendOnlyAndEveryFieldIsSelfDescribing`:
//
//   * NEW FIELDS APPEND. `9cfcbc3` inserted fields 11 and 12 BETWEEN `boards`
//     and `date_batch`, pushing `date_batch` from field 10 to field 12 and
//     shifting every positional reader â€” for no gain, since the counters read
//     no better beside `boards` than at the end. Field 10 is restored here
//     [MINORS M9], and appending is the rule that keeps it stable.
//   * EVERY FIELD IS `name=value`. Keying by NAME is always available and is
//     what a consumer should prefer; the stable position is a courtesy for
//     `awk '{print $10}'`. The gate keeps both true at once, so a future field
//     can neither be inserted nor emitted as a bare positional value.
std::string format_corpus_phase_line(double ingest_s, double build_s,
                                     const CorpusPhaseTimings &phases, std::size_t date_batch,
                                     double ingest_process_cpu_s) {
  const double named = phases.fit_fanout_s + phases.archive_write_s + phases.checkpoint_s;
  char buf[512];
  const int written =
      std::snprintf(buf, sizeof buf,
                    "PHASE ingest_s=%.2f build_s=%.2f fit_fanout_s=%.2f archive_write_s=%.2f "
                    "checkpoint_s=%.2f other_s=%.2f fanout_calls=%llu boards=%llu "
                    "date_batch=%zu reclaimed=%llu inner_slots=%llu "
                    "fit_fanout_cpu_s=%.2f ingest_cpu_s=%.2f",
                    ingest_s, build_s, phases.fit_fanout_s, phases.archive_write_s,
                    phases.checkpoint_s, build_s - named,
                    static_cast<unsigned long long>(phases.fanout_calls),
                    static_cast<unsigned long long>(phases.boards_fitted), date_batch,
                    static_cast<unsigned long long>(phases.reclaimed_inner_boards),
                    static_cast<unsigned long long>(phases.inner_worker_slots),
                    phases.fit_fanout_process_cpu_s, ingest_process_cpu_s);
  // rev2-ws-t N-M2: `written` is snprintf's UNTRUNCATED length, so it can exceed
  // `sizeof buf`. Sizing the std::string from it read past the end of the buffer
  // whenever the line truncated (measured: 1073 bytes returned from a 512-byte
  // buffer). Build from the NUL terminator instead â€” identical in the
  // non-truncating case, which is every realistic one (~270 chars).
  return written > 0 ? std::string(buf) : std::string{};
}

// â”€â”€ Public: fingerprints + corpus config â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

// â”€â”€ Public: surface-only backtest compute seam â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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
// "fraction of peak NAV" is degenerate here â€” peak NAV is 0 on a losing run and
// the ratio is meaningless. A capital base is also what a real risk system
// stops on ("halt after losing 20% of capital"), so `read_dispersion_run_config`
// requires `limit_capital` whenever `limit_drawdown_stop` is set.
//
// Enforcing it needs exactly ONE replay, not a fixed point. Halting only ever
// suppresses entries at or AFTER the breach step, and NAV up to that step is a
// function of trades made strictly before it â€” so the first breach index is
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

// â”€â”€ Public: X1 strict typed run config â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

// â”€â”€ X5: friction/impact regime â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

std::string_view to_string(DispersionFrictionRegime regime) noexcept {
  switch (regime) {
  case DispersionFrictionRegime::Frictionless:
    return "frictionless";
  case DispersionFrictionRegime::Frictioned:
    return "frictioned";
  case DispersionFrictionRegime::FrictionedWithImpact:
    return "frictioned+impact";
  }
  return "unknown";
}

namespace {

// True when the model actually charges something. `spread_kind == None` with a
// nonzero half_spread is still frictionless â€” the engine reads the kind â€” so the
// classification follows the KIND, not the bare parameter, and cannot overstate.
[[nodiscard]] bool frictions_active(const FrictionModel &f) noexcept {
  const bool spread = f.spread_kind == FrictionModel::SpreadKind::PriceBps
                          ? f.half_spread_bps != 0.0
                          : (f.spread_kind == FrictionModel::SpreadKind::VolTicks
                                 ? f.vol_tick != 0.0
                                 : false);
  return spread || f.per_contract_cost != 0.0 || f.hedge_slippage_bps != 0.0;
}

} // namespace

DispersionFrictionRegime dispersion_friction_regime(const DispersionRunConfig &config) noexcept {
  if (config.costs.active()) {
    return DispersionFrictionRegime::FrictionedWithImpact;
  }
  return frictions_active(config.frictions) ? DispersionFrictionRegime::Frictioned
                                            : DispersionFrictionRegime::Frictionless;
}

std::string dispersion_regime_detail(const FrictionModel &frictions,
                                     const DispersionCostModel &costs) {
  const auto number = [](double value) {
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.10g", value);
    return std::string(buffer.data(), written > 0 ? static_cast<std::size_t>(written) : 0u);
  };
  std::vector<std::string> parts;
  switch (frictions.spread_kind) {
  case FrictionModel::SpreadKind::None:
    break;
  case FrictionModel::SpreadKind::PriceBps:
    parts.push_back(number(frictions.half_spread_bps) + " bps half-spread");
    break;
  case FrictionModel::SpreadKind::VolTicks:
    parts.push_back(number(frictions.vol_tick) + " vol-tick half-spread");
    break;
  // B1 (backtest-lakehouse sprint): the dispersion route does not offer
  // QuoteSide (no `fill_policy`/CLI mapping selects it -- see
  // `dispersion_engine_run_config_from`), so this case exists only to keep
  // the switch exhaustive for the shared `FrictionModel::SpreadKind` enum;
  // it is unreachable from any dispersion config path today.
  case FrictionModel::SpreadKind::QuoteSide:
    parts.push_back("quote-side crossing (engine-level; not a dispersion-route knob)");
    break;
  }
  if (frictions.per_contract_cost != 0.0) {
    parts.push_back("$" + number(frictions.per_contract_cost) + "/contract");
  }
  if (frictions.hedge_slippage_bps != 0.0) {
    parts.push_back(number(frictions.hedge_slippage_bps) + " bps hedge slippage");
  }
  if (costs.active()) {
    parts.push_back("sqrt-impact k=" + number(costs.k) + " beta=" + number(costs.beta) +
                    " participation=" + number(costs.adv_fraction));
  }
  if (parts.empty()) {
    return "mid fills, no commission, no impact";
  }
  std::string detail;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    detail += (i == 0 ? "" : ", ");
    detail += parts[i];
  }
  return detail;
}

std::string_view to_string(DispersionBenchmarkJoin join) noexcept {
  switch (join) {
  case DispersionBenchmarkJoin::ExactDates:
    return "exact_dates";
  case DispersionBenchmarkJoin::InnerJoinOnDates:
    return "inner_join_on_dates";
  }
  return "unknown";
}

Result<std::vector<DispersionBenchmarkRow>>
read_dispersion_benchmark_series(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::NotFound, "cannot open benchmark series " + path.string());
  }
  std::vector<DispersionBenchmarkRow> series;
  std::string line;
  std::size_t row = 0;
  while (std::getline(stream, line)) {
    ++row;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      return Err(ErrorCode::ParseError, "benchmark series row " + std::to_string(row) +
                                            " is not date<TAB>pnl: '" + line + "'");
    }
    const std::string value = line.substr(tab + 1);
    double parsed = 0.0;
    const char *first = value.data();
    const char *last = first + value.size();
    const std::from_chars_result result =
        std::from_chars(first, last, parsed, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != last) {
      // A first unparseable row is a header; anywhere else it is a malformed file.
      // A benchmark that silently half-loads would corrupt every statistic
      // derived from it, so this is an error rather than a skip.
      if (row == 1 && series.empty()) {
        continue;
      }
      return Err(ErrorCode::ParseError, "benchmark series row " + std::to_string(row) +
                                            " has a non-numeric value: '" + value + "'");
    }
    // REVIEW C-6. The date is the JOIN KEY, not decoration â€” it used to be parsed
    // and thrown away, which is what let a misaligned file produce confident
    // numbers for the wrong observations.
    std::string date = line.substr(0, tab);
    if (date.empty()) {
      return Err(ErrorCode::ParseError,
                 "benchmark series row " + std::to_string(row) + " has an empty date");
    }
    if (!std::isfinite(parsed)) {
      return Err(ErrorCode::ParseError, "benchmark series row " + std::to_string(row) + " (" +
                                            date + ") has a non-finite value: '" + value + "'");
    }
    if (!series.empty() && date <= series.back().date) {
      // Catches duplicates, reversed files and unordered files in one check. A
      // benchmark whose dates do not advance cannot be joined, and the failure
      // must land HERE rather than as a plausible misalignment downstream.
      return Err(ErrorCode::ParseError,
                 "benchmark series row " + std::to_string(row) + " date '" + date +
                     "' does not advance on the previous row's '" + series.back().date +
                     "' â€” benchmark dates must be unique and ascending");
    }
    series.push_back(DispersionBenchmarkRow{std::move(date), parsed});
  }
  return Ok(std::move(series));
}

Result<DispersionBenchmarkPairing>
pair_dispersion_benchmark(std::span<const std::string> strategy_dates,
                          std::span<const double> strategy_pnl,
                          std::span<const DispersionBenchmarkRow> benchmark,
                          DispersionBenchmarkJoin policy) {
  if (strategy_dates.size() != strategy_pnl.size()) {
    return Err(ErrorCode::InvalidArgument,
               "benchmark join: " + std::to_string(strategy_dates.size()) +
                   " strategy dates against " + std::to_string(strategy_pnl.size()) +
                   " strategy observations");
  }
  for (std::size_t i = 1; i < strategy_dates.size(); ++i) {
    if (strategy_dates[i] <= strategy_dates[i - 1]) {
      return Err(ErrorCode::InvalidArgument,
                 "benchmark join: strategy date '" + strategy_dates[i] +
                     "' does not advance on '" + strategy_dates[i - 1] + "'");
    }
  }

  DispersionBenchmarkPairing out;
  if (policy == DispersionBenchmarkJoin::ExactDates) {
    if (benchmark.size() != strategy_dates.size()) {
      return Err(ErrorCode::InvalidArgument,
                 "benchmark join (exact_dates): the series covers " +
                     std::to_string(benchmark.size()) + " sessions but the strategy has " +
                     std::to_string(strategy_dates.size()) +
                     " return observations. Supply a benchmark over exactly the strategy's "
                     "dates, or set benchmark_join=inner to compare over the intersection.");
    }
    for (std::size_t i = 0; i < strategy_dates.size(); ++i) {
      if (benchmark[i].date != strategy_dates[i]) {
        return Err(ErrorCode::InvalidArgument,
                   "benchmark join (exact_dates): observation " + std::to_string(i) +
                       " is strategy date '" + strategy_dates[i] + "' but benchmark date '" +
                       benchmark[i].date +
                       "'. The two series describe different sessions; set "
                       "benchmark_join=inner to compare over the intersection instead.");
      }
    }
    out.strategy.assign(strategy_pnl.begin(), strategy_pnl.end());
    out.benchmark.reserve(benchmark.size());
    for (const DispersionBenchmarkRow &r : benchmark) {
      out.benchmark.push_back(r.pnl);
    }
  } else {
    // Both sides are strictly ascending (the reader and the check above enforce
    // it), so the intersection is one linear merge.
    std::size_t b = 0;
    for (std::size_t s = 0; s < strategy_dates.size(); ++s) {
      while (b < benchmark.size() && benchmark[b].date < strategy_dates[s]) {
        ++b;
      }
      if (b < benchmark.size() && benchmark[b].date == strategy_dates[s]) {
        out.strategy.push_back(strategy_pnl[s]);
        out.benchmark.push_back(benchmark[b].pnl);
        ++b;
      } else {
        ++out.n_unmatched;
      }
    }
  }

  if (out.strategy.size() < 2u) {
    return Err(ErrorCode::InvalidArgument,
               "benchmark join (" + std::string(to_string(policy)) + "): only " +
                   std::to_string(out.strategy.size()) +
                   " paired observation(s); every benchmark-relative ratio needs a sample "
                   "variance, so this is not a benchmark comparison");
  }
  return Ok(std::move(out));
}

Result<TearSheet> dispersion_tearsheet_with_benchmark(const BacktestResult &track,
                                                      const DispersionRunConfig &config) {
  TearSheet sheet = tearsheet(track, config.periods_per_year);
  if (config.benchmark_series.empty()) {
    return Ok(std::move(sheet)); // absent => absolute statistics only, nothing claimed
  }
  ATX_TRY(std::vector<DispersionBenchmarkRow> rows,
          read_dispersion_benchmark_series(config.benchmark_series));
  ATX_TRY(std::vector<std::string> dates, backtest_return_dates(track));
  const std::vector<double> returns = backtest_return_series(track);
  ATX_TRY(DispersionBenchmarkPairing paired,
          pair_dispersion_benchmark(dates, returns, rows, config.benchmark_join));
  sheet.benchmark =
      benchmark_stats(paired.strategy, paired.benchmark, config.periods_per_year);
  return Ok(std::move(sheet));
}

FrictionModel dispersion_friction_preset(DispersionFrictionPreset preset) {
  FrictionModel model;
  switch (preset) {
  case DispersionFrictionPreset::None:
    return model; // frictionless mid â€” exactly the pinned golden
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
  ATX_TRY_VOID(binder.path_key("dividend_ledger", config.dividend_ledger));

  ATX_TRY_VOID(binder.text("index_symbol", config.universe.index_symbol));
  ATX_TRY_VOID(binder.number("min_names", config.universe.min_names));
  ATX_TRY_VOID(binder.number("min_weight_coverage", config.universe.min_weight_coverage));

  ATX_TRY_VOID(binder.number("flat_rate", config.rate.flat_rate));
  ATX_TRY_VOID(binder.boolean("rate_applies_to_financing", config.rate.apply_to_financing));

  ATX_TRY_VOID(binder.enumerated("side", config.side,
                                 {{"short_index_long_names", DispersionSide::ShortIndexLongNames},
                                  {"long_index_short_names", DispersionSide::LongIndexShortNames}}));
  // X4 policies. Every spelling below maps to a scheme the sizing path really
  // implements; a knob that silently did nothing would be worse than no knob.
  ATX_TRY_VOID(binder.enumerated("weighting", config.weighting,
                                 {{"vega_neutral", WeightingScheme::VegaNeutral},
                                  {"equal_vega", WeightingScheme::EqualVega},
                                  {"gamma_neutral", WeightingScheme::GammaNeutral},
                                  {"theta_neutral", WeightingScheme::ThetaNeutral}}));
  ATX_TRY_VOID(binder.enumerated("strike", config.strike.rule,
                                 {{"atm_forward_straddle", StrikeRule::AtmForwardStraddle},
                                  {"fixed_moneyness", StrikeRule::FixedMoneyness},
                                  {"delta_strangle", StrikeRule::DeltaStrangle}}));
  // PRESENCE, not value. A strike parameter belonging to a rule that ignores it
  // must be REJECTED, and that has to key off whether the SPEC NAMED the key:
  // testing the parsed value against its default cannot distinguish "explicitly
  // set to the default" from "absent", so `strike_abs_delta = 0.25` under the
  // default rule would sail through as exactly the inert knob this seam exists
  // to prevent. `find` marks the key consumed; the `number` call still parses it.
  const bool strike_log_moneyness_named = binder.find("strike_log_moneyness") != nullptr;
  const bool strike_abs_delta_named = binder.find("strike_abs_delta") != nullptr;
  ATX_TRY_VOID(binder.number("strike_log_moneyness", config.strike.log_moneyness));
  ATX_TRY_VOID(binder.number("strike_abs_delta", config.strike.target_abs_delta));

  // X5 reporting.
  ATX_TRY_VOID(binder.path_key("benchmark_series", config.benchmark_series));
  // C-6. A NAMED join policy, not a bool and not a default that silently restores
  // positional alignment: `exact` demands the two series describe the same
  // sessions, `inner` opts into a comparison over their intersection.
  ATX_TRY_VOID(binder.enumerated("benchmark_join", config.benchmark_join,
                                 {{"exact", DispersionBenchmarkJoin::ExactDates},
                                  {"inner", DispersionBenchmarkJoin::InnerJoinOnDates}}));
  ATX_TRY_VOID(binder.number("periods_per_year", config.periods_per_year));

  ATX_TRY_VOID(binder.number("target_dte_days", config.dte.target_days));
  ATX_TRY_VOID(binder.number("min_dte_days", config.dte.min_days));
  ATX_TRY_VOID(binder.number("max_dte_days", config.dte.max_days));
  ATX_TRY_VOID(binder.number("roll_dte_days", config.roll_dte_days));
  ATX_TRY_VOID(binder.number("gross_index_vega", config.gross_index_vega));
  ATX_TRY_VOID(binder.number("multiplier", config.multiplier));
  ATX_TRY_VOID(binder.number("entry_every_n", config.entry_every_n));
  ATX_TRY_VOID(binder.boolean("record_diagnostics", config.record_diagnostics));
  // S3-T16: the loose result TSVs are a diagnostic, off unless the spec asks.
  ATX_TRY_VOID(binder.boolean("emit_tsv_diagnostics", config.emit_tsv_diagnostics));

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

  // â”€â”€ WS-F F4 (BT-W): the listed-route execution knobs â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
  ATX_TRY_VOID(binder.enumerated("unpriced", config.unpriced,
                                 {{"error", UnpricedLotPolicy::Error},
                                  {"exclude", UnpricedLotPolicy::ExcludeAndReport}}));
  ATX_TRY_VOID(binder.enumerated("fill_policy", config.fill_policy,
                                 {{"model_mark", ScheduleFillPolicy::ModelMark},
                                  {"quote_mid", ScheduleFillPolicy::QuoteMid},
                                  {"cross_spread", ScheduleFillPolicy::CrossSpread}}));
  ATX_TRY_VOID(
      binder.boolean("book_entry_fill_slippage", config.book_entry_fill_slippage));
  ATX_TRY_VOID(binder.boolean("reconcile_nav", config.reconcile_nav));

  // â”€â”€ WS-F F6 (BT-P2-8): quote-quality admission, consumed by build-schedule â”€
  ATX_TRY_VOID(binder.number("quote_min_bid", config.quote_quality.min_bid));
  ATX_TRY_VOID(binder.number("quote_max_age_ns", config.quote_quality.max_quote_age_ns));
  ATX_TRY_VOID(binder.boolean("quote_reject_locked", config.quote_quality.reject_locked));

  // Strictness: everything not bound above is rejected, by name.
  ATX_TRY_VOID(binder.reject_unknown());
  if (!config.dividend_ledger.empty()) {
    ATX_TRY(config.financing.share_dividends,
            read_share_dividend_artifact(config.dividend_ledger));
  }

  // Contract validation â€” the invariants read_run_spec enforced, plus the ones
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
  if (config.quote_quality.min_bid < 0.0 || config.quote_quality.max_quote_age_ns < 0) {
    return Err(ErrorCode::InvalidArgument,
               "quote_min_bid must be nonnegative and quote_max_age_ns must be >= 0 (0 = off)");
  }
  if (config.fill_policy != ScheduleFillPolicy::ModelMark && !config.book_entry_fill_slippage) {
    // A quote-side fill that the engine does not charge is INVISIBLE in NAV
    // (BT-P1-1 / F2): NAV sums mark-to-mark moves, and the first move is
    // measured from the entry date's mark, not from what was paid. Accepting
    // this combination would ship a knob that silently does nothing.
    return Err(ErrorCode::InvalidArgument,
               "fill_policy other than model_mark requires book_entry_fill_slippage=1, "
               "otherwise the fill/mark difference never reaches NAV");
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
  // X4 strike contract. Each rule validates only the parameter it reads, and a
  // parameter set for a rule that ignores it is refused â€” otherwise a spec could
  // carry `strike_abs_delta=0.4` under the default rule and quietly do nothing.
  if (config.strike.rule == StrikeRule::DeltaStrangle &&
      (!(config.strike.target_abs_delta > 0.0) || !(config.strike.target_abs_delta < 1.0))) {
    return Err(ErrorCode::InvalidArgument, "strike_abs_delta must lie in (0, 1)");
  }
  if (config.strike.rule != StrikeRule::FixedMoneyness && strike_log_moneyness_named) {
    return Err(ErrorCode::InvalidArgument,
               "strike_log_moneyness applies only to strike=fixed_moneyness");
  }
  if (config.strike.rule != StrikeRule::DeltaStrangle && strike_abs_delta_named) {
    return Err(ErrorCode::InvalidArgument,
               "strike_abs_delta applies only to strike=delta_strangle");
  }
  if (config.strike.rule == StrikeRule::FixedMoneyness &&
      !(std::fabs(config.strike.log_moneyness) < 5.0)) {
    return Err(ErrorCode::InvalidArgument, "strike_log_moneyness is implausibly large");
  }
  if (!(config.periods_per_year > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "periods_per_year must be positive");
  }
  return Ok(std::move(config));
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
  backtest.weighting = config.weighting; // X4
  backtest.strike = config.strike;       // X4
  // X2/X6: the wiring that never existed. The dispersion path built a RunConfig
  // that left `frictions` and `financing` default-constructed, so every fill was a
  // frictionless mid and no carry ever accrued, regardless of the run spec.
  //
  // REV-TAIL I-3. This block used to hand-build `backtest.run`: it set frictions
  // and financing (X2/X6) but hardcoded `run.unpriced = UnpricedLotPolicy::Error`,
  // ignoring `config.unpriced`, and never set `surface_provenance_policy`,
  // `book_entry_fill_slippage` or `reconcile_nav` at all. Because
  // `dispersion_run_surface_backtest` reads the STRICT typed config, all four of
  // those keys bound by name and survived `reject_unknown()` â€” so the shipped
  // `run-surface-backtest` accepted four spec keys by name and gave them no effect.
  //
  // Deferring to `dispersion_engine_run_config_from` makes the "single place"
  // claim on `dispersion_engine_run_config_from` (dispersion_run.hpp:323-325)
  // literally true â€” there is now exactly one
  // construction of the engine RunConfig, and a knob visible there is reachable
  // from BOTH routes. It cannot move a golden: every one of the four defaults to
  // precisely the value this block hardcoded or inherited (`unpriced` Error,
  // `provenance` Compatibility, the two flags false), so a spec that does not
  // mention them yields a byte-identical config. `DispersionBacktestConfigFrom.
  // DefaultSpecKeepsTheShippedEngineDefaults` pins that, and it was green BEFORE
  // this change as well as after.
  backtest.run = dispersion_engine_run_config_from(config);
  return backtest;
}

RunConfig dispersion_engine_run_config_from(const DispersionRunConfig &config) {
  // WS-F F4 (BT-W). The listed replay used to construct `RunConfig config;
  // config.unpriced = Error;` and nothing else â€” so `friction_*`,
  // `financing_*`, `cost_*` and `provenance` in the spec were accepted, echoed,
  // and then had no effect whatsoever on the headline artifact. Every field the
  // engine honours is now assigned from the typed spec HERE, in one place, so a
  // knob is either visible in this function or provably dead.
  RunConfig run;
  run.unpriced = config.unpriced;
  run.frictions = dispersion_effective_frictions(config.frictions, config.costs);
  run.financing = config.financing;
  if (config.rate.apply_to_financing) {
    // Task E1 (backtest-lakehouse sprint), fixing a latent gap A5 tracked
    // (progress.md: "dispersion_run.cpp:1861-1864 sets finance_premium=true
    // with no reference_uid/flat_r"). This used to write `config.rate.
    // flat_rate` into `run.financing.borrow_rate` -- the wrong field
    // (`borrow_rate` prices the SHORT-shares borrow proxy; it has nothing to
    // do with `finance_premium`'s cash-carry rate, and doing so silently
    // clobbered whatever `financing_borrow_rate` spec key the caller had
    // already set) -- while `finance_premium`'s own rate source
    // (`FinancingConfig::flat_r` / `reference_uid`, see backtest.cpp's
    // finance_premium block) was left unset entirely. `DispersionRateSource`'s
    // own doc comment (dispersion_run.hpp) already states the intent: "Setting
    // apply_to_financing routes the same [flat_rate] rate into FinancingConfig
    // as well." Routing it into `flat_r` is that fix: it both makes the flat
    // discount rate the ACTUAL cash-carry accrual rate (previously it accrued
    // at the base snapshot's own single-name surface `r`, or hard-errored on a
    // multi-name corpus with no reference set -- the fail-closed path A5's
    // sprint added) and stops clobbering `borrow_rate`.
    run.financing.flat_r = config.rate.flat_rate;
    run.financing.finance_premium = true;
  }
  run.surface_provenance_policy = config.provenance;
  run.book_entry_fill_slippage = config.book_entry_fill_slippage;
  run.reconcile_nav = config.reconcile_nav;
  return run;
}

RunConfig make_listed_replay_run_config(const DispersionRunConfig &config, const Clock &clock,
                                        const ListedDispersionStrategy &strategy) {
  RunConfig run = dispersion_engine_run_config_from(config);
  // WS-F F5 (BT-T2), review follow-up. The listed replay supplies its
  // OWN cache â€” it shares one across the replay and the reconciliation pass â€”
  // and the engine deliberately never subsets a SUPPLIED cache, because it
  // cannot know what else the caller will serve from it. So F5 was inert on
  // exactly the path whose premise motivated it: the listed `run-backtest`
  // still loaded the whole board every date.
  //
  // The caller DOES know. Both consumers of this cache touch exactly the
  // schedule's uids â€” the replay through `ListedDispersionStrategy`, and
  // `reconcile_listed_dispersion`, which resolves nothing but `leg.uid`
  // (listed_dispersion_reconciliation.cpp:147) â€” so the cache is subsetted at
  // construction. `uid_of` is unaffected: `MarketSnapshot::load` builds the
  // symbol table from the WHOLE archive directory even under a subset.
  //
  // Capacity is the full clock, not the private cache's 3, because the
  // reconciliation pass below holds every date's snapshot alive at once; a
  // bounded cache would evict and re-load them.
  const std::span<const std::uint32_t> replay_uids = strategy.referenced_uids();
  run.snapshot_cache = std::make_shared<SnapshotCache>(
      clock.size() > 0u ? clock.size() : 1u,
      std::vector<std::uint32_t>(replay_uids.begin(), replay_uids.end()));
  return run;
}

ListedScheduleSpec listed_schedule_spec_from(const RunSpec &spec,
                                             const DispersionRunConfig &config) {
  // REV-MTIDY I-1. A verbatim lift of the nine assignments `build_schedule_
  // command` made inline. Everything not assigned keeps `ListedScheduleSpec`'s
  // own default, so a spec that names nothing reproduces the pre-lift spec field
  // for field.
  ListedScheduleSpec sched;
  sched.target_dte_days = spec.target_dte_days;
  sched.min_dte_days = spec.min_dte_days;
  sched.max_dte_days = spec.max_dte_days;
  sched.roll_dte_days = spec.roll_dte_days;
  sched.min_names = spec.min_names;
  sched.min_weight_coverage = spec.min_weight_coverage;
  sched.gross_index_vega = spec.gross_index_vega;
  sched.core_mode = spec.core_mode;
  // The line the whole quote-knob fix rests on, and the only one of the nine the
  // loose `RunSpec` cannot supply. Delete it and `DispersionScheduleSpecFrom.
  // EveryDeclaredScheduleKnobReachesTheSelectionPolicy` goes red; before this
  // function existed, deleting its predecessor turned nothing red at all.
  sched.quality = config.quote_quality;
  return sched;
}

Status persist_typed_spec_keys(const fs::path &source_spec, const fs::path &run_spec) {
  ATX_TRY(KvMap source_values, read_kv_tsv(source_spec));
  // Exactly the vocabulary `write_resolved_spec` emits. Anything else in the
  // source spec belongs to the typed config and would otherwise be erased.
  static constexpr std::string_view kRunSpecKeys[] = {
      "label",           "date_lo",       "date_hi",           "snapshot_suffix",
      "opra_root",       "path_template", "universe_schedule", "definitions",
      "occ_ess_root",    "dividend_inputs", "dividend_ledger", "flat_rate",
      "min_names",       "min_weight_coverage",
      "target_dte_days", "min_dte_days",  "max_dte_days",      "roll_dte_days",
      "gross_index_vega","delta_band",    "fit_workers",       "core_mode"};
  std::ofstream out(run_spec, std::ios::binary | std::ios::app);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot append typed run config keys to " + run_spec.string());
  }
  const fs::path source_base = source_spec.parent_path();
  for (const auto &[key, value] : source_values) {
    bool is_run_spec_key = false;
    for (const std::string_view known : kRunSpecKeys) {
      if (key == known) {
        is_run_spec_key = true;
        break;
      }
    }
    if (is_run_spec_key) {
      continue;
    }
    // `benchmark_series` is the one path-valued extra, and the typed reader
    // resolves relative paths against the spec's OWN directory â€” so carry it
    // across absolute, or the run dir would resolve it somewhere else.
    if (key == "benchmark_series" && !value.empty() && !fs::path{value}.is_absolute()) {
      out << key << '\t' << (source_base / value).lexically_normal().string() << '\n';
      continue;
    }
    out << key << '\t' << value << '\n';
  }
  return out ? Ok() : Err(ErrorCode::IoError, "cannot flush typed run config keys");
}

Status write_quote_reject_report(const fs::path &path, std::span<const QuoteRejectRow> rows) {
  auto reserved = detail::reserve_unique_publish_temp_file(path.generic_string());
  if (!reserved) {
    return tl::unexpected<atx::core::Error>(std::move(reserved).error());
  }
  const fs::path pending{*reserved};
  {
    std::ofstream out(pending, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::error_code ignored;
      fs::remove(pending, ignored);
      return Err(ErrorCode::IoError, "cannot write " + path.string());
    }
    // FIX-F m5: a version line, so a positional reader written against an older
    // column order fails loudly instead of silently shifting. Same `#`-metadata
    // convention `write_backtest_pnl_tsv` uses.
    out << "# schema=quote_rejects/1\n";
    out << "date\tselection\tnot_two_sided\tzero_bid\tstale\tstale_unevaluable\tlocked\t"
           "locked_dropped\tnon_standard\ttotal_dropped\n";
    for (const QuoteRejectRow &row : rows) {
      const ListedQuoteRejectCounts &counts = row.counts;
      out << row.date << '\t' << (row.selected ? "ok" : "no_basket") << '\t'
          << counts.not_two_sided << '\t' << counts.zero_bid << '\t' << counts.stale
          << '\t' << counts.stale_unevaluable << '\t' << counts.locked << '\t'
          << counts.locked_dropped << '\t' << counts.non_standard << '\t'
          << counts.total_dropped() << '\n';
    }
    if (!out) {
      out.close();
      std::error_code ignored;
      fs::remove(pending, ignored);
      return Err(ErrorCode::IoError, "cannot flush " + path.string());
    }
  }
  return detail::flush_and_publish_file(pending.generic_string(), path.generic_string());
}

Status write_dispersion_effective_config(const fs::path &path, const DispersionRunConfig &config) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot write effective run config " + path.string());
  }
  const RunConfig engine = dispersion_engine_run_config_from(config);
  const auto num = [](double value) {
    std::array<char, 40> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
    return std::string(buffer.data(), written > 0 ? static_cast<std::size_t>(written) : 0u);
  };
  const auto spread_kind = [](FrictionModel::SpreadKind kind) -> const char * {
    switch (kind) {
    case FrictionModel::SpreadKind::None:
      return "none";
    case FrictionModel::SpreadKind::PriceBps:
      return "price_bps";
    case FrictionModel::SpreadKind::VolTicks:
      return "vol_ticks";
    // B1: unreachable from any dispersion config path (see the identical note
    // on `dispersion_regime_detail` above) -- present only to keep this
    // switch exhaustive for the shared enum.
    case FrictionModel::SpreadKind::QuoteSide:
      return "quote_side";
    }
    return "none";
  };
  const auto fill_policy = [](ScheduleFillPolicy p) -> const char * {
    switch (p) {
    case ScheduleFillPolicy::ModelMark:
      return "model_mark";
    case ScheduleFillPolicy::QuoteMid:
      return "quote_mid";
    case ScheduleFillPolicy::CrossSpread:
      return "cross_spread";
    }
    return "model_mark";
  };
  out << "key\tvalue\n"
      // REGIME FIRST (M4): the first two rows say which execution assumptions
      // produced every number in this run directory.
      << "friction_regime\t" << to_string(dispersion_friction_regime(config)) << '\n'
      // E1 fix round: D1's kBacktestEconomicsRev names WHICH revision of the
      // engine's economics interpretation produced every number below --
      // adjacent to friction_regime so a reader never has to open a second
      // artifact to know both "what assumptions" and "which engine build".
      << "economics_rev\t" << kBacktestEconomicsRev << '\n'
      << "friction_regime_detail\t"
      << dispersion_regime_detail(engine.frictions, config.costs) << '\n'
      << "friction_spread_kind\t" << spread_kind(engine.frictions.spread_kind) << '\n'
      << "friction_half_spread_bps\t" << num(engine.frictions.half_spread_bps) << '\n'
      << "friction_vol_tick\t" << num(engine.frictions.vol_tick) << '\n'
      << "friction_per_contract_cost\t" << num(engine.frictions.per_contract_cost) << '\n'
      << "friction_hedge_slippage_bps\t" << num(engine.frictions.hedge_slippage_bps) << '\n'
      << "cost_impact_k\t" << num(config.costs.k) << '\n'
      << "cost_impact_beta\t" << num(config.costs.beta) << '\n'
      << "cost_adv_fraction\t" << num(config.costs.adv_fraction) << '\n'
      << "financing_borrow_rate\t" << num(engine.financing.borrow_rate) << '\n'
      << "financing_finance_premium\t" << (engine.financing.finance_premium ? 1 : 0) << '\n'
      << "financing_shares_carry\t" << (engine.financing.shares_carry ? 1 : 0) << '\n'
      << "financing_initial_cash\t" << num(engine.financing.initial_cash) << '\n'
      << "provenance\t"
      << (engine.surface_provenance_policy == SurfaceProvenancePolicy::RequireAdmittedRisk
              ? "require_admitted_risk"
              : "compatibility")
      << '\n'
      << "unpriced\t" << (engine.unpriced == UnpricedLotPolicy::Error ? "error" : "exclude") << '\n'
      << "fill_policy\t" << fill_policy(config.fill_policy) << '\n'
      << "book_entry_fill_slippage\t" << (engine.book_entry_fill_slippage ? 1 : 0) << '\n'
      << "reconcile_nav\t" << (engine.reconcile_nav ? 1 : 0) << '\n'
      << "quote_min_bid\t" << num(config.quote_quality.min_bid) << '\n'
      << "quote_max_age_ns\t" << config.quote_quality.max_quote_age_ns << '\n'
      << "quote_reject_locked\t" << (config.quote_quality.reject_locked ? 1 : 0) << '\n'
      // C-6: the benchmark join policy is an assumption behind every published
      // alpha/beta/IR, so it belongs in the effective config beside the regime.
      << "benchmark_join\t" << to_string(config.benchmark_join) << '\n'
      << "delta_band\t" << num(config.hedge.band) << '\n'
      << "gross_index_vega\t" << num(config.gross_index_vega) << '\n'
      // S3-T16/S3-T17. The declared value of the loose-diagnostics knob, so a
      // reader holding this file knows whether the run beside it also left the
      // loose result tables behind. (It reads BOTH values again: S3-T16 wrote
      // this row under a gate that could only ever emit 1, but the sole
      // remaining caller of this writer is the shipped `run_backtest_command`,
      // which publishes `run_config.tsv` unconditionally.)
      << "emit_tsv_diagnostics\t" << (config.emit_tsv_diagnostics ? 1 : 0) << '\n';
  return out ? Ok() : Err(ErrorCode::IoError, "cannot flush effective run config");
}

namespace {

constexpr std::string_view kProjectedVarHeader =
    "confidence\treference_value\tvalue_at_risk\texpected_shortfall\tn_scenarios\t"
    "n_positions\tprojections_per_second\tprepared_fingerprint\tas_of_date\tas_of_ts_ns\t"
    "book_fingerprint";
constexpr std::string_view kProjectedScenarioHeader =
    "date\tts_ns\tvalue\tdelta\tgamma\tvega\ttheta\tn_ok\tn_failed\tdefinition_fingerprint";
constexpr std::string_view kProjectedLegHeader =
    "date\tleg\tuid\tside\texpiry_ts_ns\tstrike\tquantity\tmultiplier\tmark\t"
    "delta\tgamma\tvega\ttheta\tdefinition_fingerprint\tstatus";

struct ProjectedVarSummaryRow {
  double confidence{0.0};
  double reference_value{0.0};
  double value_at_risk{0.0};
  double expected_shortfall{0.0};
  std::size_t n_scenarios{0};
  std::size_t n_positions{0};
  double projections_per_second{0.0};
  std::uint64_t prepared_fingerprint{0};
  std::string as_of_date;
  std::int64_t as_of_ts_ns{0};
  std::uint64_t book_fingerprint{0};
};

struct ProjectedLegIdentity {
  std::uint32_t uid{0};
  std::string side;
  double quantity{0.0};
  double multiplier{0.0};
  std::int64_t relative_expiry_ns{0};
};

Status projected_tsv_header(std::ifstream &stream, const fs::path &path,
                            std::string_view expected) {
  std::string line;
  if (!std::getline(stream, line)) {
    return Err(ErrorCode::ParseError, path.filename().string() + " has no header");
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line != expected) {
    return Err(ErrorCode::ParseError,
               path.filename().string() + " header does not match the contract");
  }
  return Ok();
}

Result<std::vector<ProjectedVarSummaryRow>>
read_projected_var_summary(const fs::path &path, std::size_t n_sessions) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::IoError, "cannot read " + path.string());
  }
  ATX_TRY_VOID(projected_tsv_header(stream, path, kProjectedVarHeader));

  std::vector<ProjectedVarSummaryRow> rows;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " contains an empty/trailing row");
    }
    const std::vector<std::string_view> fields = split(line, '\t');
    if (fields.size() != 11u) {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " row does not have 11 fields");
    }
    ProjectedVarSummaryRow row;
    if (!parse_double(fields[0], row.confidence) ||
        !parse_double(fields[1], row.reference_value) ||
        !parse_double(fields[2], row.value_at_risk) ||
        !parse_double(fields[3], row.expected_shortfall) ||
        !parse_number(fields[4], row.n_scenarios) ||
        !parse_number(fields[5], row.n_positions) ||
        !parse_double(fields[6], row.projections_per_second) ||
        !parse_number(fields[7], row.prepared_fingerprint) || fields[8].empty() ||
        !parse_number(fields[9], row.as_of_ts_ns) ||
        !parse_number(fields[10], row.book_fingerprint)) {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " row contains invalid or trailing input");
    }
    row.as_of_date = fields[8];
    if (!(row.confidence > 0.0 && row.confidence < 1.0) ||
        row.projections_per_second <= 0.0 || row.n_scenarios == 0u ||
        row.n_positions == 0u || row.prepared_fingerprint == 0u ||
        row.as_of_ts_ns <= 0 || row.book_fingerprint == 0u) {
      return Err(ErrorCode::InvalidArgument,
                 path.filename().string() + " row violates the projected-VaR contract");
    }
    if (n_sessions != 0u && row.n_scenarios != n_sessions) {
      return Err(ErrorCode::InvalidArgument,
                 "projected VaR covers " + std::to_string(row.n_scenarios) +
                     " scenarios but the run has " + std::to_string(n_sessions) + " sessions");
    }
    if (!rows.empty()) {
      const ProjectedVarSummaryRow &first = rows.front();
      if (row.reference_value != first.reference_value ||
          row.n_scenarios != first.n_scenarios || row.n_positions != first.n_positions ||
          row.prepared_fingerprint != first.prepared_fingerprint ||
          row.as_of_date != first.as_of_date || row.as_of_ts_ns != first.as_of_ts_ns ||
          row.book_fingerprint != first.book_fingerprint ||
          row.confidence <= rows.back().confidence) {
        return Err(ErrorCode::InvalidArgument,
                   path.filename().string() + " rows disagree on run/book identity");
      }
    }
    rows.push_back(std::move(row));
  }
  if (!stream.eof()) {
    return Err(ErrorCode::IoError, "cannot finish reading " + path.string());
  }
  if (rows.size() != 2u || rows[0].confidence != 0.95 || rows[1].confidence != 0.99) {
    return Err(ErrorCode::InvalidArgument,
               path.filename().string() + " must contain exactly 0.95 and 0.99");
  }
  return Ok(std::move(rows));
}

Status verify_projected_var_artifacts_impl(const fs::path &run_dir, std::size_t n_sessions,
                                           std::span<const SnapshotRef> expected_sessions);

} // namespace

Status verify_projected_var_artifacts(const fs::path &run_dir, std::size_t n_sessions) {
  return verify_projected_var_artifacts_impl(run_dir, n_sessions, {});
}

namespace {

Result<std::vector<HistoricalProjectionFrame>>
read_projected_scenarios(const fs::path &path, const ProjectedVarSummaryRow &summary,
                         std::span<const SnapshotRef> expected_sessions) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::IoError, "cannot read " + path.string());
  }
  ATX_TRY_VOID(projected_tsv_header(stream, path, kProjectedScenarioHeader));

  std::vector<HistoricalProjectionFrame> frames;
  frames.reserve(summary.n_scenarios);
  std::string line;
  std::string previous_date;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " contains an empty/trailing row");
    }
    const std::vector<std::string_view> fields = split(line, '\t');
    if (fields.size() != 10u) {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " row does not have 10 fields");
    }
    HistoricalProjectionFrame frame;
    std::string date{fields[0]};
    if (date.empty() || !parse_number(fields[1], frame.ts_ns) ||
        !parse_double(fields[2], frame.value) || !parse_double(fields[3], frame.delta) ||
        !parse_double(fields[4], frame.gamma) || !parse_double(fields[5], frame.vega) ||
        !parse_double(fields[6], frame.theta) || !parse_number(fields[7], frame.n_ok) ||
        !parse_number(fields[8], frame.n_failed) ||
        !parse_number(fields[9], frame.definition_fingerprint)) {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " row contains invalid or trailing input");
    }
    if (frame.ts_ns <= 0 || frame.n_ok != summary.n_positions || frame.n_failed != 0u ||
        frame.definition_fingerprint == 0u || utc_date_from_ns(frame.ts_ns) != date ||
        (!previous_date.empty() && date <= previous_date)) {
      return Err(ErrorCode::InvalidArgument,
                 path.filename().string() + " row violates date/position/risk invariants");
    }
    if (!expected_sessions.empty() &&
        (frames.size() >= expected_sessions.size() ||
         date != expected_sessions[frames.size()].date)) {
      return Err(ErrorCode::InvalidArgument,
                 path.filename().string() + " dates do not match the qualified clock");
    }
    previous_date = std::move(date);
    frames.push_back(frame);
  }
  if (!stream.eof()) {
    return Err(ErrorCode::IoError, "cannot finish reading " + path.string());
  }
  if (frames.size() != summary.n_scenarios ||
      (!expected_sessions.empty() && frames.size() != expected_sessions.size())) {
    return Err(ErrorCode::InvalidArgument,
               path.filename().string() + " row count does not match n_scenarios");
  }
  return Ok(std::move(frames));
}

Status verify_projected_legs(const fs::path &path, const ProjectedVarSummaryRow &summary,
                             std::span<const HistoricalProjectionFrame> frames) {
  if (summary.n_positions > std::numeric_limits<std::size_t>::max() / summary.n_scenarios) {
    return Err(ErrorCode::InvalidArgument, "projected risk leg row count overflows");
  }
  const std::size_t expected_rows = summary.n_positions * summary.n_scenarios;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Err(ErrorCode::IoError, "cannot read " + path.string());
  }
  ATX_TRY_VOID(projected_tsv_header(stream, path, kProjectedLegHeader));

  std::vector<ProjectedLegIdentity> identities(summary.n_positions);
  std::vector<std::size_t> aggregate_fingerprints(
      summary.n_scenarios, static_cast<std::size_t>(summary.prepared_fingerprint));
  std::string line;
  std::size_t row_index = 0u;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " contains an empty/trailing row");
    }
    if (row_index >= expected_rows) {
      return Err(ErrorCode::InvalidArgument,
                 path.filename().string() + " has more rows than the summary permits");
    }
    const std::vector<std::string_view> fields = split(line, '\t');
    if (fields.size() != 15u) {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " row does not have 15 fields");
    }
    const std::size_t scenario_index = row_index / summary.n_positions;
    const std::size_t expected_leg = row_index % summary.n_positions;
    std::size_t leg = 0u;
    std::uint32_t uid = 0u;
    std::int64_t expiry_ts_ns = 0;
    double strike = 0.0;
    double quantity = 0.0;
    double multiplier = 0.0;
    double mark = 0.0;
    double delta = 0.0;
    double gamma = 0.0;
    double vega = 0.0;
    double theta = 0.0;
    std::uint64_t definition_fingerprint = 0u;
    if (fields[0] != utc_date_from_ns(frames[scenario_index].ts_ns) ||
        !parse_number(fields[1], leg) || !parse_number(fields[2], uid) ||
        (fields[3] != "Call" && fields[3] != "Put") ||
        !parse_number(fields[4], expiry_ts_ns) || !parse_double(fields[5], strike) ||
        !parse_double(fields[6], quantity) || !parse_double(fields[7], multiplier) ||
        !parse_double(fields[8], mark) || !parse_double(fields[9], delta) ||
        !parse_double(fields[10], gamma) || !parse_double(fields[11], vega) ||
        !parse_double(fields[12], theta) ||
        !parse_number(fields[13], definition_fingerprint) || fields[14] != "Ok") {
      return Err(ErrorCode::ParseError,
                 path.filename().string() + " row contains invalid or trailing input");
    }
    if (leg != expected_leg || uid == 0u || expiry_ts_ns <= frames[scenario_index].ts_ns ||
        strike <= 0.0 || quantity == 0.0 || multiplier <= 0.0 || mark < 0.0 ||
        definition_fingerprint == 0u) {
      return Err(ErrorCode::InvalidArgument,
                 path.filename().string() + " row violates leg identity/risk invariants");
    }
    const ProjectedLegIdentity identity{uid, std::string(fields[3]), quantity, multiplier,
                                        expiry_ts_ns - frames[scenario_index].ts_ns};
    if (scenario_index == 0u) {
      identities[leg] = identity;
    } else {
      const ProjectedLegIdentity &first = identities[leg];
      if (identity.uid != first.uid || identity.side != first.side ||
          identity.quantity != first.quantity || identity.multiplier != first.multiplier ||
          identity.relative_expiry_ns != first.relative_expiry_ns) {
        return Err(ErrorCode::InvalidArgument,
                   path.filename().string() + " position identity changes between scenarios");
      }
    }
    aggregate_fingerprints[scenario_index] =
        atx::core::hash_combine(aggregate_fingerprints[scenario_index],
                               definition_fingerprint);
    ++row_index;
  }
  if (!stream.eof()) {
    return Err(ErrorCode::IoError, "cannot finish reading " + path.string());
  }
  if (row_index != expected_rows) {
    return Err(ErrorCode::InvalidArgument,
               path.filename().string() + " row count does not match scenarios * positions");
  }
  for (std::size_t scenario = 0u; scenario < frames.size(); ++scenario) {
    std::uint64_t fingerprint =
        static_cast<std::uint64_t>(aggregate_fingerprints[scenario]);
    if (fingerprint == 0u) {
      fingerprint = 1u;
    }
    if (fingerprint != frames[scenario].definition_fingerprint) {
      return Err(ErrorCode::InvalidArgument,
                 path.filename().string() +
                     " definition fingerprints disagree with the scenario table");
    }
  }
  return Ok();
}

Status verify_projected_var_artifacts_impl(const fs::path &run_dir, std::size_t n_sessions,
                                           std::span<const SnapshotRef> expected_sessions) {
  const std::array<fs::path, 3> paths = {
      run_dir / "projected_var.tsv", run_dir / "projected_risk_scenarios.tsv",
      run_dir / "projected_risk_legs.tsv"};
  const std::array<fs::path, 3> pending = {
      run_dir / "projected_var.tsv.pending", run_dir / "projected_risk_scenarios.tsv.pending",
      run_dir / "projected_risk_legs.tsv.pending"};
  std::error_code error;
  const auto clear_not_found = [&error]() {
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
    }
  };
  bool any_present = false;
  bool all_present = true;
  for (const fs::path &path : paths) {
    const bool present = fs::is_regular_file(path, error);
    clear_not_found();
    if (error) {
      return Err(ErrorCode::IoError, "cannot inspect " + path.string());
    }
    any_present = any_present || present;
    all_present = all_present && present;
  }
  for (const fs::path &path : pending) {
    const bool present = fs::exists(path, error);
    clear_not_found();
    if (present) {
      return Err(ErrorCode::Unavailable,
                 "projected VaR has an unpublished generation: " + path.string());
    }
    if (error) {
      return Err(ErrorCode::IoError, "cannot inspect " + path.string());
    }
  }
  if (!any_present) {
    return Ok();
  }
  if (!all_present) {
    return Err(ErrorCode::NotFound, "projected VaR artifact generation is incomplete");
  }
  for (const fs::path &path : paths) {
    if (fs::file_size(path, error) == 0u || error) {
      return Err(ErrorCode::InvalidArgument,
                 "projected VaR artifact is empty/unreadable: " + path.string());
    }
  }

  ATX_TRY(std::vector<ProjectedVarSummaryRow> summaries,
          read_projected_var_summary(paths[0], n_sessions));
  const ProjectedVarSummaryRow &summary = summaries.front();
  ATX_TRY(std::vector<HistoricalProjectionFrame> frames,
          read_projected_scenarios(paths[1], summary, expected_sessions));
  ATX_TRY_VOID(verify_projected_legs(paths[2], summary, frames));
  if (summary.as_of_date != utc_date_from_ns(frames.back().ts_ns) ||
      summary.as_of_ts_ns != frames.back().ts_ns ||
      summary.reference_value != frames.back().value) {
    return Err(ErrorCode::InvalidArgument,
               "projected VaR summary does not identify the final scenario/as-of book");
  }
  for (const ProjectedVarSummaryRow &row : summaries) {
    ATX_TRY(ProjectedHistoricalVar expected,
            projected_historical_var(frames, frames.back().value, row.confidence));
    if (row.reference_value != expected.reference_value ||
        row.value_at_risk != expected.value_at_risk ||
        row.expected_shortfall != expected.expected_shortfall ||
        row.n_scenarios != expected.n_scenarios) {
      return Err(ErrorCode::InvalidArgument,
                 "projected VaR risk summary does not recompute from scenario rows");
    }
  }
  return Ok();
}

} // namespace

// â”€â”€ Public: native reference reconciliation (M1) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

// â”€â”€ Public: file-oriented workflow entry points â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

Status dispersion_build_corpus(const fs::path &source_spec_path, const fs::path &run_dir,
                               const DispersionCorpusPolicy &policy) {
  ATX_TRY(RunSpec spec, read_run_spec(source_spec_path));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(spec.universe_path));
  const std::vector<std::string> symbols = all_symbols(universe_rows, spec.index_symbol);
  // L9 (RECONCILE 1): the entry-gate floor reads from the ONE versioned
  // methodology policy rather than a scattered literal. `min_names_entry` == 51,
  // so this is the same gate the inline `51u` enforced â€” main's example already
  // read it from here, and dispatching build-corpus into this function must not
  // quietly put the literal back.
  const ListedDispersionMethodology methodology;
  if (spec.core_mode && symbols.size() < methodology.min_names_entry) {
    return Err(ErrorCode::InvalidArgument, "core mode requires SPY plus at least 50 names");
  }
  reset_corpus_phase_timings();
  std::error_code fs_error;
  fs::create_directories(run_dir / "archives", fs_error);
  if (fs_error) {
    return Err(ErrorCode::IoError, "cannot create run directory");
  }
  std::ofstream input_inventory(run_dir / "input_inventory.tsv",
                                std::ios::binary | std::ios::trunc);
  if (!input_inventory) {
    return Err(ErrorCode::IoError, "cannot write input inventory");
  }
  input_inventory
      << "date\tsymbol\tpath\tstatus\tsource_schema_version\tsource_fingerprint\t"
         "market_input_fingerprint\n";
  std::ofstream occ_inventory;
  std::set<std::string> occ_dates;
  if (!spec.occ_ess_root.empty()) {
    fs::create_directories(run_dir / "occ_ess", fs_error);
    if (fs_error) {
      return Err(ErrorCode::IoError, "cannot create OCC ESS evidence directory");
    }
    occ_inventory.open(run_dir / "occ_ess_inventory.tsv",
                       std::ios::binary | std::ios::trunc);
    if (!occ_inventory) {
      return Err(ErrorCode::IoError, "cannot write OCC ESS inventory");
    }
    occ_inventory << "date\tpath\tn_special_symbols\tsource_fingerprint\n";
  }
  ATX_TRY_VOID(write_methodology_map(run_dir / "methodology_map.tsv"));
  const std::uint64_t input_fingerprint =
      dispersion_input_fingerprint(spec.date_lo, spec.date_hi, symbols.size());
  QualifiedCorpusConfig config =
      dispersion_corpus_config(policy, spec.fit_workers, input_fingerprint);
  ATX_TRY(CorpusBuildSession session,
          CorpusBuildSession::create((run_dir / "archives").string(), config));
  // P-2/B1: load, fit, checkpoint, and release one bounded DATE window.
  //
  // Before P-2, `date_batch` bounded fitted surfaces only: the full
  // date_lo..date_hi OpraBatchResult (and every panel/quote) was resident before
  // the first fit. `load_opra_date_windows` owns the outer date loop and does not
  // start the next load until this consumer has completed `append_dates`.
  // Therefore peak panels are <= date_batch * symbols, and checkpointed panels
  // are destroyed before the next window is read.
  const std::size_t date_batch = corpus_date_batch_size();
  OpraBatchSpec opra_spec = batch_spec(spec, symbols, spec.date_lo, spec.date_hi);
  if (!spec.dividend_inputs_path.empty()) {
    ATX_TRY(opra_spec.market_inputs,
            read_corpus_dividend_inputs(spec.dividend_inputs_path));
  }
  std::vector<ShareDividendObservation> dividend_observations;
  OpraWindowLoadStats load_stats;
  const auto pipeline_begin = std::chrono::steady_clock::now();
  ATX_TRY_VOID(load_opra_date_windows(
      opra_spec, date_batch,
      [&](OpraBatchResult &&batch) -> Status {
        ATX_TRY_VOID(append_input_inventory(input_inventory, batch));
        if (!spec.occ_ess_root.empty()) {
          ATX_TRY_VOID(persist_occ_ess_evidence_window(
              run_dir, spec, batch, occ_inventory, occ_dates));
        }
        std::vector<std::string> window_dates;
        std::vector<std::vector<CorpusCellInput>> window_cells;
        std::size_t cursor = 0u;
        while (cursor < batch.entries.size()) {
          const std::string date = batch.entries[cursor].date;
          std::vector<CorpusCellInput> cells;
          while (cursor < batch.entries.size() &&
                 batch.entries[cursor].date == date) {
            OpraBatchEntry &entry = batch.entries[cursor++];
            if (entry.panel) {
              if (!entry.panel->frame.divs.empty()) {
                const OpraMarketInputProvenance &provenance =
                    entry.panel->market_input_provenance;
                if (provenance.dividends.source.empty() ||
                    provenance.dividends.as_of.empty() ||
                    entry.panel->source_fingerprint == 0u ||
                    provenance.fingerprint == 0u) {
                  return Err(ErrorCode::InvalidArgument,
                             "cash-dividend panel lacks authoritative provenance");
                }
                for (const DividendEvent &event : entry.panel->frame.divs) {
                  dividend_observations.push_back(
                      ShareDividendObservation{
                          entry.date, entry.symbol, uid_for_symbol(entry.symbol),
                          event.ex_date_ns, event.amount,
                          provenance.dividends.source, provenance.dividends.as_of,
                          entry.panel->source_fingerprint, provenance.fingerprint});
                }
              }
              cells.emplace_back(corpus_board_from_opra(
                  entry.date, entry.symbol, std::move(*entry.panel)));
            } else {
              CorpusSourceFailure failure;
              failure.date = entry.date;
              failure.symbol = entry.symbol;
              failure.reason =
                  entry.panel.error().code() == ErrorCode::NotFound
                      ? CorpusAdmissionReason::MissingSource
                      : CorpusAdmissionReason::InvalidSourceSchema;
              failure.error_code = entry.panel.error().code();
              cells.emplace_back(std::move(failure));
            }
          }
          window_dates.push_back(date);
          window_cells.push_back(std::move(cells));
        }
        std::vector<CorpusBuildSession::DateCells> batched;
        batched.reserve(window_dates.size());
        for (std::size_t i = 0u; i < window_dates.size(); ++i) {
          batched.push_back(
              CorpusBuildSession::DateCells{window_dates[i], window_cells[i]});
        }
        return session.append_dates(batched);
      },
      &load_stats));
  if (!input_inventory || (!spec.occ_ess_root.empty() && !occ_inventory)) {
    return Err(ErrorCode::IoError, "cannot flush corpus input evidence");
  }
  if (!spec.occ_ess_root.empty() && occ_dates.empty()) {
    return Err(ErrorCode::NotFound, "no loaded dates for OCC ESS evidence");
  }
  ATX_TRY(QualifiedCorpusManifest built, session.finish());
  const double pipeline_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - pipeline_begin).count();
  const double ingest_s = load_stats.load_wall_s;
  const double build_s = std::max(0.0, pipeline_s - ingest_s);
  ATX_TRY_VOID(write_share_dividend_artifact(
      run_dir / "share_dividends.tsv", dividend_observations));
  ATX_TRY_VOID(write_manifest_file((run_dir / "surface_manifest.tsv").string(), built.manifest));
  ATX_TRY_VOID(write_quality_report_file((run_dir / "quality.tsv").string(), built.quality));
  fs::copy_file(spec.universe_path, run_dir / "universe_schedule.tsv",
                fs::copy_options::overwrite_existing, fs_error);
  if (fs_error) {
    return Err(ErrorCode::IoError, "cannot copy universe schedule");
  }
  RunSpec persisted_spec = spec;
  persisted_spec.universe_path = "universe_schedule.tsv";
  persisted_spec.dividend_inputs_path.clear();
  persisted_spec.dividend_ledger_path = "share_dividends.tsv";
  if (!spec.definitions_path.empty()) {
    fs_error.clear();
    fs::copy_file(spec.definitions_path, run_dir / "definitions.tsv",
                  fs::copy_options::overwrite_existing, fs_error);
    if (fs_error)
      return Err(ErrorCode::IoError, "cannot copy definitions");
    persisted_spec.definitions_path = "definitions.tsv";
  }
  ATX_TRY_VOID(write_resolved_spec(run_dir / "run_spec.tsv", persisted_spec));
  // WS-F F4 (BT-W), second half of the wiring gap: the RunSpec writer knows only
  // the RunSpec vocabulary, so every typed knob was dropped here.
  ATX_TRY_VOID(persist_typed_spec_keys(source_spec_path, run_dir / "run_spec.tsv"));
  detail::log_emitf(LogLevel::Info, LogStream::Stdout,
                    "built qualified corpus: admitted=%u quarantined=%u source_failed=%u",
                    built.quality.n_admitted, built.quality.n_quarantined,
                    built.quality.n_source_failed);
  if (corpus_phase_timing_enabled()) {
    detail::log_emitf(LogLevel::Info, LogStream::Stdout, "%s",
                      format_corpus_phase_line(ingest_s, build_s, corpus_phase_timings(),
                                               date_batch, load_stats.load_process_cpu_s)
                          .c_str());
  }
  return Ok();
}

// â”€â”€ X5: tearsheet emission â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

namespace {

[[nodiscard]] std::string metric_text(double value) {
  std::array<char, 40> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%.10g", value);
  return std::string(buffer.data(), written > 0 ? static_cast<std::size_t>(written) : 0u);
}

[[nodiscard]] std::string_view weighting_text(WeightingScheme scheme) noexcept {
  switch (scheme) {
  case WeightingScheme::VegaNeutral:
    return "vega_neutral";
  case WeightingScheme::EqualVega:
    return "equal_vega";
  case WeightingScheme::GammaNeutral:
    return "gamma_neutral";
  case WeightingScheme::ThetaNeutral:
    return "theta_neutral";
  }
  return "unknown";
}

[[nodiscard]] std::string_view strike_text(StrikeRule rule) noexcept {
  switch (rule) {
  case StrikeRule::AtmForwardStraddle:
    return "atm_forward_straddle";
  case StrikeRule::FixedMoneyness:
    return "fixed_moneyness";
  case StrikeRule::DeltaStrangle:
    return "delta_strangle";
  }
  return "unknown";
}

} // namespace

std::vector<std::pair<std::string, std::string>>
dispersion_report_metadata(const DispersionRunConfig &config, const TearSheet &sheet,
                           std::size_t n_sessions) {
  std::vector<std::pair<std::string, std::string>> meta;
  const DispersionFrictionRegime regime = dispersion_friction_regime(config);
  // REGIME FIRST, ALWAYS. A reader (human or renderer) that sees only the head of
  // this block still knows which execution assumptions produced every number
  // below it. `gross_return` is the pre-cost figure and `total_cost` the drag, so
  // the cost share of the headline is checkable without a second artifact.
  // THE REGIME IS NOT OPTIONAL METADATA. This single emplace_back is the one and
  // only source of the `friction_regime` key on the tearsheet artifacts
  // (surface_tearsheet.tsv + surface_pnl_track.tsv); the Python renderer contract
  // that HARD-REFUSES a track without it lives in
  // tools/spy_dispersion_tearsheet_report.py (Python-side enforcement is task Y4).
  meta.emplace_back("friction_regime", std::string(to_string(regime)));
  meta.emplace_back("friction_detail", dispersion_regime_detail(config.frictions, config.costs));
  // E1 fix round: D1's kBacktestEconomicsRev, right beside the regime it
  // completes -- "what assumptions" (friction_regime/friction_detail) and
  // "which engine build interpreted them" (economics_rev) belong together.
  meta.emplace_back("economics_rev", std::to_string(kBacktestEconomicsRev));
  meta.emplace_back("total_return", metric_text(sheet.total_return));
  meta.emplace_back("total_cost", metric_text(sheet.total_cost));
  meta.emplace_back("total_financing", metric_text(sheet.total_financing));
  meta.emplace_back("gross_return", metric_text(sheet.total_return + sheet.total_cost));

  meta.emplace_back("label", config.label);
  meta.emplace_back("date_lo", config.dates.lo);
  meta.emplace_back("date_hi", config.dates.hi);
  meta.emplace_back("n_sessions", std::to_string(n_sessions));
  meta.emplace_back("index_symbol", config.universe.index_symbol);
  meta.emplace_back("gross_index_vega", metric_text(config.gross_index_vega));
  // X4 policies, so a report states the construction it describes.
  meta.emplace_back("weighting", std::string(weighting_text(config.weighting)));
  meta.emplace_back("strike_rule", std::string(strike_text(config.strike.rule)));
  if (config.strike.rule == StrikeRule::FixedMoneyness) {
    meta.emplace_back("strike_log_moneyness", metric_text(config.strike.log_moneyness));
  }
  if (config.strike.rule == StrikeRule::DeltaStrangle) {
    meta.emplace_back("strike_abs_delta", metric_text(config.strike.target_abs_delta));
  }

  meta.emplace_back("sharpe", metric_text(sheet.sharpe));
  meta.emplace_back("ann_return", metric_text(sheet.ann_return));
  meta.emplace_back("ann_vol", metric_text(sheet.ann_vol));
  meta.emplace_back("max_drawdown", metric_text(sheet.max_drawdown));
  meta.emplace_back("hit_rate", metric_text(sheet.hit_rate));
  meta.emplace_back("return_on_gross_vega", metric_text(sheet.return_on_gross_vega));
  meta.emplace_back("avg_gross_vega", metric_text(sheet.avg_gross_vega));

  // Benchmark-relative keys appear ONLY when a benchmark was actually supplied,
  // so an absent benchmark cannot be misread as a zero alpha / zero beta.
  if (sheet.benchmark.has_benchmark) {
    // C-6: WHICH join produced these numbers. `exact_dates` means every strategy
    // observation is paired; `inner_join_on_dates` means the block covers the
    // intersection only, and `benchmark_n_obs` below is how much of it survived.
    meta.emplace_back("benchmark_join", std::string(to_string(config.benchmark_join)));
    meta.emplace_back("benchmark_n_obs", std::to_string(sheet.benchmark.n_obs));
    meta.emplace_back("benchmark_beta", metric_text(sheet.benchmark.beta));
    meta.emplace_back("benchmark_alpha", metric_text(sheet.benchmark.alpha));
    meta.emplace_back("benchmark_active_return", metric_text(sheet.benchmark.active_return));
    meta.emplace_back("benchmark_tracking_error", metric_text(sheet.benchmark.tracking_error));
    meta.emplace_back("benchmark_information_ratio",
                      metric_text(sheet.benchmark.information_ratio));
    meta.emplace_back("benchmark_correlation", metric_text(sheet.benchmark.correlation));
  }
  return meta;
}

Status write_dispersion_tearsheet(const fs::path &run_dir, const DispersionRunConfig &config,
                                  const DispersionBacktestOutcome &outcome) {
  const std::vector<std::pair<std::string, std::string>> meta =
      dispersion_report_metadata(config, outcome.sheet, outcome.track.size());

  // The renderer's input: one self-describing TSV carrying the whole series plus
  // the regime-led metadata header.
  ATX_TRY_VOID(
      write_backtest_pnl_tsv(outcome.track, meta, (run_dir / "surface_pnl_track.tsv").string()));

  // The metrics table. Same `metric<TAB>value` shape as the rest of the run's
  // artifacts, and it opens with the regime for the same reason the meta does.
  std::ofstream out(run_dir / "surface_tearsheet.tsv", std::ios::binary | std::ios::trunc);
  if (!out) {
    return Err(ErrorCode::IoError, "cannot write surface tearsheet");
  }
  out << "metric\tvalue\n";
  for (const auto &[key, value] : meta) {
    out << key << '\t' << value << '\n';
  }
  const TearSheet &sheet = outcome.sheet;
  const std::pair<const char *, double> attribution[] = {
      {"attr_delta", sheet.attr_delta},   {"attr_gamma", sheet.attr_gamma},
      {"attr_vega", sheet.attr_vega},     {"attr_vanna", sheet.attr_vanna},
      {"attr_volga", sheet.attr_volga},   {"attr_theta", sheet.attr_theta},
      {"attr_rho", sheet.attr_rho},       {"attr_charm", sheet.attr_charm},
      {"attr_unexplained", sheet.attr_unexplained},
      {"attr_settlement", sheet.attr_settlement},
      {"attr_shares", sheet.attr_shares}, {"attr_financing", sheet.attr_financing},
      {"attr_cost", sheet.attr_cost},     {"avg_turnover", sheet.avg_turnover},
      {"vega_adj_sharpe", sheet.vega_adj_sharpe},
      {"pnl_per_vega_traded", sheet.pnl_per_vega_traded},
      {"avg_gross_gamma", sheet.avg_gross_gamma},
  };
  for (const auto &[key, value] : attribution) {
    out << key << '\t' << metric_text(value) << '\n';
  }
  if (!out) {
    return Err(ErrorCode::IoError, "cannot flush surface tearsheet");
  }
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
  // X5. Fold in the benchmark-relative block when â€” and only when â€” the spec
  // supplied a benchmark. Absent (the default) this is skipped entirely and the
  // sheet stays exactly the absolute one `run_dispersion_surface_backtest` built.
  if (!run_config.benchmark_series.empty()) {
    // C-6: joined BY DATE under the spec's named policy. A benchmark that does
    // not describe the strategy's own sessions fails the run instead of being
    // reported against the wrong observations.
    ATX_TRY(outcome.sheet, dispersion_tearsheet_with_benchmark(outcome.track, run_config));
  } else if (run_config.periods_per_year != 252.0) {
    outcome.sheet = tearsheet(outcome.track, run_config.periods_per_year);
  }
  const BacktestResult &backtest = outcome.track;
#if defined(ATX_VOL_PROFILE)
  if (run_config.emit_tsv_diagnostics) {
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
  if (run_config.emit_tsv_diagnostics) {
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
  // S3-T16. The surface route's three reporting artifacts. Unlike the listed
  // route there is no archive counterpart to fall back on, so a default run of
  // this stage publishes nothing and an operator who wants the tables declares
  // `emit_tsv_diagnostics true`. The economics above are unchanged either way â€”
  // the run still computes, and its regime/NAV console line still prints.
  if (run_config.emit_tsv_diagnostics) {
    ATX_TRY_VOID(write_backtest_tsv(backtest, (run_dir / "surface_backtest.tsv").string()));
    // X5. Two ADDITIONAL artifacts; `surface_backtest.tsv` above is untouched, so
    // the reproducibility pin is measured on exactly the bytes it always was.
    ATX_TRY_VOID(write_dispersion_tearsheet(run_dir, run_config, outcome));
  }
  const DispersionFrictionRegime regime = dispersion_friction_regime(run_config);
  // The console line names the regime too: the single most common way to
  // misread this run is to quote its final_nav without knowing which
  // execution assumptions produced it.
  detail::log_emitf(LogLevel::Info, LogStream::Stdout,
                    "surface-only projected backtest complete: dates=%zu final_nav=%.10g "
                    "regime=%s (%s) economics_rev=%d cost=%.10g",
                    backtest.size(), backtest.nav.back(), std::string(to_string(regime)).c_str(),
                    dispersion_regime_detail(run_config.frictions, run_config.costs).c_str(),
                    kBacktestEconomicsRev, outcome.sheet.total_cost);
  return Ok();
}

namespace {

// REVIEW C-1. Identity of the AS-OF BOOK, published beside the VaR so a run's
// number can be tied to the exact portfolio it describes.
//
// It is NOT the same thing as `prepared_fingerprint`, which the summary already
// carries: that one hashes the RELATIVE templates the projection re-ages (uid,
// side, multiplier, an ATM-forward strike and a relative maturity) plus their
// quantities. The absolute strikes and expiries the book was actually sized at
// never enter it, so two books struck at different levels on different sessions
// can share a prepared fingerprint. This hashes the book itself.
[[nodiscard]] std::uint64_t dispersion_book_fingerprint(const DispersionBook &book) {
  std::string material;
  material.reserve(book.positions.size() * 6u * sizeof(std::uint64_t));
  const auto append = [&material](const void *bytes, std::size_t n) {
    material.append(static_cast<const char *>(bytes), n);
  };
  const std::uint64_t count = book.positions.size();
  append(&count, sizeof count);
  for (const Position &position : book.positions) {
    const std::uint32_t uid = position.contract.uid;
    const std::uint8_t side = position.contract.side == Side::Call ? 0u : 1u;
    const std::uint64_t strike_bits = std::bit_cast<std::uint64_t>(position.contract.K);
    const std::uint64_t tenor_bits = std::bit_cast<std::uint64_t>(position.contract.T);
    const std::uint64_t qty_bits = std::bit_cast<std::uint64_t>(position.qty);
    const std::uint64_t multiplier_bits = std::bit_cast<std::uint64_t>(position.multiplier);
    append(&uid, sizeof uid);
    append(&side, sizeof side);
    append(&strike_bits, sizeof strike_bits);
    append(&tenor_bits, sizeof tenor_bits);
    append(&qty_bits, sizeof qty_bits);
    append(&multiplier_bits, sizeof multiplier_bits);
  }
  const std::uint64_t digest = atx::core::hash_bytes(material.data(), material.size());
  return digest == 0u ? 1u : digest; // 0 is reserved for "absent"
}

Status invalidate_projected_var_generation(const fs::path &run_dir) {
  // projected_var.tsv is the commit record and is published LAST. Removing it
  // first makes every earlier generation unverifiable before any fallible input
  // read or projection begins; stale companions can never bless a failed rerun.
  const std::array<fs::path, 4> stale = {
      run_dir / "projected_var.tsv", run_dir / "projected_var.tsv.pending",
      run_dir / "projected_risk_scenarios.tsv.pending",
      run_dir / "projected_risk_legs.tsv.pending"};
  for (const fs::path &path : stale) {
    std::error_code error;
    fs::remove(path, error);
    if (error) {
      return Err(ErrorCode::IoError,
                 "projected VaR: cannot invalidate stale generation " + path.string());
    }
  }
  return Ok();
}

Status publish_projected_var_generation(const fs::path &run_dir) {
  // All three files are already closed and complete at this point. Publish the
  // two data tables first and the summary commit record last. verify rejects
  // both pending files and any strict subset of the canonical triple, so every
  // interruption is recoverable by the next rerun and cannot verify as current.
  for (const char *leaf :
       {"projected_risk_scenarios.tsv", "projected_risk_legs.tsv", "projected_var.tsv"}) {
    const fs::path target = run_dir / leaf;
    const fs::path pending = target.string() + ".pending";
    std::error_code error;
    fs::remove(target, error);
    if (error) {
      return Err(ErrorCode::IoError, "projected VaR: cannot replace " + target.string());
    }
    fs::rename(pending, target, error);
    if (error) {
      return Err(ErrorCode::IoError, "projected VaR: cannot publish " + target.string());
    }
  }
  return Ok();
}

} // namespace

Status dispersion_run_projected_var(const fs::path &run_dir, CancelToken cancel) {
  ATX_TRY_VOID(invalidate_projected_var_generation(run_dir));
  // REVIEW C-15. This used to be the LOOSE `read_run_spec`, on the stated
  // grounds that "a projected-VaR run consumes no execution knobs". True, and
  // beside the point: it consumes CONSTRUCTION knobs â€” side, weighting, strike
  // policy and contract multiplier â€” and the loose reader has no field for any
  // of them, so the route hardcoded two and ignored two. One spec therefore
  // built one book in `run-surface-backtest` and a different book here, with no
  // error and no diagnostic. The same run directory's `run_spec.tsv` is already
  // read strictly by `dispersion_run_surface_backtest`, so this is the reader it
  // always should have used; it also means a misspelled key now fails BY NAME
  // instead of being silently dropped.
  ATX_TRY(DispersionRunConfig run_config, read_dispersion_run_config(run_dir / "run_spec.tsv"));
  ATX_TRY(std::vector<UniverseRow> universe_rows, read_universe(run_dir / "universe_schedule.tsv"));
  ATX_TRY(CorpusManifest manifest, read_manifest_file((run_dir / "surface_manifest.tsv").string()));
  ATX_TRY(Clock clock, Clock::from_manifest(manifest));
  if (clock.size() == 0u)
    return Err(ErrorCode::Unavailable, "projected VaR: empty qualified clock");

  // P-1: open the current anchor FIRST and keep only this one whole-board
  // snapshot. It supplies both PIT symbol resolution and book construction.
  // Historical snapshots are loaded later, after the book tells us its required
  // uids, under sealed read-only backing and in bounded batches.
  ATX_TRY(MarketSnapshot as_of_snapshot,
          MarketSnapshot::load(clock.refs().back().archive_path,
                               QueryPricingTier::LegacyCompatible, {},
                               ArchiveBacking::Sealed));

  // C1-ACTIVATE (projected VaR). The book this VaR is measured on is built from
  // ONE anchor snapshot, so "point-in-time" here means resolving the basket from
  // the schedule at THAT snapshot's own timestamp rather than string-matching the
  // manifest's first session date. Routing it through the shared PIT resolver
  // removes the day-1 freeze and the manifest-string coupling in one move; with a
  // single-block schedule the resolved basket is identical to before.
  //
  // â”€â”€ REVIEW C-1: THE AS-OF IS THE LAST QUALIFIED SNAPSHOT â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
  //
  // The anchor used to be `snapshots.front()` â€” the OLDEST session â€” while
  // `dispersion_book_var` fixes the VaR reference at `frames.back().value`, the
  // LAST session (listed_dispersion_pipeline.cpp:508-516), and every loss is
  // `reference_value - frame.value` (historical_projection.cpp:118-147). That is
  // historical-simulation VaR, and it is only VaR if the book being re-valued is
  // the book actually HELD at the reference date. It was not: a name
  // reconstituted out of the basket mid-window was still in it, a name
  // reconstituted in was missing from it, and every quantity was sized on the
  // oldest session's spot and vol. The output stayed finite and plausible while
  // describing a portfolio nobody holds.
  //
  // AS-OF SEMANTICS, chosen deliberately: the as-of is THE LAST QUALIFIED
  // SNAPSHOT IN THE MANIFEST CLOCK. It is not a caller-supplied parameter,
  // because the reference value is not one either â€” the seam fixes it at
  // `frames.back()`, so any anchor other than the last session simply re-opens
  // the mismatch this closes. A caller who wants a different as-of moves
  // `date_hi`, which already bounds which sessions qualify. The book is then
  // IMMUTABLE and is projected back over every prior surface.
  //
  // Both the as-of timestamp and a fingerprint of the resulting book are written
  // into `projected_var.tsv` below, so a reader is told which session the number
  // belongs to instead of having to infer it from the manifest.
  const MarketSnapshot &as_of = as_of_snapshot;
  // REVIEW C-15: `index_symbol` was hardcoded to the resolver's "SPY" default,
  // so a run whose index leg is anything else could not resolve its own basket.
  const auto pit = make_pit_universe_resolver(universe_rows, run_config.universe.index_symbol);
  ATX_TRY(DispersionUniverse authored, pit(as_of.ts_ns()));
  ATX_TRY(ResolvedUniverse resolved,
          resolve_universe_uids(
              authored, [&](std::string_view symbol) { return as_of.uid_of(symbol); },
              MissingNameSpec{MissingNamePolicy::DropRenormalize, run_config.universe.min_names}));
  // REVIEW C-15: ONE builder, shared with the surface route
  // (`dispersion_backtest.cpp:make_specs`), so side / weighting / strike /
  // multiplier / target vega / tenor cannot drift between the two.
  DispersionConfig dispersion = dispersion_config_from(dispersion_backtest_config_from(run_config));
  // The one field this route sets itself, and why: relative-template VaR re-ages
  // every leg to `scenario_valuation + maturity`, so a RELATIVE maturity is
  // required unconditionally here. The shared builder keys `projected_maturity`
  // off `project_to_calendar_expiry`, which is a surface-replay knob; leaving it
  // unset would drop this route onto the legacy single-strike `target_T` path.
  dispersion.projected_maturity = ProjectedMaturitySpec::days(
      static_cast<std::int32_t>(std::llround(run_config.dte.target_days)));
  ATX_TRY(DispersionBook initial, build_dispersion_book(resolved.universe, as_of.set(), dispersion));
  const std::int64_t as_of_ts_ns = as_of.ts_ns();
  const std::string &as_of_date = clock.refs().back().date;
  const std::uint64_t book_fingerprint = dispersion_book_fingerprint(initial);

  // REV-TAIL I-2. The book -> OptionProjectionSpec synthesis + prepare +
  // evaluate_into + per-confidence VaR split USED to be hand-rolled inline here,
  // a second copy of `dispersion_book_var` (listed_dispersion_pipeline.cpp:464).
  // `347ad44` â€” the commit whose stated purpose was restoring CLI seams â€” created
  // that copy by inlining, and in doing so left `dispersion_book_var` with ZERO
  // production callers: an exported public API whose only remaining caller was a
  // test. That is exactly the shape this file's own header warns rots quietly and
  // is then deleted by someone who assumes it was always dead. Worse, the tested
  // copy was the ORPHANED one â€” `dispersion_run_projected_var` has no test at all
  // â€” so the shipped economics were the untested duplicate of a tested seam.
  //
  // What `347ad44` did NOT do is regress this route. The pre-inline CLI body
  // (`b0080fa:examples/spy_dispersion_backtest.cpp:950,981-982`) read the SAME
  // loose `read_run_spec` and hardcoded the SAME `side` / `multiplier = 100.0`;
  // those are not a capability this route ever had and then lost. `347ad44`
  // genuinely ADDED C1-ACTIVATE point-in-time universe resolution above, replacing
  // `universe_at(universe_rows, clock.refs().front().date)`. So the correct repair
  // is to keep the PIT resolution and give the synthesis back to the seam, which
  // is what this call does. (REVIEW C-15 has since closed the loose-spec half of
  // that history: the route reads the STRICT typed config above and builds its
  // book through the shared builder, so the two routes cannot disagree.)
  //
  // ORDERING, stated because it is the one behavioural delta: the incomplete-frame
  // gate now fires INSIDE `dispersion_book_var`, i.e. BEFORE the loose TSVs are
  // written, so a failed projection no longer leaves `projected_risk_scenarios
  // .tsv` / `projected_risk_legs.tsv` behind in the run directory. That is the
  // `b0080fa` ordering, restored. `elapsed_seconds` likewise spans the whole call
  // (prepare + evaluate + risk) again rather than `evaluate_into` alone; it feeds
  // only `projections_per_second`, non-deterministic wall-clock telemetry.
  const std::vector<double> confidences = {0.95, 0.99};
  HistoricalProjectionConfig config;
  config.n_threads = run_config.fit.workers;
  std::vector<std::uint32_t> required_uids;
  required_uids.reserve(initial.positions.size());
  for (const Position &position : initial.positions) {
    required_uids.push_back(position.contract.uid);
  }
  std::sort(required_uids.begin(), required_uids.end());
  required_uids.erase(std::unique(required_uids.begin(), required_uids.end()),
                      required_uids.end());

  constexpr std::size_t kSnapshotBatch = 16u;
  const auto started = std::chrono::steady_clock::now();
  ATX_TRY(
      DispersionBookVar var,
      dispersion_book_var(
          initial, *dispersion.projected_maturity, clock.size(), confidences,
          [&](const PreparedHistoricalProjection &prepared,
              std::span<HistoricalProjectionFrame> output_frames,
              std::span<ProjectedOption> output_legs,
              const HistoricalProjectionConfig &evaluation_config) -> Status {
            for (std::size_t batch_start = 0u; batch_start < clock.size();
                 batch_start += kSnapshotBatch) {
              // Plan 5.5 safe point: the top of a snapshot batch. This lambda is
              // the whole of the entry's long-running work, and every artifact
              // (`projected_var.tsv`, the scenarios/legs pair, the generation
              // marker) is written only after it returns â€” so a stop here leaves
              // the run dir byte-for-byte as it was found. The Status propagates
              // out through dispersion_book_var's ATX_TRY.
              if (cancel.stop_requested()) {
                return Err(ErrorCode::Cancelled,
                           "dispersion_run_projected_var: cancelled before scenario batch at " +
                               std::to_string(batch_start) + " (no artifacts written)");
              }
              const std::size_t batch_size =
                  std::min(kSnapshotBatch, clock.size() - batch_start);
              std::vector<std::unique_ptr<MarketSnapshot>> owners;
              std::vector<HistoricalProjectionScenario> scenarios;
              owners.reserve(batch_size);
              scenarios.reserve(batch_size);
              for (std::size_t offset = 0u; offset < batch_size; ++offset) {
                const std::size_t scenario_index = batch_start + offset;
                if (scenario_index + 1u == clock.size()) {
                  scenarios.push_back({as_of.ts_ns(), &as_of.set()});
                  continue;
                }
                ATX_TRY(MarketSnapshot snapshot,
                        MarketSnapshot::load(
                            clock.refs()[scenario_index].archive_path,
                            QueryPricingTier::LegacyCompatible, required_uids,
                            ArchiveBacking::Sealed));
                owners.push_back(
                    std::make_unique<MarketSnapshot>(std::move(snapshot)));
                scenarios.push_back(
                    {owners.back()->ts_ns(), &owners.back()->set()});
              }
              ATX_TRY_VOID(prepared.evaluate_into(
                  scenarios, output_frames.subspan(batch_start, batch_size),
                  output_legs.subspan(batch_start * prepared.n_positions(),
                                      batch_size * prepared.n_positions()),
                  evaluation_config));
            }
            return Ok();
          },
          config));
  const double elapsed_seconds = std::max(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
      std::numeric_limits<double>::min());
  const std::vector<HistoricalProjectionFrame> &frames = var.frames;
  const std::vector<ProjectedOption> &legs = var.legs;

  // S3-T16. The canonical triple is this stage's ONLY output and it has no
  // archive counterpart, so with the flag off the projection still runs and
  // still reports on the console, but publishes nothing â€” and the unconditional
  // `invalidate_projected_var_generation` above has already removed any earlier
  // generation, so a run directory is never left claiming a VaR that no longer
  // matches its inputs. `verify_projected_var_artifacts` treats an absent triple
  // as "the optional stage did not publish", which is exactly the state here.
  if (run_config.emit_tsv_diagnostics) {
    std::ofstream frame_out(run_dir / "projected_risk_scenarios.tsv.pending",
                            std::ios::binary | std::ios::trunc);
    std::ofstream leg_out(run_dir / "projected_risk_legs.tsv.pending",
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
      // `var.n_positions == initial.positions.size()`, and the seam builds one
      // relative template per book position IN ORDER, so leg `i` is book position
      // `i` and its quantity is `initial.positions[i].qty` â€” the same value the
      // inlined copy read out of its own `relative_positions[leg].quantity`.
      for (std::size_t leg = 0; leg < var.n_positions; ++leg) {
        const ProjectedOption &projected = legs[scenario * var.n_positions + leg];
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
    // The incomplete-frame gate that used to sit here now fires inside
    // `dispersion_book_var`, before these writes â€” see the ordering note above.

    std::ofstream summary(run_dir / "projected_var.tsv.pending",
                          std::ios::binary | std::ios::trunc);
    if (!summary)
      return Err(ErrorCode::IoError, "projected VaR: cannot open summary");
    // The three trailing columns are REVIEW C-1's provenance: which session the
    // immutable book was resolved and sized on, and that book's identity. They are
    // APPENDED so `n_scenarios` stays field 5 for `verify_projected_var_artifacts`.
    summary << std::setprecision(17)
            << "confidence\treference_value\tvalue_at_risk\texpected_shortfall\tn_scenarios\t"
               "n_positions\tprojections_per_second\tprepared_fingerprint\tas_of_date\t"
               "as_of_ts_ns\tbook_fingerprint\n";
    for (const ProjectedHistoricalVar &risk : var.risks) {
      summary << risk.confidence << '\t' << risk.reference_value << '\t' << risk.value_at_risk
              << '\t' << risk.expected_shortfall << '\t' << risk.n_scenarios << '\t'
              << var.n_positions << '\t' << (static_cast<double>(legs.size()) / elapsed_seconds)
              << '\t' << var.prepared_fingerprint << '\t' << as_of_date << '\t' << as_of_ts_ns
              << '\t' << book_fingerprint << '\n';
    }
    summary.close();
    if (!summary)
      return Err(ErrorCode::IoError, "projected VaR: summary write failed");
    ATX_TRY_VOID(publish_projected_var_generation(run_dir));
  }
  // The as-of is on the console line for the same reason it is in the artifact:
  // the number is meaningless without the session whose book it describes.
  detail::log_emitf(LogLevel::Info, LogStream::Stdout,
                    "projected relative-template VaR complete: scenarios=%zu positions=%zu "
                    "as_of=%s book_fingerprint=%llu rate=%.1f/s",
                    frames.size(), var.n_positions, as_of_date.c_str(),
                    static_cast<unsigned long long>(book_fingerprint),
                    static_cast<double>(legs.size()) / elapsed_seconds);
  return Ok();
}

} // namespace atx::vol

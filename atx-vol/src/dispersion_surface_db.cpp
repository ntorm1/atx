#include "atx/vol/dispersion_surface_db.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
namespace fs = std::filesystem;

namespace {

// `read_run_spec`'s two number contracts (dispersion_workflow.cpp:29-38),
// restated here rather than called: those helpers live in that TU's ANONYMOUS
// namespace, so they have no external linkage and cannot be reused without
// promoting them to a shared header — a change to a file this task does not own.
// The contract is what matters and it is identical: `from_chars` must consume
// the WHOLE token, so "45x" and "" are rejected instead of parsing as 45, and a
// double must additionally be finite.
template <class T> bool parse_number(std::string_view text, T &value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

bool parse_double(std::string_view text, double &value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}

// The documented token tables. These are the ONLY spellings accepted; anything
// else is InvalidArgument rather than a silent fallback to the enum's zero.
constexpr std::pair<std::string_view, DispersionSide> kSides[] = {
    {"short_index_long_names", DispersionSide::ShortIndexLongNames},
    {"long_index_short_names", DispersionSide::LongIndexShortNames},
};
constexpr std::pair<std::string_view, WeightingScheme> kWeightings[] = {
    {"vega_neutral", WeightingScheme::VegaNeutral},
    {"equal_vega", WeightingScheme::EqualVega},
    {"gamma_neutral", WeightingScheme::GammaNeutral},
    {"theta_neutral", WeightingScheme::ThetaNeutral},
};
constexpr std::pair<std::string_view, StrikeRule> kStrikeRules[] = {
    {"atm_forward_straddle", StrikeRule::AtmForwardStraddle},
    {"fixed_moneyness", StrikeRule::FixedMoneyness},
    {"delta_strangle", StrikeRule::DeltaStrangle},
};
constexpr std::pair<std::string_view, HedgeSpec::Kind> kHedgeKinds[] = {
    {"none", HedgeSpec::Kind::None},
    {"delta_to_zero", HedgeSpec::Kind::DeltaToZero},
};
constexpr std::pair<std::string_view, HedgeSpec::Cadence> kHedgeCadences[] = {
    {"at_entry", HedgeSpec::Cadence::AtEntry},
    {"daily", HedgeSpec::Cadence::Daily},
};

template <class E, std::size_t N>
bool parse_enum(std::string_view text, const std::pair<std::string_view, E> (&table)[N], E &value) {
  for (const auto &[token, mapped] : table) {
    if (text == token) {
      value = mapped;
      return true;
    }
  }
  return false;
}

// Every rejection names the offending key, its value and the line, so a typo in
// a hand-authored config is self-diagnosing.
[[nodiscard]] std::string at(std::string_view key, std::string_view value, std::size_t line) {
  return "key '" + std::string(key) + "' value '" + std::string(value) + "' (line " +
         std::to_string(line) + ")";
}

} // namespace

Result<DispersionBacktestConfig> read_dispersion_backtest_config(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return Err(ErrorCode::NotFound, "cannot open dispersion config " + path.string());
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof())
    return Err(ErrorCode::IoError, "cannot read dispersion config " + path.string());

  DispersionBacktestConfig config{};
  std::size_t start = 0;
  std::size_t line_no = 0;
  while (start < text.size()) {
    const std::size_t eol = text.find('\n', start);
    std::string_view line{text.data() + start,
                          (eol == std::string::npos ? text.size() : eol) - start};
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    start = eol == std::string::npos ? text.size() : eol + 1;
    ++line_no;
    if (line.empty() || line.starts_with('#'))
      continue;

    // Split on the FIRST tab; the remainder is the value verbatim. A row that is
    // not key<TAB>value — including a present-but-empty value — is an authoring
    // error, never a silent skip.
    const std::size_t tab = line.find('\t');
    if (tab == std::string_view::npos || tab == 0 || tab + 1 >= line.size())
      return Err(ErrorCode::InvalidArgument, "dispersion config: line " + std::to_string(line_no) +
                                                 " is not a key<TAB>value row: '" +
                                                 std::string(line) + "'");
    const std::string_view key = line.substr(0, tab);
    const std::string_view value = line.substr(tab + 1);

    const auto number = [&](auto &field) -> Status {
      using Value = std::remove_reference_t<decltype(field)>;
      const bool parsed = [&] {
        if constexpr (std::is_same_v<Value, double>)
          return parse_double(value, field);
        else
          return parse_number(value, field);
      }();
      return parsed ? Ok()
                    : Err(ErrorCode::InvalidArgument,
                          "dispersion config: unparsable " + at(key, value, line_no));
    };
    const auto token = [&](auto &field, const auto &table) -> Status {
      return parse_enum(value, table, field)
                 ? Ok()
                 : Err(ErrorCode::InvalidArgument,
                       "dispersion config: unrecognized token for " + at(key, value, line_no));
    };

    Status assigned = Ok();
    if (key == "target_dte_days")
      assigned = number(config.target_dte_days);
    else if (key == "roll_dte_days")
      assigned = number(config.roll_dte_days);
    else if (key == "gross_index_vega")
      assigned = number(config.gross_index_vega);
    else if (key == "delta_band")
      assigned = number(config.delta_band);
    else if (key == "min_names")
      assigned = number(config.min_names);
    else if (key == "entry_every_n")
      assigned = number(config.entry_every_n);
    else if (key == "record_diagnostics") {
      unsigned flag = 0;
      assigned = number(flag);
      if (assigned)
        config.record_diagnostics = flag != 0;
    } else if (key == "multiplier")
      assigned = number(config.multiplier);
    else if (key == "side")
      assigned = token(config.side, kSides);
    else if (key == "weighting")
      assigned = token(config.weighting, kWeightings);
    else if (key == "strike_rule")
      assigned = token(config.strike.rule, kStrikeRules);
    else if (key == "log_moneyness")
      assigned = number(config.strike.log_moneyness);
    else if (key == "target_abs_delta")
      assigned = number(config.strike.target_abs_delta);
    else if (key == "hedge_kind")
      assigned = token(config.hedge_kind, kHedgeKinds);
    else if (key == "hedge_cadence")
      assigned = token(config.hedge_cadence, kHedgeCadences);
    else if (key == "half_spread_bps") {
      assigned = number(config.run.frictions.half_spread_bps);
      // See the header's SPREAD LANE note: the engine reads `half_spread_bps`
      // only under PriceBps, so authoring one without arming that lane would be
      // a knob that does nothing. Zero leaves the lane exactly as it was, which
      // keeps a frictionless config bit-identical to the default.
      if (assigned && config.run.frictions.half_spread_bps != 0.0 &&
          config.run.frictions.spread_kind == FrictionModel::SpreadKind::None)
        config.run.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
    } else if (key == "per_contract_cost")
      assigned = number(config.run.frictions.per_contract_cost);
    else if (key == "n_threads")
      assigned = number(config.run.price.n_threads);
    else if (key == "prefetch_depth")
      assigned = number(config.run.prefetch_depth);
    else
      return Err(ErrorCode::InvalidArgument, "dispersion config: unknown key '" + std::string(key) +
                                                 "' (line " + std::to_string(line_no) + ")");

    if (!assigned)
      return Err(assigned.error());
  }
  return config;
}

} // namespace atx::vol

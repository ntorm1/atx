#include "atx/vol/api/backtest/listed_dispersion_schedule.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/pricing/american.hpp"         // AmericanGreeks
#include "atx/vol/api/backtest/dispersion.hpp"       // contract_vega_per_vol_point (the ONE vol-point conversion)
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // kNsPerYear
#include "atx/vol/api/backtest/priced_surface.hpp"   // PricedSurface

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

constexpr std::string_view kMagic = "ATX_LISTED_DISPERSION_SCHEDULE\t1\n";
constexpr std::string_view kHeader =
    "roll_date\tvaluation_ts_ns\tcohort\texpiry_ts_ns\tgross_index_vega_target\t"
    "net_vega\tgross_vega\tn_names\tis_index\tsymbol\tuid\tinstrument_id\t"
    "raw_symbol\tstrike\tside\tquantity\tmultiplier\traw_bid\traw_ask\traw_mid\t"
    "model_mark\tdelta_per_share\tvega_per_unit_vol\tvega_per_contract_per_vol_point\t"
    "normalized_weight\ttarget_straddle_vega\tachieved_leg_vega\tsource_fingerprint\t"
    "surface_fingerprint\n";

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }
[[nodiscard]] bool finite_positive(double value) noexcept { return finite(value) && value > 0.0; }

[[nodiscard]] double quote_mid(const ListedOptionQuote &quote) noexcept {
  return 0.5 * (quote.bid + quote.ask);
}

void append_u64(std::string &out, std::uint64_t value) {
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, value);
  (void)ec;
  out.append(buf, ptr);
}

void append_i64(std::string &out, std::int64_t value) {
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, value);
  (void)ec;
  out.append(buf, ptr);
}

void append_double(std::string &out, double value) {
  char buf[64];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, value);
  (void)ec;
  out.append(buf, ptr);
}

[[nodiscard]] bool clean_text(std::string_view value) noexcept {
  return value.find_first_of("\t\r\n") == std::string_view::npos;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delim) {
  std::vector<std::string_view> out;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t end = text.find(delim, begin);
    if (end == std::string_view::npos) {
      out.push_back(text.substr(begin));
      break;
    }
    out.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return out;
}

template <class T> [[nodiscard]] bool parse_integer(std::string_view text, T &out) noexcept {
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc{} && ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_double(std::string_view text, double &out) noexcept {
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc{} && ptr == text.data() + text.size() && finite(out);
}

[[nodiscard]] Result<ListedOptionRisk> surface_risk(const ListedDispersionSelection &selection,
                                                    const SurfaceSet &surfaces, std::uint32_t uid,
                                                    const ListedOptionQuote &quote) {
  const SurfaceRef surface = surfaces.find(uid);
  if (surface == nullptr) {
    return Err(ErrorCode::NotFound,
               "build_listed_dispersion_roll: selected surface is unavailable");
  }
  const double T = static_cast<double>(quote.expiry_ts_ns - selection.valuation_ts_ns) / kNsPerYear;
  if (!finite_positive(T)) {
    return Err(ErrorCode::InvalidArgument,
               "build_listed_dispersion_roll: selected contract has expired");
  }
  using Field = PricedSurface::EvalField;
  const PricedSurface::FusedResult evaluated = surface->evaluate(
      quote.strike, T, quote.side, Field::Price | Field::Delta | Field::Vega, false);
  if (!evaluated.status.has_value()) {
    return Err(evaluated.status.error());
  }
  return Ok(ListedOptionRisk{evaluated.price, evaluated.greeks.delta, evaluated.greeks.vega});
}

struct StraddleRisk {
  ListedOptionRisk call{};
  ListedOptionRisk put{};
  double vega_per_contract_per_vol_point{0.0};
};

[[nodiscard]] Result<StraddleRisk> lookup_straddle_risk(const ListedStraddle &straddle,
                                                        const ListedRiskLookup &lookup) {
  ATX_TRY(ListedOptionRisk call, lookup(straddle.uid, straddle.call));
  ATX_TRY(ListedOptionRisk put, lookup(straddle.uid, straddle.put));
  if (!finite(call.model_mark) || call.model_mark < 0.0 || !finite(call.delta_per_share) ||
      !finite_positive(call.vega_per_unit_vol) || !finite(put.model_mark) || put.model_mark < 0.0 ||
      !finite(put.delta_per_share) || !finite_positive(put.vega_per_unit_vol)) {
    return Err(ErrorCode::Unavailable,
               "build_listed_dispersion_roll: invalid selected contract risk");
  }
  if (straddle.call.multiplier != straddle.put.multiplier ||
      !finite_positive(straddle.call.multiplier)) {
    return Err(ErrorCode::InvalidArgument,
               "build_listed_dispersion_roll: straddle multiplier mismatch");
  }
  const double vega = contract_vega_per_vol_point(
      call.vega_per_unit_vol + put.vega_per_unit_vol, straddle.call.multiplier);
  if (!finite_positive(vega)) {
    return Err(ErrorCode::Unavailable, "build_listed_dispersion_roll: nonpositive straddle vega");
  }
  return Ok(StraddleRisk{call, put, vega});
}

void append_leg(ListedScheduleRoll &roll, const ListedStraddle &straddle, const StraddleRisk &risk,
                bool is_index, double quantity, double target, std::uint64_t surface_fingerprint) {
  const auto add = [&](const ListedOptionQuote &quote, const ListedOptionRisk &option) {
    ListedScheduleLeg leg;
    leg.roll_date = roll.roll_date;
    leg.cohort = roll.cohort;
    leg.is_index = is_index;
    leg.symbol = straddle.symbol;
    leg.uid = straddle.uid;
    leg.instrument_id = quote.instrument_id;
    leg.raw_symbol = quote.raw_symbol;
    leg.expiry_ts_ns = quote.expiry_ts_ns;
    leg.strike = quote.strike;
    leg.side = quote.side;
    leg.quantity = quantity;
    leg.multiplier = quote.multiplier;
    leg.raw_bid = quote.bid;
    leg.raw_ask = quote.ask;
    leg.raw_mid = quote_mid(quote);
    leg.model_mark = option.model_mark;
    leg.delta_per_share = option.delta_per_share;
    leg.vega_per_unit_vol = option.vega_per_unit_vol;
    leg.vega_per_contract_per_vol_point =
        contract_vega_per_vol_point(option.vega_per_unit_vol, quote.multiplier);
    leg.normalized_weight = straddle.normalized_weight;
    leg.target_straddle_vega_per_vol_point = target;
    leg.achieved_leg_vega_per_vol_point = quantity * leg.vega_per_contract_per_vol_point;
    leg.source_fingerprint = quote.source_fingerprint;
    leg.surface_fingerprint = surface_fingerprint;
    roll.legs.push_back(std::move(leg));
  };
  add(straddle.call, risk.call);
  add(straddle.put, risk.put);
}

[[nodiscard]] Result<void> validate_roll(const ListedScheduleRoll &roll,
                                         double max_relative_residual) {
  if (roll.legs.size() != 2u * (1u + roll.n_names) ||
      !finite_positive(roll.gross_index_vega_target_per_vol_point) ||
      !finite(roll.net_vega_per_vol_point) || !finite_positive(roll.gross_vega_per_vol_point)) {
    return Err(ErrorCode::ParseError, "listed schedule: invalid roll shape or totals");
  }
  double net = 0.0;
  double gross = 0.0;
  double normalized_weight = 0.0;
  double basket_target = 0.0;
  std::set<std::tuple<std::string, std::uint32_t, std::string>> source_keys;
  for (const ListedScheduleLeg &leg : roll.legs) {
    if (leg.roll_date != roll.roll_date || leg.cohort != roll.cohort ||
        leg.expiry_ts_ns != roll.expiry_ts_ns || leg.symbol.empty() || leg.uid == 0 ||
        leg.instrument_id == 0 || leg.raw_symbol.empty() || !finite_positive(leg.strike) ||
        !finite(leg.quantity) || leg.quantity == 0.0 || !finite_positive(leg.multiplier) ||
        !finite(leg.raw_bid) || leg.raw_bid < 0.0 || !finite_positive(leg.raw_ask) ||
        leg.raw_ask < leg.raw_bid || leg.raw_mid != 0.5 * (leg.raw_bid + leg.raw_ask) ||
        !finite(leg.model_mark) || leg.model_mark < 0.0 || !finite(leg.delta_per_share) ||
        !finite_positive(leg.vega_per_unit_vol) ||
        !finite_positive(leg.vega_per_contract_per_vol_point) ||
        leg.vega_per_contract_per_vol_point !=
            contract_vega_per_vol_point(leg.vega_per_unit_vol, leg.multiplier) ||
        !finite(leg.normalized_weight) || !finite(leg.target_straddle_vega_per_vol_point) ||
        !finite(leg.achieved_leg_vega_per_vol_point) ||
        leg.achieved_leg_vega_per_vol_point != leg.quantity * leg.vega_per_contract_per_vol_point) {
      return Err(ErrorCode::ParseError, "listed schedule: invalid leg economics");
    }
    net += leg.achieved_leg_vega_per_vol_point;
    gross += std::fabs(leg.achieved_leg_vega_per_vol_point);
    if (!source_keys.emplace(leg.roll_date, leg.instrument_id, leg.raw_symbol).second) {
      return Err(ErrorCode::ParseError, "listed schedule: duplicate source contract key");
    }
  }

  std::set<std::string> name_symbols;
  for (std::size_t i = 0; i < roll.legs.size(); i += 2u) {
    const ListedScheduleLeg &call = roll.legs[i];
    const ListedScheduleLeg &put = roll.legs[i + 1u];
    if (call.side != Side::Call || put.side != Side::Put || call.is_index != put.is_index ||
        call.symbol != put.symbol || call.uid != put.uid || call.expiry_ts_ns != put.expiry_ts_ns ||
        call.strike != put.strike || call.quantity != put.quantity ||
        call.multiplier != put.multiplier || call.normalized_weight != put.normalized_weight ||
        call.target_straddle_vega_per_vol_point != put.target_straddle_vega_per_vol_point ||
        (i == 0u) != call.is_index) {
      return Err(ErrorCode::ParseError, "listed schedule: invalid straddle pair/order");
    }
    const double achieved =
        call.achieved_leg_vega_per_vol_point + put.achieved_leg_vega_per_vol_point;
    const double pair_scale = std::max(1.0, std::fabs(call.target_straddle_vega_per_vol_point));
    if (std::fabs(achieved - call.target_straddle_vega_per_vol_point) >
        max_relative_residual * pair_scale) {
      return Err(ErrorCode::Unavailable, "listed schedule: straddle target vega mismatch");
    }
    if (i == 0u) {
      if (call.normalized_weight != 0.0 || std::fabs(call.target_straddle_vega_per_vol_point) !=
                                               roll.gross_index_vega_target_per_vol_point) {
        return Err(ErrorCode::ParseError, "listed schedule: invalid index target");
      }
    } else {
      if (!finite_positive(call.normalized_weight) || !name_symbols.insert(call.symbol).second) {
        return Err(ErrorCode::ParseError, "listed schedule: invalid name weight/key");
      }
      normalized_weight += call.normalized_weight;
      basket_target += call.target_straddle_vega_per_vol_point;
    }
  }
  if (name_symbols.size() != roll.n_names ||
      std::fabs(normalized_weight - 1.0) > max_relative_residual ||
      std::fabs(basket_target + roll.legs.front().target_straddle_vega_per_vol_point) >
          max_relative_residual * roll.gross_index_vega_target_per_vol_point) {
    return Err(ErrorCode::ParseError, "listed schedule: invalid basket weights/target");
  }
  if (net != roll.net_vega_per_vol_point || gross != roll.gross_vega_per_vol_point) {
    return Err(ErrorCode::ParseError, "listed schedule: persisted vega totals disagree");
  }
  const double relative = std::fabs(net) / roll.gross_index_vega_target_per_vol_point;
  if (!finite(max_relative_residual) || max_relative_residual < 0.0 ||
      relative > max_relative_residual) {
    return Err(ErrorCode::Unavailable, "listed schedule: vega residual exceeds tolerance");
  }
  return Ok();
}

} // namespace

Result<ListedScheduleRoll> build_listed_dispersion_roll(const ListedDispersionSelection &selection,
                                                        const ListedRiskLookup &risk_lookup,
                                                        const ListedScheduleBuildConfig &config) {
  if (!risk_lookup || selection.trade_date.empty() || selection.valuation_ts_ns <= 0 ||
      selection.expiry_ts_ns <= selection.valuation_ts_ns || selection.names.empty() ||
      !finite_positive(config.gross_index_vega_target_per_vol_point) ||
      !finite(config.max_relative_vega_residual) || config.max_relative_vega_residual < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "build_listed_dispersion_roll: invalid selection or config");
  }

  ATX_TRY(StraddleRisk index_risk, lookup_straddle_risk(selection.index, risk_lookup));
  const double index_sign = config.side == DispersionSide::ShortIndexLongNames ? -1.0 : 1.0;
  const double name_sign = -index_sign;

  ListedScheduleRoll roll;
  roll.roll_date = selection.trade_date;
  roll.valuation_ts_ns = selection.valuation_ts_ns;
  roll.cohort = config.cohort;
  roll.expiry_ts_ns = selection.expiry_ts_ns;
  roll.gross_index_vega_target_per_vol_point = config.gross_index_vega_target_per_vol_point;
  roll.n_names = static_cast<std::uint32_t>(selection.names.size());
  roll.legs.reserve(2u * (1u + selection.names.size()));

  const double index_target = index_sign * config.gross_index_vega_target_per_vol_point;
  const double index_quantity = index_target / index_risk.vega_per_contract_per_vol_point;
  append_leg(roll, selection.index, index_risk, true, index_quantity, index_target,
             config.surface_fingerprint);

  for (const ListedStraddle &name : selection.names) {
    if (!finite_positive(name.normalized_weight)) {
      return Err(ErrorCode::InvalidArgument,
                 "build_listed_dispersion_roll: invalid normalized weight");
    }
    ATX_TRY(StraddleRisk risk, lookup_straddle_risk(name, risk_lookup));
    const double target =
        name_sign * name.normalized_weight * config.gross_index_vega_target_per_vol_point;
    const double quantity = target / risk.vega_per_contract_per_vol_point;
    append_leg(roll, name, risk, false, quantity, target, config.surface_fingerprint);
  }

  for (const ListedScheduleLeg &leg : roll.legs) {
    roll.net_vega_per_vol_point += leg.achieved_leg_vega_per_vol_point;
    roll.gross_vega_per_vol_point += std::fabs(leg.achieved_leg_vega_per_vol_point);
  }
  ATX_TRY_VOID(validate_roll(roll, config.max_relative_vega_residual));
  return Ok(std::move(roll));
}

Result<ListedScheduleRoll> build_listed_dispersion_roll(const ListedDispersionSelection &selection,
                                                        const SurfaceSet &surfaces,
                                                        const ListedScheduleBuildConfig &config) {
  const ListedRiskLookup lookup = [&](std::uint32_t uid, const ListedOptionQuote &quote) {
    return surface_risk(selection, surfaces, uid, quote);
  };
  return build_listed_dispersion_roll(selection, lookup, config);
}

Result<std::string> serialize_listed_dispersion_schedule(const ListedDispersionSchedule &schedule) {
  ATX_TRY_VOID(validate_listed_dispersion_schedule(schedule));
  std::string out;
  out.reserve(1024u + schedule.rolls.size() * 4096u);
  out.append(kMagic);
  out.append(kHeader);
  std::string previous_date;
  std::uint32_t previous_cohort = 0;
  bool first = true;
  for (const ListedScheduleRoll &roll : schedule.rolls) {
    if (!clean_text(roll.roll_date) || (!first && std::tie(previous_date, previous_cohort) >=
                                                      std::tie(roll.roll_date, roll.cohort))) {
      return Err(ErrorCode::InvalidArgument,
                 "serialize_listed_dispersion_schedule: rolls not strictly ordered");
    }
    previous_date = roll.roll_date;
    previous_cohort = roll.cohort;
    first = false;
    for (const ListedScheduleLeg &leg : roll.legs) {
      if (!clean_text(leg.symbol) || !clean_text(leg.raw_symbol)) {
        return Err(ErrorCode::InvalidArgument,
                   "serialize_listed_dispersion_schedule: invalid text field");
      }
      out.append(roll.roll_date);
      out.push_back('\t');
      append_i64(out, roll.valuation_ts_ns);
      out.push_back('\t');
      append_u64(out, roll.cohort);
      out.push_back('\t');
      append_i64(out, roll.expiry_ts_ns);
      out.push_back('\t');
      append_double(out, roll.gross_index_vega_target_per_vol_point);
      out.push_back('\t');
      append_double(out, roll.net_vega_per_vol_point);
      out.push_back('\t');
      append_double(out, roll.gross_vega_per_vol_point);
      out.push_back('\t');
      append_u64(out, roll.n_names);
      out.push_back('\t');
      out.push_back(leg.is_index ? '1' : '0');
      out.push_back('\t');
      out.append(leg.symbol);
      out.push_back('\t');
      append_u64(out, leg.uid);
      out.push_back('\t');
      append_u64(out, leg.instrument_id);
      out.push_back('\t');
      out.append(leg.raw_symbol);
      out.push_back('\t');
      append_double(out, leg.strike);
      out.push_back('\t');
      out.push_back(leg.side == Side::Call ? 'C' : 'P');
      out.push_back('\t');
      append_double(out, leg.quantity);
      out.push_back('\t');
      append_double(out, leg.multiplier);
      out.push_back('\t');
      append_double(out, leg.raw_bid);
      out.push_back('\t');
      append_double(out, leg.raw_ask);
      out.push_back('\t');
      append_double(out, leg.raw_mid);
      out.push_back('\t');
      append_double(out, leg.model_mark);
      out.push_back('\t');
      append_double(out, leg.delta_per_share);
      out.push_back('\t');
      append_double(out, leg.vega_per_unit_vol);
      out.push_back('\t');
      append_double(out, leg.vega_per_contract_per_vol_point);
      out.push_back('\t');
      append_double(out, leg.normalized_weight);
      out.push_back('\t');
      append_double(out, leg.target_straddle_vega_per_vol_point);
      out.push_back('\t');
      append_double(out, leg.achieved_leg_vega_per_vol_point);
      out.push_back('\t');
      append_u64(out, leg.source_fingerprint);
      out.push_back('\t');
      append_u64(out, leg.surface_fingerprint);
      out.push_back('\n');
    }
  }
  return Ok(std::move(out));
}

Result<ListedDispersionSchedule> parse_listed_dispersion_schedule(std::string_view tsv) {
  if (!tsv.starts_with(kMagic)) {
    return Err(ErrorCode::ParseError, "listed schedule: bad magic/version");
  }
  tsv.remove_prefix(kMagic.size());
  if (!tsv.starts_with(kHeader)) {
    return Err(ErrorCode::ParseError, "listed schedule: bad header");
  }
  tsv.remove_prefix(kHeader.size());

  ListedDispersionSchedule schedule;
  while (!tsv.empty()) {
    const std::size_t newline = tsv.find('\n');
    if (newline == std::string_view::npos) {
      return Err(ErrorCode::ParseError, "listed schedule: unterminated row");
    }
    const std::string_view line = tsv.substr(0, newline);
    tsv.remove_prefix(newline + 1);
    if (line.empty()) {
      return Err(ErrorCode::ParseError, "listed schedule: empty row");
    }
    const std::vector<std::string_view> f = split(line, '\t');
    if (f.size() != 29u) {
      return Err(ErrorCode::ParseError, "listed schedule: wrong column count");
    }

    std::int64_t valuation = 0;
    std::uint32_t cohort = 0;
    std::int64_t expiry = 0;
    double target = 0.0;
    double net = 0.0;
    double gross = 0.0;
    std::uint32_t n_names = 0;
    if (!parse_integer(f[1], valuation) || !parse_integer(f[2], cohort) ||
        !parse_integer(f[3], expiry) || !parse_double(f[4], target) || !parse_double(f[5], net) ||
        !parse_double(f[6], gross) || !parse_integer(f[7], n_names)) {
      return Err(ErrorCode::ParseError, "listed schedule: invalid roll field");
    }

    const bool new_roll = schedule.rolls.empty() || schedule.rolls.back().roll_date != f[0] ||
                          schedule.rolls.back().cohort != cohort;
    if (new_roll) {
      if (!schedule.rolls.empty() &&
          std::tie(schedule.rolls.back().roll_date, schedule.rolls.back().cohort) >=
              std::tie(f[0], cohort)) {
        return Err(ErrorCode::ParseError, "listed schedule: rows not ordered");
      }
      ListedScheduleRoll roll;
      roll.roll_date = std::string{f[0]};
      roll.valuation_ts_ns = valuation;
      roll.cohort = cohort;
      roll.expiry_ts_ns = expiry;
      roll.gross_index_vega_target_per_vol_point = target;
      roll.net_vega_per_vol_point = net;
      roll.gross_vega_per_vol_point = gross;
      roll.n_names = n_names;
      schedule.rolls.push_back(std::move(roll));
    } else {
      const ListedScheduleRoll &roll = schedule.rolls.back();
      if (roll.valuation_ts_ns != valuation || roll.expiry_ts_ns != expiry ||
          roll.gross_index_vega_target_per_vol_point != target ||
          roll.net_vega_per_vol_point != net || roll.gross_vega_per_vol_point != gross ||
          roll.n_names != n_names) {
        return Err(ErrorCode::ParseError, "listed schedule: inconsistent roll fields");
      }
    }

    ListedScheduleLeg leg;
    leg.roll_date = std::string{f[0]};
    leg.cohort = cohort;
    if (f[8] == "1")
      leg.is_index = true;
    else if (f[8] == "0")
      leg.is_index = false;
    else
      return Err(ErrorCode::ParseError, "listed schedule: invalid index flag");
    leg.symbol = std::string{f[9]};
    leg.raw_symbol = std::string{f[12]};
    if (!parse_integer(f[10], leg.uid) || !parse_integer(f[11], leg.instrument_id) ||
        !parse_double(f[13], leg.strike) || !parse_double(f[15], leg.quantity) ||
        !parse_double(f[16], leg.multiplier) || !parse_double(f[17], leg.raw_bid) ||
        !parse_double(f[18], leg.raw_ask) || !parse_double(f[19], leg.raw_mid) ||
        !parse_double(f[20], leg.model_mark) || !parse_double(f[21], leg.delta_per_share) ||
        !parse_double(f[22], leg.vega_per_unit_vol) ||
        !parse_double(f[23], leg.vega_per_contract_per_vol_point) ||
        !parse_double(f[24], leg.normalized_weight) ||
        !parse_double(f[25], leg.target_straddle_vega_per_vol_point) ||
        !parse_double(f[26], leg.achieved_leg_vega_per_vol_point) ||
        !parse_integer(f[27], leg.source_fingerprint) ||
        !parse_integer(f[28], leg.surface_fingerprint)) {
      return Err(ErrorCode::ParseError, "listed schedule: invalid leg field");
    }
    leg.expiry_ts_ns = expiry;
    if (f[14] == "C")
      leg.side = Side::Call;
    else if (f[14] == "P")
      leg.side = Side::Put;
    else
      return Err(ErrorCode::ParseError, "listed schedule: invalid side");
    schedule.rolls.back().legs.push_back(std::move(leg));
  }

  ATX_TRY_VOID(validate_listed_dispersion_schedule(schedule));
  return Ok(std::move(schedule));
}

Status validate_listed_dispersion_schedule(const ListedDispersionSchedule &schedule,
                                           double max_relative_vega_residual) {
  if (schedule.rolls.empty() || !finite(max_relative_vega_residual) ||
      max_relative_vega_residual < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "validate_listed_dispersion_schedule: empty schedule or invalid tolerance");
  }
  std::string previous_date;
  std::uint32_t previous_cohort = 0;
  bool first = true;
  for (const ListedScheduleRoll &roll : schedule.rolls) {
    if (roll.roll_date.empty() || roll.valuation_ts_ns <= 0 ||
        roll.expiry_ts_ns <= roll.valuation_ts_ns ||
        (!first &&
         std::tie(previous_date, previous_cohort) >= std::tie(roll.roll_date, roll.cohort))) {
      return Err(ErrorCode::InvalidArgument,
                 "validate_listed_dispersion_schedule: rolls not strictly ordered");
    }
    ATX_TRY_VOID(validate_roll(roll, max_relative_vega_residual));
    previous_date = roll.roll_date;
    previous_cohort = roll.cohort;
    first = false;
  }
  return Ok();
}

Status write_listed_dispersion_schedule_file(std::string_view path,
                                             const ListedDispersionSchedule &schedule) {
  if (path.empty()) {
    return Err(ErrorCode::InvalidArgument, "write_listed_dispersion_schedule_file: empty path");
  }
  ATX_TRY(std::string text, serialize_listed_dispersion_schedule(schedule));
  const std::filesystem::path target{path};
  std::error_code ec;
  if (!target.parent_path().empty()) {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "write_listed_dispersion_schedule_file: cannot create parent");
    }
  }
  const std::filesystem::path pending = target.string() + ".pending";
  {
    std::ofstream out(pending, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Err(ErrorCode::IoError,
                 "write_listed_dispersion_schedule_file: cannot open pending file");
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
      return Err(ErrorCode::IoError, "write_listed_dispersion_schedule_file: write failed");
    }
  }
  if (std::filesystem::exists(target, ec) && !ec) {
    std::filesystem::remove(target, ec);
    if (ec) {
      std::filesystem::remove(pending, ec);
      return Err(ErrorCode::IoError,
                 "write_listed_dispersion_schedule_file: cannot replace target");
    }
  }
  std::filesystem::rename(pending, target, ec);
  if (ec) {
    std::filesystem::remove(pending, ec);
    return Err(ErrorCode::IoError, "write_listed_dispersion_schedule_file: commit failed");
  }
  return Ok();
}

Result<ListedDispersionSchedule> read_listed_dispersion_schedule_file(std::string_view path) {
  const std::filesystem::path source{path};
  std::error_code ec;
  if (!std::filesystem::exists(source, ec) || ec) {
    return Err(ErrorCode::NotFound, "read_listed_dispersion_schedule_file: file not found");
  }
  std::ifstream in(source, std::ios::binary | std::ios::ate);
  if (!in) {
    return Err(ErrorCode::IoError, "read_listed_dispersion_schedule_file: cannot open file");
  }
  const std::streampos size = in.tellg();
  if (size < 0) {
    return Err(ErrorCode::IoError, "read_listed_dispersion_schedule_file: cannot size file");
  }
  std::string text(static_cast<std::size_t>(size), '\0');
  in.seekg(0);
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!in && !text.empty()) {
    return Err(ErrorCode::IoError, "read_listed_dispersion_schedule_file: read failed");
  }
  return parse_listed_dispersion_schedule(text);
}

} // namespace atx::vol

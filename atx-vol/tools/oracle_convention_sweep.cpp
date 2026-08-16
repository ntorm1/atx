#include "oracle_convention_sweep.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol::oracle {

using atx::core::Ok;

namespace {

constexpr std::array<InputModel, 8> kInputModels = {
    InputModel::CurrentSpotSdivYield,      InputModel::DiscreteDividendPvSdivYield,
    InputModel::DiscreteForwardNetCarry,   InputModel::DiscreteForwardRateSdivYield,
    InputModel::DiscreteForwardNetRate,    InputModel::DiscreteForwardZeroRates,
    InputModel::DiscreteDividendPvNetRate, InputModel::DiscreteDividendPvRatePlusSdiv,
};

// The staged search evaluates the CLOSED candidate set, so the array and the
// enum must not drift apart. InputModel is default-numbered from 0, so the last
// enumerator's value pins the enum's cardinality.
static_assert(static_cast<std::size_t>(InputModel::DiscreteDividendPvRatePlusSdiv) + 1 ==
                  kInputModels.size(),
              "kInputModels must enumerate every InputModel");

[[nodiscard]] constexpr bool enumerates_every_input_model() noexcept {
  for (std::size_t index = 0; index < kInputModels.size(); ++index) {
    const auto wanted = static_cast<InputModel>(index);
    bool found = false;
    for (const InputModel model : kInputModels) {
      found = found || model == wanted;
    }
    if (!found) {
      return false;
    }
  }
  return true;
}
static_assert(enumerates_every_input_model(), "kInputModels must list each InputModel exactly once");

// The second stage always ranks exactly two finalists.
constexpr std::size_t kFinalistCount = 2;
static_assert(kInputModels.size() >= kFinalistCount, "the smoke cut needs two survivors");

constexpr std::array<double, 6> kUnitScales = {0.01, -0.01, 1.0, -1.0, 100.0, -100.0};
constexpr std::array<double, 6> kPointScales = {0.0001, -0.0001, 0.01, -0.01, 1.0, -1.0};
constexpr std::array<double, 10> kTimeScales = {
    1.0 / 365.0,  -1.0 / 365.0, 1.0 / 365.25, -1.0 / 365.25, 1.0 / 360.0,
    -1.0 / 360.0, 1.0 / 252.0,  -1.0 / 252.0, 1.0,           -1.0};

struct PriceCandidate {
  InputModel model{};
  Accumulator smoke;
  Accumulator tune;
};

// The price/share search has no Greek source to pick, so it carries none: a
// filler GreekSource would degenerate into a constant tie-break prefix.
struct PriceScaleCandidate {
  double scale = 1.0;
  Accumulator error;
};

constexpr std::size_t kGreekCount = 9;

// One table binds each relative Greek's metric id, its SpiderRock oracle
// column, and its baseline-arm member together, in REPORTED metric order. Nine
// hand-matched call sites is precisely the transposition class this file
// already guards against elsewhere; the winner attribution and the greek-aware
// finalist ranking both index this table, so the two can never disagree about
// which Greek is which.
struct GreekSpec {
  std::string_view metric_id;
  double OracleRow::*oracle;
  double OracleUnitGreeks::*baseline;
};
constexpr std::array<GreekSpec, kGreekCount> kGreekSpecs = {
    GreekSpec{"mode_a_delta_rel", &OracleRow::de, &OracleUnitGreeks::de},
    GreekSpec{"mode_a_gamma_rel", &OracleRow::ga, &OracleUnitGreeks::ga},
    GreekSpec{"mode_a_theta_rel", &OracleRow::th, &OracleUnitGreeks::th},
    GreekSpec{"mode_a_vega_rel", &OracleRow::ve, &OracleUnitGreeks::ve},
    GreekSpec{"mode_a_rho_rel", &OracleRow::rh, &OracleUnitGreeks::rh},
    GreekSpec{"mode_a_phi_rel", &OracleRow::ph, &OracleUnitGreeks::ph},
    GreekSpec{"mode_a_volga_rel", &OracleRow::vo, &OracleUnitGreeks::vo},
    GreekSpec{"mode_a_vanna_rel", &OracleRow::va, &OracleUnitGreeks::va},
    GreekSpec{"mode_a_delta_decay_rel", &OracleRow::de_decay, &OracleUnitGreeks::de_decay},
};

// Positions in kGreekSpecs. The winner map assigns each search's pick to its
// own ConventionMap field, so the positions must be named rather than counted,
// and each name is checked against the id it claims.
constexpr std::size_t kDeltaSearch = 0;
constexpr std::size_t kGammaSearch = 1;
constexpr std::size_t kThetaSearch = 2;
constexpr std::size_t kVegaSearch = 3;
constexpr std::size_t kRhoSearch = 4;
constexpr std::size_t kPhiSearch = 5;
constexpr std::size_t kVolgaSearch = 6;
constexpr std::size_t kVannaSearch = 7;
constexpr std::size_t kDecaySearch = 8;
static_assert(kGreekSpecs[kDeltaSearch].metric_id == "mode_a_delta_rel");
static_assert(kGreekSpecs[kGammaSearch].metric_id == "mode_a_gamma_rel");
static_assert(kGreekSpecs[kThetaSearch].metric_id == "mode_a_theta_rel");
static_assert(kGreekSpecs[kVegaSearch].metric_id == "mode_a_vega_rel");
static_assert(kGreekSpecs[kRhoSearch].metric_id == "mode_a_rho_rel");
static_assert(kGreekSpecs[kPhiSearch].metric_id == "mode_a_phi_rel");
static_assert(kGreekSpecs[kVolgaSearch].metric_id == "mode_a_volga_rel");
static_assert(kGreekSpecs[kVannaSearch].metric_id == "mode_a_vanna_rel");
static_assert(kGreekSpecs[kDecaySearch].metric_id == "mode_a_delta_decay_rel");

using GreekSearches = std::array<std::vector<ScaleCandidate>, kGreekCount>;

// Absolute floors (price, vol) have no relative denominator, so the reported
// population IS the selection population.
struct BaselineFloors {
  Accumulator price;
  Accumulator vol;
  std::array<FloorAccumulators, kGreekCount> greeks;
};

// Which cohort a price candidate is ranked on. Smoke decides the 8-way cut;
// the two finalists are then ranked on the tune sample ALONE, so smoke evidence
// is never counted a second time under a ~14%-weighted tune sample.
enum class PriceStage { Smoke, TuneSample };

[[nodiscard]] bool less_price(const PriceCandidate &left, const PriceCandidate &right,
                              PriceStage stage) noexcept {
  const Accumulator &left_acc = stage == PriceStage::Smoke ? left.smoke : left.tune;
  const Accumulator &right_acc = stage == PriceStage::Smoke ? right.smoke : right.tune;
  const double left_mean = left_acc.mean();
  const double right_mean = right_acc.mean();
  if (left_mean != right_mean) {
    return left_mean < right_mean;
  }
  return input_model_id(left.model) < input_model_id(right.model);
}

void evaluate_price_rows(std::span<const OracleRow> rows, std::size_t stride,
                         PriceCandidate &candidate, PriceStage stage) {
  ConventionMap map = baseline_convention();
  map.input_model = candidate.model;
  Accumulator &acc = stage == PriceStage::Smoke ? candidate.smoke : candidate.tune;
  for (std::size_t index = 0; index < rows.size(); index += stride) {
    const OracleRow &row = rows[index];
    const EnginePricingInputs in = mode_a_inputs(row, map);
    const auto price = andersen_lake(in.spot, in.strike, in.years, in.sigma, in.rate, in.carry,
                                     in.side, al_fast_opts());
    if (price.has_value()) {
      acc.absolute(*price, row.sr_prc);
    }
  }
}

[[nodiscard]] std::vector<ScaleCandidate> scales_for(GreekSource source,
                                                     std::span<const double> scales) {
  std::vector<ScaleCandidate> out;
  out.reserve(scales.size());
  for (const double scale : scales) {
    out.push_back(ScaleCandidate{source, scale, {}});
  }
  return out;
}

[[nodiscard]] std::vector<ScaleCandidate> source_scales(std::span<const GreekSource> sources,
                                                        std::span<const double> scales) {
  std::vector<ScaleCandidate> out;
  out.reserve(sources.size() * scales.size());
  for (const GreekSource source : sources) {
    for (const double scale : scales) {
      out.push_back(ScaleCandidate{source, scale, {}});
    }
  }
  return out;
}

// ONE definition of which bounded grid each Greek searches, shared by the
// winner attribution and the stage-2 finalist ranking. Two copies could drift,
// and a finalist ranked on a grid the winner is not later scored on would
// disagree with the published floor by construction.
[[nodiscard]] GreekSearches make_greek_searches() {
  constexpr std::array<GreekSource, 2> kSecondOrder = {GreekSource::Volga, GreekSource::Vanna};
  return GreekSearches{{
      scales_for(GreekSource::Delta, kUnitScales),
      scales_for(GreekSource::Gamma, kUnitScales),
      scales_for(GreekSource::Theta, kTimeScales),
      scales_for(GreekSource::Vega, kPointScales),
      scales_for(GreekSource::Rho, kPointScales),
      scales_for(GreekSource::CarryRho, kPointScales),
      source_scales(kSecondOrder, kPointScales),
      source_scales(kSecondOrder, kPointScales),
      scales_for(GreekSource::Charm, kTimeScales),
  }};
}

[[nodiscard]] std::vector<PriceScaleCandidate> price_scales_for(std::span<const double> scales) {
  std::vector<PriceScaleCandidate> out;
  out.reserve(scales.size());
  for (const double scale : scales) {
    out.push_back(PriceScaleCandidate{scale, {}});
  }
  return out;
}

[[nodiscard]] double source_value(const AmericanGreeks &g, double dp_dq,
                                  GreekSource source) noexcept {
  switch (source) {
  case GreekSource::Delta:
    return g.delta;
  case GreekSource::Gamma:
    return g.gamma;
  case GreekSource::Theta:
    return g.theta;
  case GreekSource::Vega:
    return g.vega;
  case GreekSource::Rho:
    return g.rho;
  case GreekSource::CarryRho:
    return dp_dq;
  case GreekSource::Volga:
    return g.volga;
  case GreekSource::Vanna:
    return g.vanna;
  case GreekSource::Charm:
    return g.charm;
  }
  assert(false);
  return std::numeric_limits<double>::quiet_NaN();
}

// One engine evaluation of a row under one convention map.
struct PricedRow {
  EnginePricingInputs inputs{};
  AmericanGreeks greeks{};
  double dp_dq = std::numeric_limits<double>::quiet_NaN();
};

// A failed carry solve leaves dp_dq non-finite (only the phi metric reads it)
// rather than discarding the row's other eight Greeks.
[[nodiscard]] std::optional<PricedRow> price_row(const OracleRow &row, const ConventionMap &map) {
  const EnginePricingInputs in = mode_a_inputs(row, map);
  const auto greeks = american_greeks_al(in.spot, in.strike, in.years, in.sigma, in.rate, in.carry,
                                         in.side, al_fast_opts());
  if (!greeks.has_value()) {
    return std::nullopt;
  }
  PricedRow out{.inputs = in, .greeks = *greeks, .dp_dq = std::numeric_limits<double>::quiet_NaN()};
  const auto carry = american_carry_greeks_al(in.spot, in.strike, in.years, in.sigma, in.rate,
                                              in.carry, in.side, al_fast_opts());
  if (carry.has_value()) {
    out.dp_dq = carry->dP_dq;
  }
  return out;
}

// Every scale in one search must be scored on the same rows, so a search is
// admitted only when every source it may pick is finite on the winner arm.
[[nodiscard]] bool sources_finite(std::span<const ScaleCandidate> candidates,
                                  const PricedRow &row) noexcept {
  for (const ScaleCandidate &candidate : candidates) {
    if (!std::isfinite(source_value(row.greeks, row.dp_dq, candidate.source))) {
      return false;
    }
  }
  return true;
}

// Nine best-scale SELECTION errors for one convention map on the deterministic
// tune sample — the same bounded attribution the winner runs, restricted to the
// sample. Only `input_model` of `map` matters here: every candidate scale
// multiplies the raw engine Greek, so the map's own scale fields never enter.
//
// An unobserved Greek yields infinity (Accumulator::mean on an empty
// accumulator). Compared with `>`, that reads as a regression unless the other
// side is equally unobserved, which is the intended fail-closed direction for a
// finalist nothing can be said about.
[[nodiscard]] std::array<double, kGreekCount>
attribute_greeks(std::span<const OracleRow> rows, std::size_t stride, const ConventionMap &map) {
  GreekSearches searches = make_greek_searches();
  for (std::size_t index = 0; index < rows.size(); index += stride) {
    const OracleRow &row = rows[index];
    const std::optional<PricedRow> priced = price_row(row, map);
    if (!priced.has_value()) {
      continue;
    }
    for (std::size_t greek = 0; greek < kGreekCount; ++greek) {
      std::vector<ScaleCandidate> &candidates = searches[greek];
      const double oracle = row.*kGreekSpecs[greek].oracle;
      if (!std::isfinite(oracle) || !sources_finite(candidates, *priced)) {
        continue;
      }
      for (ScaleCandidate &candidate : candidates) {
        const double model =
            source_value(priced->greeks, priced->dp_dq, candidate.source) * candidate.scale;
        candidate.error.selection.symmetric_relative(model, oracle);
      }
    }
  }
  std::array<double, kGreekCount> out{};
  for (std::size_t greek = 0; greek < kGreekCount; ++greek) {
    out[greek] = searches[greek][best_scale(searches[greek])].error.selection.mean();
  }
  return out;
}

// Stable total order on candidate identity: source ID first, then the SIGNED
// SCALE NUMERICALLY. Formatting the scale into a string would order 0.01 ahead
// of 1.0 by digits rather than by value, and would allocate past SSO on ids
// like "carry_rho:0.002740".
[[nodiscard]] bool less_candidate_id(const ScaleCandidate &left,
                                     const ScaleCandidate &right) noexcept {
  const std::string_view left_id = greek_source_id(left.source);
  const std::string_view right_id = greek_source_id(right.source);
  if (left_id != right_id) {
    return left_id < right_id;
  }
  return left.scale < right.scale;
}

[[nodiscard]] std::size_t
best_price_scale(std::span<const PriceScaleCandidate> candidates) noexcept {
  assert(!candidates.empty());
  std::size_t best = 0;
  for (std::size_t index = 1; index < candidates.size(); ++index) {
    const double value = candidates[index].error.mean();
    const double best_value = candidates[best].error.mean();
    const bool ties_lower = value == best_value && candidates[index].scale < candidates[best].scale;
    if (value < best_value || ties_lower) {
      best = index;
    }
  }
  return best;
}

// Designated initializers: `count` and `selection_count` are adjacent same-typed
// fields, and transposing them is exactly the defect the gates' population
// parity check would then be unable to see.
[[nodiscard]] FloorMetric floor_metric(std::string id, const FloorAccumulators &acc,
                                       std::string unit, double multiplier = 1.0) {
  return FloorMetric{.metric_id = std::move(id),
                     .value = acc.report.mean() * multiplier,
                     .count = acc.report.count,
                     .selection_count = acc.selection.count,
                     .unit = std::move(unit)};
}

// Absolute floors have no relative denominator, so the reported population IS
// the selection population and both counts are the same accumulator's.
[[nodiscard]] FloorMetric floor_metric(std::string id, const Accumulator &acc, std::string unit,
                                       double multiplier = 1.0) {
  return FloorMetric{.metric_id = std::move(id),
                     .value = acc.mean() * multiplier,
                     .count = acc.count,
                     .selection_count = acc.count,
                     .unit = std::move(unit)};
}

// DEFINITION SITE 2 of the two published floor arrays. Same FloorMetric shape
// and same rows as floor_metric() above; it reads the SYMMETRIC accumulator
// instead of the reported one.
//
// The symmetric loss is what the scale search minimises, because it is bounded
// and carries no smallest-scale gradient; the no-regression gate and the ratchet
// baseline are stated against THIS array so that the gate and the selector
// optimise one objective. The standard-relative array is still published,
// unchanged, so the committed floor stays directly comparable to the charter's
// "greeks within 1% rel" target. Do not unify the two.
[[nodiscard]] FloorMetric symmetric_floor_metric(std::string id, const FloorAccumulators &acc,
                                                 std::string unit, double multiplier = 1.0) {
  return FloorMetric{.metric_id = std::move(id),
                     .value = acc.selection.mean() * multiplier,
                     .count = acc.report.count,
                     .selection_count = acc.selection.count,
                     .unit = std::move(unit)};
}

// An accumulator that admitted nothing has an infinite mean, and `%.17g` writes
// that as a bare `inf` — JSON that does not parse. Name the empty metric at the
// source instead of failing a 12-minute sweep on "sweep is not JSON".
[[nodiscard]] const FloorMetric *first_unobserved_metric(std::span<const FloorMetric> metrics) {
  for (const FloorMetric &metric : metrics) {
    if (metric.count <= 0 || !std::isfinite(metric.value)) {
      return &metric;
    }
  }
  return nullptr;
}

[[nodiscard]] std::string_view sign_id(double scale) noexcept {
  return std::signbit(scale) ? "negative" : "positive";
}

[[nodiscard]] std::string_view scale_id(double scale) noexcept {
  const double value = std::abs(scale);
  if (value == 100.0) {
    return "per_contract_100";
  }
  if (value == 1.0) {
    return "per_unit";
  }
  if (value == 0.01) {
    return "per_point";
  }
  if (value == 0.0001) {
    return "per_point_squared";
  }
  return "per_day";
}

[[nodiscard]] std::string_view price_scale_id(double scale) noexcept {
  const double value = std::abs(scale);
  if (value == 100.0) {
    return "per_contract_100";
  }
  if (value == 0.01) {
    return "per_share_from_contract";
  }
  return "per_share";
}

[[nodiscard]] std::string_view day_count_id(double days) noexcept {
  if (days == 365.0) {
    return "ACT_365F";
  }
  if (days == 365.25) {
    return "ACT_365_25";
  }
  if (days == 360.0) {
    return "ACT_360";
  }
  if (days == 252.0) {
    return "BUS_252";
  }
  return "PER_YEAR";
}

[[nodiscard]] double days_from_time_scale(double scale) noexcept {
  const double value = std::abs(scale);
  return value == 1.0 ? 365.0 : 1.0 / value;
}

[[nodiscard]] std::string_view time_basis_id(double scale) noexcept {
  return std::abs(scale) == 1.0 ? "per_year" : "per_day";
}

void append_json_string(std::string &out, std::string_view value) {
  out.push_back('"');
  for (const char ch : value) {
    if (ch == '"') {
      out.append("\\\"");
    } else if (ch == '\\') {
      out.append(2, '\\');
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
}

// PRECONDITION: `value` is finite. `%.17g` renders infinity/NaN as bare
// `inf`/`nan`, which is not JSON — run_convention_sweep refuses to return a
// result carrying a non-finite number so this stays unreachable.
void append_double(std::string &out, double value) {
  assert(std::isfinite(value));
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%.17g", value);
  out.append(buffer);
}

void append_int(std::string &out, std::int64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
  out.append(buffer);
}

void append_metric_array(std::string &out, std::span<const FloorMetric> metrics) {
  out.push_back('[');
  for (std::size_t index = 0; index < metrics.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    const FloorMetric &metric = metrics[index];
    out.append("{\"metric_id\":");
    append_json_string(out, metric.metric_id);
    out.append(",\"value\":");
    append_double(out, metric.value);
    out.append(",\"count\":");
    append_int(out, metric.count);
    out.append(",\"selection_count\":");
    append_int(out, metric.selection_count);
    out.append(",\"unit\":");
    append_json_string(out, metric.unit);
    out.push_back('}');
  }
  out.push_back(']');
}

// ONE delta rendering, shared by the standard and the symmetric arrays.
// `candidate - baseline == delta` is the invariant five gate layers re-check to
// 1e-12, and two hand-written copies of this loop is exactly how the two arrays
// would come to disagree about what a delta is.
// PRECONDITION: both spans carry the same metric ids in the same order.
void append_delta_array(std::string &out, std::span<const FloorMetric> metrics,
                        std::span<const FloorMetric> baseline) {
  assert(metrics.size() == baseline.size());
  out.push_back('[');
  for (std::size_t index = 0; index < metrics.size(); ++index) {
    assert(metrics[index].metric_id == baseline[index].metric_id);
    if (index != 0) {
      out.push_back(',');
    }
    out.append("{\"metric_id\":");
    append_json_string(out, metrics[index].metric_id);
    out.append(",\"candidate\":");
    append_double(out, metrics[index].value);
    out.append(",\"baseline\":");
    append_double(out, baseline[index].value);
    out.append(",\"delta\":");
    append_double(out, metrics[index].value - baseline[index].value);
    out.append(",\"count\":");
    append_int(out, metrics[index].count);
    out.append(",\"unit\":");
    append_json_string(out, metrics[index].unit);
    out.push_back('}');
  }
  out.push_back(']');
}

[[nodiscard]] std::string_view forward_formula(InputModel model) noexcept {
  return model == InputModel::CurrentSpotSdivYield ? "none" : "uprc_exp_rate_t_minus_ddiv";
}

// These labels are written verbatim into bootstrap/conventions.json, so a
// silently defaulted label would be a falsified receipt: enumerate every model.
[[nodiscard]] std::string_view rate_model(InputModel model) noexcept {
  switch (model) {
  case InputModel::CurrentSpotSdivYield:
  case InputModel::DiscreteDividendPvSdivYield:
  case InputModel::DiscreteForwardNetCarry:
  case InputModel::DiscreteForwardRateSdivYield:
    return "continuous_row_rate";
  case InputModel::DiscreteForwardNetRate:
  case InputModel::DiscreteDividendPvNetRate:
    return "continuous_rate_minus_sdiv";
  case InputModel::DiscreteDividendPvRatePlusSdiv:
    return "continuous_rate_plus_sdiv";
  case InputModel::DiscreteForwardZeroRates:
    return "zero";
  }
  assert(false);
  return "invalid";
}

[[nodiscard]] std::string_view carry_model(InputModel model) noexcept {
  switch (model) {
  case InputModel::CurrentSpotSdivYield:
  case InputModel::DiscreteDividendPvSdivYield:
  case InputModel::DiscreteForwardNetCarry:
  case InputModel::DiscreteForwardRateSdivYield:
    return "sdiv_as_yield";
  case InputModel::DiscreteForwardNetRate:
  case InputModel::DiscreteForwardZeroRates:
  case InputModel::DiscreteDividendPvNetRate:
  case InputModel::DiscreteDividendPvRatePlusSdiv:
    return "zero";
  }
  assert(false);
  return "invalid";
}

} // namespace

void Accumulator::absolute(double model, double oracle) noexcept {
  if (std::isfinite(model) && std::isfinite(oracle)) {
    sum += std::abs(model - oracle);
    ++count;
  }
}

// REPORTED objective. Asymmetric on purpose: the charter states the Greek
// target as an error relative to the ORACLE, so the published number stays
// directly comparable to it.
void Accumulator::relative(double model, double oracle) noexcept {
  if (std::isfinite(model) && std::isfinite(oracle)) {
    sum += std::abs(model - oracle) / std::max(std::abs(oracle), kSelectionAbsFloor);
    ++count;
  }
}

// SELECTION objective. The largest of the two magnitudes (floored only for the
// degenerate both-near-zero case) bounds the ratio, which removes the
// asymmetric form's systematic pull toward the smallest candidate scale.
// Deliberately NOT the same function as relative() above — see the header.
void Accumulator::symmetric_relative(double model, double oracle) noexcept {
  if (std::isfinite(model) && std::isfinite(oracle)) {
    sum += std::abs(model - oracle) /
           std::max({std::abs(model), std::abs(oracle), kSelectionAbsFloor});
    ++count;
  }
}

double Accumulator::mean() const noexcept {
  return count > 0 ? sum / static_cast<double>(count) : std::numeric_limits<double>::infinity();
}

std::size_t best_scale(std::span<const ScaleCandidate> candidates) noexcept {
  assert(!candidates.empty());
  std::size_t best = 0;
  for (std::size_t index = 1; index < candidates.size(); ++index) {
    const double value = candidates[index].error.selection.mean();
    const double best_value = candidates[best].error.selection.mean();
    if (value < best_value ||
        (value == best_value && less_candidate_id(candidates[index], candidates[best]))) {
      best = index;
    }
  }
  return best;
}

bool less_finalist(const FinalistRank &left, const FinalistRank &right) noexcept {
  if (left.regresses_any_greek != right.regresses_any_greek) {
    return !left.regresses_any_greek;
  }
  if (left.tune_price_mae != right.tune_price_mae) {
    return left.tune_price_mae < right.tune_price_mae;
  }
  return left.candidate_id < right.candidate_id;
}

Result<ConventionSweepResult> run_convention_sweep(std::span<const OracleRow> smoke,
                                                   std::span<const OracleRow> tune) {
  if (smoke.empty() || tune.empty()) {
    return Err(ErrorCode::InvalidArgument, "convention sweep requires non-empty smoke+tune");
  }
  const ConventionMap &baseline = baseline_convention();
  std::vector<PriceCandidate> prices;
  prices.reserve(kInputModels.size());
  for (const InputModel model : kInputModels) {
    prices.push_back(PriceCandidate{model, {}, {}});
    evaluate_price_rows(smoke, 1, prices.back(), PriceStage::Smoke);
  }
  std::vector<std::size_t> finalists(prices.size());
  for (std::size_t index = 0; index < finalists.size(); ++index) {
    finalists[index] = index;
  }
  std::sort(finalists.begin(), finalists.end(), [&](std::size_t left, std::size_t right) {
    return less_price(prices[left], prices[right], PriceStage::Smoke);
  });
  assert(finalists.size() >= kFinalistCount);
  finalists.resize(kFinalistCount);
  const std::size_t tune_stride = std::max<std::size_t>(1, tune.size() / 32768);
  for (const std::size_t index : finalists) {
    evaluate_price_rows(tune, tune_stride, prices[index], PriceStage::TuneSample);
  }
  ConventionSweepResult out;
  // Stage 2 is the ONLY place Greeks enter the input-model choice. Nine-Greek
  // attribution for all eight candidates is prohibitively expensive, so only
  // the two finalists — and the baseline they are measured against — pay for
  // it. Ranking all eight on price alone is how a Greek regression used to
  // reach the winner unnoticed.
  const std::array<double, kGreekCount> baseline_greeks =
      attribute_greeks(tune, tune_stride, baseline);
  std::array<std::vector<std::string>, kFinalistCount> finalist_regressions;
  std::array<FinalistRank, kFinalistCount> ranks;
  for (std::size_t slot = 0; slot < kFinalistCount; ++slot) {
    const PriceCandidate &candidate = prices[finalists[slot]];
    ConventionMap arm = baseline;
    arm.input_model = candidate.model;
    const std::array<double, kGreekCount> arm_greeks =
        arm.input_model == baseline.input_model ? baseline_greeks
                                                : attribute_greeks(tune, tune_stride, arm);
    for (std::size_t greek = 0; greek < kGreekCount; ++greek) {
      if (arm_greeks[greek] > baseline_greeks[greek]) {
        finalist_regressions[slot].emplace_back(kGreekSpecs[greek].metric_id);
      }
    }
    ranks[slot] = FinalistRank{.regresses_any_greek = !finalist_regressions[slot].empty(),
                               .tune_price_mae = candidate.tune.mean(),
                               .candidate_id = input_model_id(candidate.model)};
  }
  const std::size_t chosen = less_finalist(ranks[1], ranks[0]) ? 1 : 0;
  ConventionMap winner = baseline;
  winner.input_model = prices[finalists[chosen]].model;
  out.input_model_regressed_greeks = std::move(finalist_regressions[chosen]);

  std::vector<PriceScaleCandidate> price_scales = price_scales_for(kUnitScales);
  GreekSearches searches = make_greek_searches();
  BaselineFloors baseline_floors;
  Accumulator candidate_vol;
  out.smoke_rows = static_cast<std::int64_t>(smoke.size());
  out.tune_rows = static_cast<std::int64_t>(tune.size());
  const auto start = std::chrono::steady_clock::now();

  auto evaluate_full = [&](std::span<const OracleRow> rows) {
    for (const OracleRow &row : rows) {
      // Both arms are priced BEFORE anything is committed. A row either feeds
      // the candidate and the baseline floors or feeds neither, so
      // metric_deltas can never compare two different populations.
      const std::optional<PricedRow> win = price_row(row, winner);
      if (!win.has_value()) {
        ++out.engine_errors;
        continue;
      }
      const std::optional<PricedRow> base =
          winner.input_model == baseline.input_model ? win : price_row(row, baseline);
      if (!base.has_value()) {
        ++out.engine_errors;
        continue;
      }
      ++out.rows_priced;

      const double base_price = price_to_oracle_units(base->greeks.price, baseline);
      if (std::isfinite(win->greeks.price) && std::isfinite(base_price) &&
          std::isfinite(row.sr_prc)) {
        for (PriceScaleCandidate &candidate : price_scales) {
          candidate.error.absolute(win->greeks.price * candidate.scale, row.sr_prc);
        }
        baseline_floors.price.absolute(base_price, row.sr_prc);
      }
      // Identity by construction on both arms (oracle_scorecard.hpp:68-70:
      // srVol is the supplied Mode A pricing input). It reads the CONVENTION
      // LAYER's sigma, not the raw row, so a convention that ever transformed
      // sigma would surface here instead of reporting a hard-coded zero.
      if (std::isfinite(win->inputs.sigma) && std::isfinite(base->inputs.sigma) &&
          std::isfinite(row.sr_vol)) {
        candidate_vol.absolute(win->inputs.sigma, row.sr_vol);
        baseline_floors.vol.absolute(base->inputs.sigma, row.sr_vol);
      }

      const OracleUnitGreeks base_units = to_oracle_units(base->greeks, base->dp_dq, baseline);
      for (std::size_t greek = 0; greek < kGreekCount; ++greek) {
        std::vector<ScaleCandidate> &candidates = searches[greek];
        FloorAccumulators &baseline_acc = baseline_floors.greeks[greek];
        const double oracle = row.*kGreekSpecs[greek].oracle;
        const double baseline_value = base_units.*kGreekSpecs[greek].baseline;
        if (!std::isfinite(oracle) || !std::isfinite(baseline_value) ||
            !sources_finite(candidates, *win)) {
          continue;
        }
        for (ScaleCandidate &candidate : candidates) {
          const double model =
              source_value(win->greeks, win->dp_dq, candidate.source) * candidate.scale;
          // REPORTING: the published floor, standard relative error over the
          // full population, so it stays directly comparable to the charter's
          // "greeks within 1% rel" target.
          candidate.error.report.relative(model, oracle);
          // SELECTION: the symmetric objective. A DIFFERENT function from the
          // reported metric by design — the selection loss must be
          // well-conditioned, the reported metric must be the target. A future
          // reader should not "helpfully" unify these two lines.
          candidate.error.selection.symmetric_relative(model, oracle);
        }
        baseline_acc.report.relative(baseline_value, oracle);
        baseline_acc.selection.symmetric_relative(baseline_value, oracle);
      }
    }
  };
  evaluate_full(smoke);
  evaluate_full(tune);

  const std::size_t price_index = best_price_scale(price_scales);
  std::array<std::size_t, kGreekCount> best{};
  for (std::size_t greek = 0; greek < kGreekCount; ++greek) {
    best[greek] = best_scale(searches[greek]);
  }
  winner.price_scale = price_scales[price_index].scale;
  winner.delta_scale = searches[kDeltaSearch][best[kDeltaSearch]].scale;
  winner.gamma_scale = searches[kGammaSearch][best[kGammaSearch]].scale;
  winner.theta_scale = searches[kThetaSearch][best[kThetaSearch]].scale;
  // Theta's day count only; the DTE banding day count stays pinned.
  winner.theta_days_per_year = days_from_time_scale(winner.theta_scale);
  winner.vega_scale = searches[kVegaSearch][best[kVegaSearch]].scale;
  winner.rho_scale = searches[kRhoSearch][best[kRhoSearch]].scale;
  winner.phi_scale = searches[kPhiSearch][best[kPhiSearch]].scale;
  winner.volga_source = searches[kVolgaSearch][best[kVolgaSearch]].source;
  winner.volga_scale = searches[kVolgaSearch][best[kVolgaSearch]].scale;
  winner.vanna_source = searches[kVannaSearch][best[kVannaSearch]].source;
  winner.vanna_scale = searches[kVannaSearch][best[kVannaSearch]].scale;
  winner.delta_decay_scale = searches[kDecaySearch][best[kDecaySearch]].scale;
  out.winner = winner;
  out.metrics = {
      floor_metric("mode_a_price_mae", price_scales[price_index].error, "ticks", 100.0),
      floor_metric("mode_a_vol_mae", candidate_vol, "bp", 10000.0),
  };
  out.baseline_metrics = {
      floor_metric("mode_a_price_mae", baseline_floors.price, "ticks", 100.0),
      floor_metric("mode_a_vol_mae", baseline_floors.vol, "bp", 10000.0),
  };
  // The two absolute floors have no relative denominator, so their symmetric
  // entry IS their standard entry — the arrays differ only on the nine relative
  // Greeks. Publishing them anyway keeps both arrays at the same eleven ids over
  // the same populations, which is what lets the gate compare them index-free.
  out.symmetric_metrics = {
      floor_metric("mode_a_price_mae", price_scales[price_index].error, "ticks", 100.0),
      floor_metric("mode_a_vol_mae", candidate_vol, "bp", 10000.0),
  };
  out.baseline_symmetric_metrics = {
      floor_metric("mode_a_price_mae", baseline_floors.price, "ticks", 100.0),
      floor_metric("mode_a_vol_mae", baseline_floors.vol, "bp", 10000.0),
  };
  for (std::size_t greek = 0; greek < kGreekCount; ++greek) {
    const std::string metric_id{kGreekSpecs[greek].metric_id};
    out.metrics.push_back(
        floor_metric(metric_id, searches[greek][best[greek]].error, "relative"));
    out.baseline_metrics.push_back(
        floor_metric(metric_id, baseline_floors.greeks[greek], "relative"));
    out.symmetric_metrics.push_back(
        symmetric_floor_metric(metric_id, searches[greek][best[greek]].error, "relative"));
    out.baseline_symmetric_metrics.push_back(
        symmetric_floor_metric(metric_id, baseline_floors.greeks[greek], "relative"));
  }
  // Designated initializers: two doubles then two int64s in a row is exactly the
  // transposition that would defeat the population checks by construction.
  for (const PriceCandidate &candidate : prices) {
    out.candidate_prices.push_back(CandidatePriceMetric{
        .candidate_id = std::string(input_model_id(candidate.model)),
        .smoke_price_mae_ticks = candidate.smoke.mean() * 100.0,
        .smoke_count = candidate.smoke.count,
        .tune_sample_price_mae_ticks =
            candidate.tune.count > 0 ? candidate.tune.mean() * 100.0 : 0.0,
        .tune_sample_count = candidate.tune.count});
  }
  std::sort(out.candidate_prices.begin(), out.candidate_prices.end(),
            [](const CandidatePriceMetric &left, const CandidatePriceMetric &right) {
              return left.candidate_id < right.candidate_id;
            });
  out.diagnostic_wall_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  out.diagnostic_rows_per_second =
      out.diagnostic_wall_seconds > 0.0
          ? static_cast<double>(out.rows_priced) / out.diagnostic_wall_seconds
          : 0.0;

  // Every published number must be finite, and the only way one is not is an
  // accumulator that admitted nothing. Refuse HERE, naming the metric or the
  // input candidate, rather than emitting `inf` and having the gate report the
  // whole 12-minute sweep as "not JSON".
  if (const FloorMetric *empty = first_unobserved_metric(out.metrics)) {
    return Err(ErrorCode::InvalidArgument,
               "convention sweep candidate metric has no observation: " + empty->metric_id);
  }
  if (const FloorMetric *empty = first_unobserved_metric(out.baseline_metrics)) {
    return Err(ErrorCode::InvalidArgument,
               "convention sweep baseline metric has no observation: " + empty->metric_id);
  }
  if (const FloorMetric *empty = first_unobserved_metric(out.symmetric_metrics)) {
    return Err(ErrorCode::InvalidArgument,
               "convention sweep symmetric metric has no observation: " + empty->metric_id);
  }
  if (const FloorMetric *empty = first_unobserved_metric(out.baseline_symmetric_metrics)) {
    return Err(ErrorCode::InvalidArgument,
               "convention sweep baseline symmetric metric has no observation: " +
                   empty->metric_id);
  }
  for (const CandidatePriceMetric &candidate : out.candidate_prices) {
    if (candidate.smoke_count <= 0 || !std::isfinite(candidate.smoke_price_mae_ticks) ||
        !std::isfinite(candidate.tune_sample_price_mae_ticks)) {
      return Err(ErrorCode::InvalidArgument,
                 "convention sweep input candidate priced no smoke row: " + candidate.candidate_id);
    }
  }
  return Ok(std::move(out));
}

std::string convention_map_json(const ConventionMap &map) {
  std::string out = "{";
  auto field = [&](std::string_view name, std::string_view value, bool first = false) {
    if (!first) {
      out.push_back(',');
    }
    append_json_string(out, name);
    out.push_back(':');
    append_json_string(out, value);
  };
  field("input_model", input_model_id(map.input_model), true);
  field("forward_formula", forward_formula(map.input_model));
  field("rate_model", rate_model(map.input_model));
  field("carry_model", carry_model(map.input_model));
  field("dividend_model", map.input_model == InputModel::CurrentSpotSdivYield
                              ? "continuous_yield_only"
                              : "discrete_cash_forward");
  // Derived from the MULTIPLIER PRODUCTION APPLIES, not from the descriptive
  // `theta_days_per_year` field: a map whose two theta fields disagree must
  // render differently from a correct one, or the divergence check between
  // `production_conventions` and the resolved winner cannot see the difference.
  // `theta_days_per_year` stays a redundant cross-check, asserted here.
  assert(day_count_id(map.theta_days_per_year) ==
         day_count_id(days_from_time_scale(map.theta_scale)));
  field("day_count", day_count_id(days_from_time_scale(map.theta_scale)));
  // The scorecard's DTE-banding day count. It is outside the search, but a
  // silent change to it re-buckets every cell, so the receipt records it.
  field("dte_banding_day_count", day_count_id(map.days_per_year));
  field("price_scale", price_scale_id(map.price_scale));
  field("price_sign", sign_id(map.price_scale));
  field("vol_scale", "decimal_identity");
  field("delta_scale", scale_id(map.delta_scale));
  field("delta_sign", sign_id(map.delta_scale));
  field("gamma_scale", scale_id(map.gamma_scale));
  field("gamma_sign", sign_id(map.gamma_scale));
  field("theta_basis", time_basis_id(map.theta_scale));
  field("theta_sign", sign_id(map.theta_scale));
  field("vega_scale", scale_id(map.vega_scale));
  field("vega_sign", sign_id(map.vega_scale));
  field("rho_scale", scale_id(map.rho_scale));
  field("rho_sign", sign_id(map.rho_scale));
  field("phi_scale", scale_id(map.phi_scale));
  field("phi_sign", sign_id(map.phi_scale));
  field("volga_source", greek_source_id(map.volga_source));
  field("volga_scale", scale_id(map.volga_scale));
  field("volga_sign", sign_id(map.volga_scale));
  field("vanna_source", greek_source_id(map.vanna_source));
  field("vanna_scale", scale_id(map.vanna_scale));
  field("vanna_sign", sign_id(map.vanna_scale));
  field("delta_decay_basis", time_basis_id(map.delta_decay_scale));
  field("delta_decay_day_count", day_count_id(days_from_time_scale(map.delta_decay_scale)));
  field("delta_decay_sign", sign_id(map.delta_decay_scale));
  out.push_back('}');
  return out;
}

std::string convention_sweep_json(const ConventionSweepResult &result, std::string_view git_sha) {
  std::string out = "{\"schema_version\":2,\"kind\":\"convention_sweep\",\"git_sha\":";
  append_json_string(out, git_sha);
  out.append(",\"cohorts\":[\"smoke\",\"tune\"],\"selection_strategy\":"
             "\"all_smoke_then_top2_greek_then_price_tune_sample_then_full_attribution\","
             "\"smoke_rows\":");
  append_int(out, result.smoke_rows);
  out.append(",\"tune_rows\":");
  append_int(out, result.tune_rows);
  out.append(",\"rows_priced\":");
  append_int(out, result.rows_priced);
  out.append(",\"engine_errors\":");
  append_int(out, result.engine_errors);
  out.append(",\"baseline_conventions\":");
  out.append(convention_map_json(baseline_convention()));
  out.append(",\"conventions\":");
  out.append(convention_map_json(result.winner));
  // What production actually prices with. The gate fails closed while this
  // differs from the sweep's winner, so a committed floor can never describe a
  // map the engine does not use.
  out.append(",\"production_conventions\":");
  out.append(convention_map_json(winning_convention()));
  // DEFINITION SITE 3 of the two published floor arrays. The STANDARD-RELATIVE
  // trio is emitted unchanged so the committed floor stays directly comparable
  // to the charter's "greeks within 1% rel" target, which is stated relative to
  // the oracle. The SYMMETRIC-RELATIVE trio beside it is the loss the scale
  // selection actually minimises — bounded, with no smallest-scale gradient —
  // and it is the array the no-regression gate and the ratchet baseline are
  // stated against, so gate and selector optimise the same thing. Publishing
  // both is the point; do not unify them.
  out.append(",\"metrics\":");
  append_metric_array(out, result.metrics);
  out.append(",\"baseline_metrics\":");
  append_metric_array(out, result.baseline_metrics);
  out.append(",\"metric_deltas\":");
  append_delta_array(out, result.metrics, result.baseline_metrics);
  out.append(",\"symmetric_metrics\":");
  append_metric_array(out, result.symmetric_metrics);
  out.append(",\"baseline_symmetric_metrics\":");
  append_metric_array(out, result.baseline_symmetric_metrics);
  out.append(",\"symmetric_metric_deltas\":");
  append_delta_array(out, result.symmetric_metrics, result.baseline_symmetric_metrics);
  out.append(",\"candidate_prices\":[");
  for (std::size_t index = 0; index < result.candidate_prices.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    const CandidatePriceMetric &candidate = result.candidate_prices[index];
    out.append("{\"candidate_id\":");
    append_json_string(out, candidate.candidate_id);
    out.append(",\"smoke_price_mae_ticks\":");
    append_double(out, candidate.smoke_price_mae_ticks);
    out.append(",\"smoke_count\":");
    append_int(out, candidate.smoke_count);
    out.append(",\"tune_sample_price_mae_ticks\":");
    append_double(out, candidate.tune_sample_price_mae_ticks);
    out.append(",\"tune_sample_count\":");
    append_int(out, candidate.tune_sample_count);
    out.push_back('}');
  }
  // What the greek-aware finalist rank cost, if anything. Empty when the
  // selected input model regressed on none of the nine; non-empty only when
  // BOTH finalists regressed and the rank degenerated to price MAE, which must
  // be visible in the receipt rather than absorbed silently.
  out.append("],\"input_model_regressed_greeks\":[");
  for (std::size_t index = 0; index < result.input_model_regressed_greeks.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    append_json_string(out, result.input_model_regressed_greeks[index]);
  }
  out.append("],\"oracle_suspect_candidates\":[],"
             "\"market_evidence_status\":\"not_evaluated_no_nbbo_gate\","
             "\"diagnostic_speed\":{\"preset\":\"dev\",\"citable\":false,"
             "\"wall_seconds\":");
  append_double(out, result.diagnostic_wall_seconds);
  out.append(",\"rows_per_second\":");
  append_double(out, result.diagnostic_rows_per_second);
  out.append("}}\n");
  return out;
}

} // namespace atx::vol::oracle

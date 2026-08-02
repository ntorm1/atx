#include "atx/vol/backtest_template.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"

namespace atx::vol {
namespace {

[[nodiscard]] bool finite_positive(double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool valid_maturity(const ProjectedMaturitySpec &maturity) noexcept {
  switch (maturity.kind) {
  case ProjectedMaturityKind::YearFraction:
    return finite_positive(maturity.year_fraction);
  case ProjectedMaturityKind::CalendarDays:
    return maturity.calendar_count > 0;
  case ProjectedMaturityKind::CalendarMonths:
    return maturity.calendar_count > 0 && maturity.calendar_count <= 1200;
  case ProjectedMaturityKind::AbsoluteExpiry:
    return maturity.expiry_ts_ns > 0;
  }
  return false;
}

[[nodiscard]] bool valid_strike(const ProjectedStrikeSpec &strike) noexcept {
  if (!std::isfinite(strike.value)) {
    return false;
  }
  switch (strike.kind) {
  case ProjectedStrikeKind::AtmForward:
    return true;
  case ProjectedStrikeKind::Delta:
    return strike.value > 0.0 && strike.value < 1.0;
  case ProjectedStrikeKind::LogMoneyness:
    return true;
  case ProjectedStrikeKind::AbsoluteStrike:
    return strike.value > 0.0;
  }
  return false;
}

[[nodiscard]] bool valid_side(Side side) noexcept {
  switch (side) {
  case Side::Call:
  case Side::Put:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_hedge(const HedgeSpec &hedge) noexcept {
  switch (hedge.kind) {
  case HedgeSpec::Kind::None:
  case HedgeSpec::Kind::DeltaToZero:
    break;
  default:
    return false;
  }
  switch (hedge.cadence) {
  case HedgeSpec::Cadence::AtEntry:
  case HedgeSpec::Cadence::Daily:
    break;
  default:
    return false;
  }
  return std::isfinite(hedge.band) && hedge.band >= 0.0;
}

[[nodiscard]] bool valid_projection(const BacktestProjectionSettings &projection) noexcept {
  if (!(std::isfinite(projection.delta_tolerance) && projection.delta_tolerance > 0.0 &&
        projection.delta_tolerance <= 1.0e-3)) {
    return false;
  }
  switch (projection.query_execution) {
  case QueryExecution::Configured:
  case QueryExecution::ColdReference:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_frictions(const FrictionModel &frictions) noexcept {
  switch (frictions.spread_kind) {
  case FrictionModel::SpreadKind::None:
  case FrictionModel::SpreadKind::PriceBps:
  case FrictionModel::SpreadKind::VolTicks:
    break;
  default:
    return false;
  }
  const double values[] = {
      frictions.half_spread_bps,   frictions.vol_tick,           frictions.impact_fraction,
      frictions.per_contract_cost, frictions.hedge_slippage_bps,
  };
  for (const double value : values) {
    if (!(std::isfinite(value) && value >= 0.0)) {
      return false;
    }
  }
  return true;
}

class StableFingerprint final {
public:
  void add_byte(std::uint8_t value) noexcept {
    state_ ^= value;
    state_ *= kFnvPrime;
  }

  void add_bool(bool value) noexcept { add_byte(value ? 1u : 0u); }

  void add_u64(std::uint64_t value) noexcept {
    for (unsigned shift = 0u; shift < 64u; shift += 8u) {
      add_byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }

  void add_i64(std::int64_t value) noexcept { add_u64(static_cast<std::uint64_t>(value)); }

  void add_double(double value) noexcept {
    // Validation excludes NaNs. Normalize signed zero so semantically identical
    // zero-valued settings have one persistent representation.
    add_u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
  }

  [[nodiscard]] std::uint64_t finish() const noexcept { return state_ == 0u ? 1u : state_; }

private:
  static constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
  static constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
  std::uint64_t state_{kFnvOffset};
};

void fingerprint_maturity(StableFingerprint &hash, const ProjectedMaturitySpec &maturity) noexcept {
  hash.add_u64(static_cast<std::uint64_t>(maturity.kind));
  switch (maturity.kind) {
  case ProjectedMaturityKind::YearFraction:
    hash.add_double(maturity.year_fraction);
    return;
  case ProjectedMaturityKind::CalendarDays:
  case ProjectedMaturityKind::CalendarMonths:
    hash.add_i64(maturity.calendar_count);
    return;
  case ProjectedMaturityKind::AbsoluteExpiry:
    hash.add_i64(maturity.expiry_ts_ns);
    return;
  }
}

void fingerprint_strike(StableFingerprint &hash, const ProjectedStrikeSpec &strike) noexcept {
  hash.add_u64(static_cast<std::uint64_t>(strike.kind));
  switch (strike.kind) {
  case ProjectedStrikeKind::AtmForward:
    return;
  case ProjectedStrikeKind::Delta:
  case ProjectedStrikeKind::LogMoneyness:
  case ProjectedStrikeKind::AbsoluteStrike:
    hash.add_double(strike.value);
    return;
  }
}

struct AdjustedMaturity {
  ProjectedMaturitySpec requested{};
  std::int64_t expiry_ts_ns{0};
};

[[nodiscard]] Result<std::int64_t> resolve_adjusted_expiry(
    const SurfaceRef &surface, std::uint32_t uid, const ProjectedMaturitySpec &maturity,
    const BacktestProjectionSettings &projection, TheoreticalSettlementRule settlement) {
  OptionProjectionSpec target;
  target.uid = uid;
  target.maturity = maturity;
  target.strike = ProjectedStrikeSpec::atm_forward();
  target.side = Side::Call;
  target.multiplier = 1.0;

  OptionProjectionConfig target_config;
  target_config.output = OptionProjectionOutput::DefinitionOnly;
  target_config.analytic_greeks = projection.analytic_greeks;
  target_config.delta_tolerance = projection.delta_tolerance;
  target_config.query_execution = projection.query_execution;
  ATX_TRY(const ProjectedOption raw, project_option_contract(surface, target, target_config));

  switch (settlement) {
  case TheoreticalSettlementRule::FollowingNyseSessionSnapshot: {
    const atx::core::time::Calendar calendar;
    const atx::core::time::CivilTime target = atx::core::time::to_civil_utc(
        atx::core::time::Timestamp::from_unix_nanos(raw.definition.expiry_ts_ns));
    atx::core::time::Date date = target.date;
    if (!calendar.is_trading_day(date)) {
      date = calendar.next_trading_day(date);
    }
    const std::int64_t adjusted =
        atx::core::time::timestamp_from_utc(date.year, date.month, date.day, target.hour,
                                            target.minute, target.second, target.nano)
            .unix_nanos();
    if (adjusted <= surface->pricing().now_ts_ns) {
      return atx::core::Err(ErrorCode::OutOfRange,
                            "backtest template: adjusted expiry is not after valuation");
    }
    return atx::core::Ok(adjusted);
  }
  }
  return atx::core::Err(ErrorCode::InvalidArgument,
                        "backtest template: unsupported settlement rule");
}

[[nodiscard]] Result<std::vector<AdjustedMaturity>>
adjusted_maturities(const SurfaceRef &surface, std::uint32_t uid,
                    const BacktestStrategyTemplate &strategy_template) {
  std::vector<AdjustedMaturity> adjusted;
  adjusted.reserve(strategy_template.legs.size());
  for (const BacktestTemplateLeg &leg : strategy_template.legs) {
    bool found = false;
    for (const AdjustedMaturity &known : adjusted) {
      if (known.requested == leg.maturity) {
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }
    ATX_TRY(const std::int64_t expiry,
            resolve_adjusted_expiry(surface, uid, leg.maturity, strategy_template.projection,
                                    strategy_template.settlement));
    adjusted.push_back(AdjustedMaturity{leg.maturity, expiry});
  }
  return atx::core::Ok(std::move(adjusted));
}

[[nodiscard]] std::int64_t find_adjusted_expiry(const std::vector<AdjustedMaturity> &adjusted,
                                                const ProjectedMaturitySpec &maturity) noexcept {
  for (const AdjustedMaturity &known : adjusted) {
    if (known.requested == maturity) {
      return known.expiry_ts_ns;
    }
  }
  return 0;
}

} // namespace

Status validate_backtest_template(const BacktestStrategyTemplate &strategy_template) {
  if (strategy_template.id.empty() || strategy_template.name.empty() ||
      strategy_template.legs.empty() || strategy_template.entry_every_n == 0u) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "backtest template: id, name, legs, and cadence are required");
  }
  switch (strategy_template.holding) {
  case BacktestHoldingRule::HoldToExpiry:
    break;
  default:
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "backtest template: unsupported holding rule");
  }
  switch (strategy_template.settlement) {
  case TheoreticalSettlementRule::FollowingNyseSessionSnapshot:
    break;
  default:
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "backtest template: unsupported settlement rule");
  }
  if (!valid_hedge(strategy_template.hedge) || !valid_projection(strategy_template.projection) ||
      !valid_frictions(strategy_template.frictions)) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "backtest template: invalid hedge, projection, or frictions");
  }
  for (const BacktestTemplateLeg &leg : strategy_template.legs) {
    if (!valid_maturity(leg.maturity) || !valid_strike(leg.strike) || !valid_side(leg.side) ||
        !finite_positive(leg.multiplier) || !(std::isfinite(leg.quantity) && leg.quantity != 0.0) ||
        !std::isfinite(leg.quantity * leg.multiplier)) {
      return atx::core::Err(ErrorCode::InvalidArgument, "backtest template: invalid leg economics");
    }
  }
  return atx::core::Ok();
}

std::uint64_t fingerprint_backtest_template(const BacktestStrategyTemplate &strategy_template) {
  if (!validate_backtest_template(strategy_template)) {
    return 0u;
  }

  StableFingerprint hash;
  hash.add_u64(kBacktestTemplateEngineSchemaSalt);
  hash.add_u64(static_cast<std::uint64_t>(strategy_template.legs.size()));
  for (const BacktestTemplateLeg &leg : strategy_template.legs) {
    fingerprint_maturity(hash, leg.maturity);
    fingerprint_strike(hash, leg.strike);
    hash.add_u64(static_cast<std::uint64_t>(leg.side));
    hash.add_double(leg.quantity);
    hash.add_double(leg.multiplier);
  }
  hash.add_u64(strategy_template.entry_every_n);
  hash.add_u64(static_cast<std::uint64_t>(strategy_template.holding));
  hash.add_u64(static_cast<std::uint64_t>(strategy_template.hedge.kind));
  hash.add_u64(static_cast<std::uint64_t>(strategy_template.hedge.cadence));
  hash.add_double(strategy_template.hedge.band);
  hash.add_bool(strategy_template.projection.analytic_greeks);
  hash.add_double(strategy_template.projection.delta_tolerance);
  hash.add_u64(static_cast<std::uint64_t>(strategy_template.projection.query_execution));
  hash.add_u64(static_cast<std::uint64_t>(strategy_template.frictions.spread_kind));
  hash.add_double(strategy_template.frictions.half_spread_bps);
  hash.add_double(strategy_template.frictions.vol_tick);
  hash.add_double(strategy_template.frictions.impact_fraction);
  hash.add_double(strategy_template.frictions.per_contract_cost);
  hash.add_double(strategy_template.frictions.hedge_slippage_bps);
  hash.add_u64(static_cast<std::uint64_t>(strategy_template.settlement));
  return hash.finish();
}

Result<BacktestStrategyTemplate>
make_40_delta_3_calendar_month_strangle_template(double position_sign, unsigned entry_every_n) {
  if ((position_sign != 1.0 && position_sign != -1.0) || entry_every_n == 0u) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "backtest template: strangle sign must be +/-1 and cadence positive");
  }

  BacktestStrategyTemplate strategy_template;
  strategy_template.id = "strangle-40d-3cm-hold-expiry-daily-delta-v1";
  strategy_template.name = "40 Delta 3 Calendar Month Strangle";
  strategy_template.entry_every_n = entry_every_n;
  strategy_template.holding = BacktestHoldingRule::HoldToExpiry;
  strategy_template.hedge.kind = HedgeSpec::Kind::DeltaToZero;
  strategy_template.hedge.cadence = HedgeSpec::Cadence::Daily;
  strategy_template.hedge.band = 0.0;
  strategy_template.settlement = TheoreticalSettlementRule::FollowingNyseSessionSnapshot;

  BacktestTemplateLeg call;
  call.maturity = ProjectedMaturitySpec::months(3);
  call.strike = ProjectedStrikeSpec::delta(0.40);
  call.side = Side::Call;
  call.quantity = position_sign;
  call.multiplier = 100.0;
  BacktestTemplateLeg put = call;
  put.side = Side::Put;
  strategy_template.legs = {call, put};

  ATX_TRY_VOID(validate_backtest_template(strategy_template));
  return atx::core::Ok(std::move(strategy_template));
}

ProjectedTemplateStrategy::ProjectedTemplateStrategy(BacktestStrategyTemplate strategy_template,
                                                     std::uint32_t uid,
                                                     std::uint32_t initial_cohort_counter) noexcept
    : strategy_template_{std::move(strategy_template)}, uid_{uid}, referenced_uids_{uid},
      cohort_counter_{initial_cohort_counter} {}

Result<ProjectedTemplateStrategy>
ProjectedTemplateStrategy::create(BacktestStrategyTemplate strategy_template, std::uint32_t uid,
                                  std::uint32_t initial_cohort_counter) {
  ATX_TRY_VOID(validate_backtest_template(strategy_template));
  if (uid == 0u) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "backtest template strategy: uid must be nonzero");
  }
  return atx::core::Ok(
      ProjectedTemplateStrategy{std::move(strategy_template), uid, initial_cohort_counter});
}

Status ProjectedTemplateStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                          PortfolioState &book, std::uint64_t &next_lot_id) {
  return on_step(base, step_index, book, next_lot_id, PriceOptions{});
}

Status ProjectedTemplateStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                          PortfolioState &book, std::uint64_t &next_lot_id,
                                          const PriceOptions & /*price_options*/) {
  if ((step_index % strategy_template_.entry_every_n) != 0u) {
    return atx::core::Ok();
  }
  if (cohort_counter_ == std::numeric_limits<std::uint32_t>::max()) {
    return atx::core::Err(ErrorCode::OutOfRange,
                          "backtest template strategy: cohort counter exhausted");
  }
  if (strategy_template_.legs.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max() - next_lot_id)) {
    return atx::core::Err(ErrorCode::OutOfRange,
                          "backtest template strategy: lot id range exhausted");
  }

  const SurfaceRef surface = base.find(uid_);
  if (surface == nullptr) {
    return atx::core::Err(ErrorCode::Unavailable,
                          "backtest template strategy: projection surface unavailable");
  }
  ATX_TRY(const std::vector<AdjustedMaturity> expiries,
          adjusted_maturities(surface, uid_, strategy_template_));

  OptionProjectionConfig projection_config;
  projection_config.output = OptionProjectionOutput::FullGreeks;
  projection_config.analytic_greeks = strategy_template_.projection.analytic_greeks;
  projection_config.delta_tolerance = strategy_template_.projection.delta_tolerance;
  projection_config.query_execution = strategy_template_.projection.query_execution;

  std::vector<Lot> pending;
  pending.reserve(strategy_template_.legs.size());
  std::uint64_t lot_id = next_lot_id;
  for (const BacktestTemplateLeg &leg : strategy_template_.legs) {
    const std::int64_t expiry = find_adjusted_expiry(expiries, leg.maturity);
    if (expiry <= base.ts_ns()) {
      return atx::core::Err(ErrorCode::Internal,
                            "backtest template strategy: adjusted expiry lookup failed");
    }

    OptionProjectionSpec request;
    request.uid = uid_;
    request.maturity = ProjectedMaturitySpec::absolute(expiry);
    request.strike = leg.strike;
    request.side = leg.side;
    request.multiplier = leg.multiplier;
    ATX_TRY(const ProjectedOption projected,
            project_option_contract(surface, request, projection_config));
    if (projected.status != OptionProjectionStatus::Ok ||
        projected.definition.expiry_ts_ns != expiry ||
        !(std::isfinite(projected.model_mark) && projected.model_mark >= 0.0)) {
      return atx::core::Err(ErrorCode::Unavailable,
                            "backtest template strategy: projected leg is incomplete");
    }

    Lot lot;
    lot.id = lot_id++;
    lot.contract = projected.definition.contract;
    lot.qty = leg.quantity;
    lot.multiplier = projected.definition.multiplier;
    lot.expiry_ts_ns = projected.definition.expiry_ts_ns;
    lot.cohort = cohort_counter_;
    lot.entry_price = projected.model_mark;
    pending.push_back(lot);
  }

  if (pending.size() > std::numeric_limits<std::size_t>::max() - book.lots.size()) {
    return atx::core::Err(ErrorCode::OutOfRange, "backtest template strategy: book size exhausted");
  }
  book.lots.reserve(book.lots.size() + pending.size());
  book.lots.insert(book.lots.end(), pending.begin(), pending.end());
  next_lot_id += static_cast<std::uint64_t>(pending.size());
  ++cohort_counter_;
  return atx::core::Ok();
}

} // namespace atx::vol

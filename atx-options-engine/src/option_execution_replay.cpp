#include "atx/options/option_execution_replay.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/safe_math.hpp"

namespace atx::options::execution {
namespace {

using atx::core::Decimal;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

constexpr std::size_t kNoIndex = (std::numeric_limits<std::size_t>::max)();
constexpr std::int64_t kMaxExactlyRepresentableContracts = 9'007'199'254'740'991LL;
constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

[[nodiscard]] std::uint64_t fold_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
    hash ^= value & 0xFFU;
    hash *= kFnvPrime;
    value >>= 8U;
  }
  return hash;
}

[[nodiscard]] std::uint64_t fold_i64(std::uint64_t hash, std::int64_t value) noexcept {
  return fold_u64(hash, static_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t fold_order_request(std::uint64_t hash,
                                               const OptionOrderRequest &request) noexcept {
  hash = fold_u64(hash, request.order_id.value);
  hash = fold_u64(hash, request.strategy_id);
  hash = fold_u64(hash, request.basket_id);
  hash = fold_u64(hash, request.contract_id);
  hash = fold_u64(hash, request.engine_id.id);
  hash = fold_i64(hash, request.quantity_contracts);
  hash = fold_i64(hash, request.limit_price.raw());
  hash = fold_i64(hash, request.decision_ts_ns);
  hash = fold_i64(hash, request.arrival_ts_ns);
  hash = fold_i64(hash, request.expire_ts_ns);
  hash = fold_u64(hash, request.priority_sequence);
  hash = fold_u64(hash, request.fee_schedule_key);
  return fold_u64(hash, static_cast<std::uint64_t>(request.time_in_force));
}

[[nodiscard]] std::uint64_t fold_cancel_request(std::uint64_t hash,
                                                const OptionCancelRequest &request) noexcept {
  hash = fold_u64(hash, request.cancel_id.value);
  hash = fold_u64(hash, request.order_id.value);
  hash = fold_i64(hash, request.event_ts_ns);
  hash = fold_i64(hash, request.available_ts_ns);
  return fold_u64(hash, request.priority_sequence);
}

[[nodiscard]] bool has_identity(const atx::vol::ArchiveContentIdentity &identity) noexcept {
  return identity.file_size != 0U;
}

[[nodiscard]] Result<std::size_t> checked_add_size(std::size_t left, std::size_t right) {
  return atx::core::checked_add(left, right);
}

[[nodiscard]] Result<std::size_t> checked_mul_size(std::size_t left, std::size_t right) {
  return atx::core::checked_mul(left, right);
}

[[nodiscard]] Result<std::size_t> magnitude_as_size(std::int64_t value) {
  if (value == (std::numeric_limits<std::int64_t>::min)()) {
    return Err(ErrorCode::OutOfRange, "option quantity magnitude exceeds int64 range");
  }
  const std::uint64_t magnitude =
      value < 0 ? static_cast<std::uint64_t>(-value) : static_cast<std::uint64_t>(value);
  if (magnitude > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    return Err(ErrorCode::OutOfRange, "option quantity magnitude exceeds size_t range");
  }
  return Ok(static_cast<std::size_t>(magnitude));
}

[[nodiscard]] Result<Decimal> checked_scale_money(Decimal unit, std::int64_t count) {
  if (count < 0) {
    return Err(ErrorCode::InvalidArgument, "money scale count must be nonnegative");
  }
  ATX_TRY(std::int64_t raw, atx::core::checked_mul(unit.raw(), count));
  return Ok(Decimal::from_raw(raw));
}

[[nodiscard]] Result<Decimal> checked_money_product(Decimal left, Decimal right) {
  bool overflow = false;
  const std::int64_t raw =
      atx::core::detail::mul_div_128(left.raw(), right.raw(), Decimal::kScale, overflow);
  if (overflow) {
    return Err(ErrorCode::OutOfRange, "decimal product exceeds representable money range");
  }
  return Ok(Decimal::from_raw(raw));
}

[[nodiscard]] Result<Decimal> checked_premium(Decimal price, std::int64_t multiplier,
                                              std::int64_t contracts) {
  ATX_TRY(Decimal contract_price, checked_scale_money(price, multiplier));
  return checked_scale_money(contract_price, contracts);
}

[[nodiscard]] Result<void> add_money(Decimal &total, Decimal value) {
  ATX_TRY(Decimal next, total.checked_add(value));
  total = next;
  return Ok();
}

[[nodiscard]] Result<void> add_size(std::size_t &total, std::size_t value) {
  ATX_TRY(std::size_t next, checked_add_size(total, value));
  total = next;
  return Ok();
}

[[nodiscard]] bool is_buy(std::int64_t quantity) noexcept { return quantity > 0; }

[[nodiscard]] bool order_terminal(OptionOrderDisposition disposition) noexcept {
  switch (disposition) {
  case OptionOrderDisposition::Filled:
  case OptionOrderDisposition::Canceled:
  case OptionOrderDisposition::Expired:
    return true;
  case OptionOrderDisposition::OpenAtEnd:
    return false;
  }
  ATX_ASSERT(false);
  return true;
}

[[nodiscard]] bool quote_status_executable(OptionQuoteStatus status,
                                           bool allow_locked_market) noexcept {
  switch (status) {
  case OptionQuoteStatus::Firm:
    return true;
  case OptionQuoteStatus::Locked:
    return allow_locked_market;
  case OptionQuoteStatus::MissingQuote:
  case OptionQuoteStatus::MissingSize:
  case OptionQuoteStatus::Crossed:
  case OptionQuoteStatus::NonFirm:
  case OptionQuoteStatus::Halted:
    return false;
  }
  ATX_ASSERT(false);
  return false;
}

[[nodiscard]] Result<void> validate_scenario(OptionReplayScenario scenario) {
  switch (scenario) {
  case OptionReplayScenario::Strict:
  case OptionReplayScenario::Calibrated:
  case OptionReplayScenario::Stress:
    return Ok();
  }
  return Err(ErrorCode::InvalidArgument, "option replay scenario is invalid");
}

[[nodiscard]] Result<void> validate_tif(OptionTimeInForce tif) {
  switch (tif) {
  case OptionTimeInForce::FirstFutureQuoteOrCancel:
  case OptionTimeInForce::Day:
  case OptionTimeInForce::GoodTillCanceled:
    return Ok();
  }
  return Err(ErrorCode::InvalidArgument, "option time in force is invalid");
}

[[nodiscard]] Result<void> validate_quote_status(OptionQuoteStatus status) {
  switch (status) {
  case OptionQuoteStatus::Firm:
  case OptionQuoteStatus::Locked:
  case OptionQuoteStatus::MissingQuote:
  case OptionQuoteStatus::MissingSize:
  case OptionQuoteStatus::Crossed:
  case OptionQuoteStatus::NonFirm:
  case OptionQuoteStatus::Halted:
    return Ok();
  }
  return Err(ErrorCode::InvalidArgument, "option quote status is invalid");
}

[[nodiscard]] bool decimal_nonnegative(Decimal value) noexcept { return value.raw() >= 0; }

[[nodiscard]] Result<void> validate_quote_shape(const OptionTopOfBookEvent &quote) {
  ATX_TRY_VOID(validate_quote_status(quote.status));
  if (!quote.bid_updated && !quote.ask_updated) {
    return Err(ErrorCode::InvalidArgument, "option quote event must update at least one side");
  }
  if (quote.bid_size_contracts < 0 || quote.ask_size_contracts < 0 ||
      quote.bid_size_contracts > kMaxExactlyRepresentableContracts ||
      quote.ask_size_contracts > kMaxExactlyRepresentableContracts) {
    return Err(ErrorCode::InvalidArgument, "option quote displayed size is invalid");
  }

  const bool positive_bid = quote.bid.raw() > 0;
  const bool positive_ask = quote.ask.raw() > 0;
  switch (quote.status) {
  case OptionQuoteStatus::Firm:
    if (!positive_bid || !positive_ask || quote.bid >= quote.ask || quote.bid_size_contracts == 0 ||
        quote.ask_size_contracts == 0 || quote.bid_participant_id == 0U ||
        quote.ask_participant_id == 0U) {
      return Err(ErrorCode::InvalidArgument,
                 "firm option quote must have positive price, size, and participant identity");
    }
    break;
  case OptionQuoteStatus::Locked:
    if (!positive_bid || quote.bid != quote.ask || quote.bid_size_contracts == 0 ||
        quote.ask_size_contracts == 0 || quote.bid_participant_id == 0U ||
        quote.ask_participant_id == 0U) {
      return Err(ErrorCode::InvalidArgument,
                 "locked option quote must have equal positive bid and ask");
    }
    break;
  case OptionQuoteStatus::Crossed:
    if (!positive_bid || !positive_ask || quote.bid <= quote.ask) {
      return Err(ErrorCode::InvalidArgument,
                 "crossed option quote status must carry crossed positive prices");
    }
    break;
  case OptionQuoteStatus::MissingSize:
    if (!positive_bid || !positive_ask || quote.bid > quote.ask) {
      return Err(ErrorCode::InvalidArgument,
                 "missing-size option quote must retain a valid price pair");
    }
    break;
  case OptionQuoteStatus::MissingQuote:
  case OptionQuoteStatus::NonFirm:
  case OptionQuoteStatus::Halted:
    break;
  }
  return Ok();
}

[[nodiscard]] Result<std::int64_t> modeled_size(std::int64_t observed, Decimal fraction) {
  bool overflow = false;
  const std::int64_t result =
      atx::core::detail::mul_div_128(observed, fraction.raw(), Decimal::kScale, overflow);
  if (overflow || result < 0 || result > kMaxExactlyRepresentableContracts) {
    return Err(ErrorCode::OutOfRange, "modeled option displayed size exceeds integer range");
  }
  return Ok(result);
}

[[nodiscard]] Decimal tick_for_price(const OptionTickSchedule &schedule, Decimal price) noexcept {
  return schedule.price_threshold.raw() > 0 && price < schedule.price_threshold
             ? schedule.tick_below_threshold
             : schedule.tick_at_or_above_threshold;
}

[[nodiscard]] bool price_on_tick(const OptionTickSchedule &schedule, Decimal price) noexcept {
  const Decimal tick = tick_for_price(schedule, price);
  return price.raw() > 0 && tick.raw() > 0 && price.raw() % tick.raw() == 0;
}

struct PositiveQuotient {
  std::int64_t value{0};
  std::uint64_t remainder{0U};
};

[[nodiscard]] Result<PositiveQuotient>
positive_mul_div_with_remainder(std::int64_t left, std::int64_t right, std::int64_t divisor) {
  if (left <= 0 || right <= 0 || divisor <= 0) {
    return Err(ErrorCode::InvalidArgument,
               "positive multiply-divide operands must be strictly positive");
  }
  std::uint64_t high = 0U;
  std::uint64_t low = 0U;
  atx::core::detail::umul_64_to_128(static_cast<std::uint64_t>(left),
                                    static_cast<std::uint64_t>(right), high, low);
  std::uint64_t remainder = 0U;
  bool overflow = false;
  const std::uint64_t quotient = atx::core::detail::udiv_128_by_64(
      high, low, static_cast<std::uint64_t>(divisor), remainder, overflow);
  if (overflow ||
      quotient > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
    return Err(ErrorCode::OutOfRange, "exact option price multiply-divide overflowed");
  }
  return Ok(PositiveQuotient{static_cast<std::int64_t>(quotient), remainder});
}

[[nodiscard]] Result<Decimal> modeled_fill_price(Decimal touch, bool buy, Decimal adverse_price_bps,
                                                 const OptionTickSchedule &tick_schedule) {
  if (adverse_price_bps.raw() == 0) {
    return Ok(touch);
  }
  constexpr std::int64_t kBasisPointDenominator = 10'000 * Decimal::kScale;
  const std::int64_t numerator = buy ? kBasisPointDenominator + adverse_price_bps.raw()
                                     : kBasisPointDenominator - adverse_price_bps.raw();
  ATX_TRY(PositiveQuotient theoretical,
          positive_mul_div_with_remainder(touch.raw(), numerator, kBasisPointDenominator));
  if (theoretical.value <= 0) {
    return Err(ErrorCode::OutOfRange, "modeled option fill price is outside the exact tick grid");
  }
  const Decimal tick = tick_for_price(tick_schedule, Decimal::from_raw(theoretical.value));
  std::int64_t rounded_raw = 0;
  if (buy) {
    std::int64_t tick_count = theoretical.value / tick.raw();
    if (theoretical.value % tick.raw() != 0 || theoretical.remainder != 0U) {
      ATX_TRY(tick_count, atx::core::checked_add(tick_count, std::int64_t{1}));
    }
    ATX_TRY(rounded_raw, atx::core::checked_mul(tick_count, tick.raw()));
  } else {
    ATX_TRY(rounded_raw, atx::core::checked_mul(theoretical.value / tick.raw(), tick.raw()));
  }
  if (rounded_raw <= 0) {
    return Err(ErrorCode::OutOfRange, "modeled option fill price rounded to a nonpositive tick");
  }
  return Ok(Decimal::from_raw(rounded_raw));
}

[[nodiscard]] Result<Decimal> absolute_difference(Decimal left, Decimal right) {
  if (left >= right) {
    return left.checked_sub(right);
  }
  return right.checked_sub(left);
}

struct ReplayEvent {
  enum class Kind : std::uint8_t {
    Cancel = 0,
    OrderExpiry = 1,
    ContractExpiry = 2,
    Quote = 3,
    Submit = 4,
  };

  std::int64_t available_ts_ns{0};
  Kind kind{Kind::Quote};
  std::uint32_t rank_a{0};
  std::uint32_t rank_b{0};
  std::uint64_t rank_epoch{0};
  std::uint64_t rank_c{0};
  std::uint32_t rank_d{0};
  std::uint64_t rank_e{0};
  std::uint64_t stable_id{0};
  std::size_t index{0};
};

[[nodiscard]] bool event_less(const ReplayEvent &left, const ReplayEvent &right) noexcept {
  return std::tie(left.available_ts_ns, left.kind, left.rank_a, left.rank_b, left.rank_epoch,
                  left.rank_c, left.rank_d, left.rank_e, left.stable_id) <
         std::tie(right.available_ts_ns, right.kind, right.rank_a, right.rank_b, right.rank_epoch,
                  right.rank_c, right.rank_d, right.rank_e, right.stable_id);
}

struct OrderLookup {
  OptionOrderId id{};
  std::size_t index{0};
};

struct SideState {
  Decimal price{};
  std::int64_t observed_size_contracts{0};
  std::int64_t observed_modeled_size{0};
  std::int64_t remaining_size{0};
  std::uint16_t participant_id{0};
  std::size_t quote_index{kNoIndex};
  bool valid{false};
};

struct BookState {
  SideState bid{};
  SideState ask{};
  std::size_t buy_root{kNoIndex};
  std::size_t sell_root{kNoIndex};
  std::size_t live_head{kNoIndex};
  std::size_t live_tail{kNoIndex};
  std::size_t ioc_head{kNoIndex};
  std::size_t ioc_tail{kNoIndex};
};

struct OrderRuntime {
  std::size_t heap_child{kNoIndex};
  std::size_t heap_sibling{kNoIndex};
  std::size_t live_previous{kNoIndex};
  std::size_t live_next{kNoIndex};
  std::size_t ioc_next{kNoIndex};
  bool live{false};
  bool first_fill_charged{false};
  bool cancel_pending{false};
};

[[nodiscard]] Result<std::size_t> workspace_bytes(const OptionReplayLimits &limits,
                                                  std::size_t event_capacity) {
  std::size_t bytes = 0U;
  const auto add_array = [&bytes](std::size_t count, std::size_t element_size) -> Result<void> {
    ATX_TRY(std::size_t block, checked_mul_size(count, element_size));
    ATX_TRY(std::size_t next, checked_add_size(bytes, block));
    bytes = next;
    return Ok();
  };
  ATX_TRY_VOID(add_array(limits.max_contracts, sizeof(OptionPositionSnapshot)));
  ATX_TRY_VOID(add_array(limits.max_contracts, sizeof(OptionReplayContract)));
  ATX_TRY_VOID(add_array(limits.max_contracts, sizeof(std::uint32_t)));
  ATX_TRY_VOID(add_array(limits.max_contracts, sizeof(BookState)));
  ATX_TRY_VOID(add_array(limits.max_quote_events, sizeof(OptionTopOfBookEvent)));
  ATX_TRY_VOID(add_array(limits.max_orders, sizeof(OptionOrderAudit)));
  ATX_TRY_VOID(add_array(limits.max_orders, sizeof(OrderRuntime)));
  ATX_TRY_VOID(add_array(limits.max_orders, sizeof(OrderLookup)));
  ATX_TRY_VOID(add_array(limits.max_cancellations, sizeof(OptionCancelAudit)));
  ATX_TRY_VOID(add_array(limits.max_cancellations, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(limits.max_fee_rows, sizeof(OptionFeeSchedule)));
  ATX_TRY_VOID(add_array(limits.max_tick_rows, sizeof(OptionTickSchedule)));
  ATX_TRY_VOID(add_array(event_capacity, sizeof(ReplayEvent)));
  ATX_TRY_VOID(add_array(limits.max_fills, sizeof(OptionFill)));
  return Ok(bytes);
}

} // namespace

struct ReplayCore {
  explicit ReplayCore(OptionReplayLimits configured_limits) : limits{configured_limits} {}

  OptionReplayLimits limits{};
  OptionReplayConfig config{};
  std::vector<OptionReplayContract> contracts;
  std::vector<std::uint32_t> engine_ids;
  std::vector<OptionPositionSnapshot> positions;
  std::vector<BookState> books;
  std::vector<OptionTopOfBookEvent> quotes;
  std::vector<OptionOrderAudit> orders;
  std::vector<OrderRuntime> order_runtime;
  std::vector<OrderLookup> order_lookup;
  std::vector<OptionCancelAudit> cancellations;
  // Target resolved when the cancel request is accepted. kNoIndex pins an
  // unknown-order outcome so a later order cannot inherit an earlier cancel.
  std::vector<std::size_t> cancellation_targets;
  std::vector<OptionFeeSchedule> fee_schedules;
  std::vector<OptionTickSchedule> tick_schedules;
  std::vector<ReplayEvent> events;
  std::vector<OptionFill> fills;
  std::vector<ReplayEvent> pending_events;
  std::vector<OptionOrderRequest> command_orders;
  std::vector<OptionCancelRequest> command_cancellations;
  std::vector<std::uint64_t> command_cancel_targets;
  std::vector<std::int64_t> command_exposure_deltas;
  std::vector<std::int64_t> command_pending_cancel_values;
  std::vector<std::uint64_t> command_contract_epochs;
  std::vector<std::size_t> command_touched_contracts;
  std::vector<OptionOrderStateSnapshot> order_states;
  std::vector<OptionContractExposureSnapshot> exposures;
  std::vector<OptionOrderTransition> transitions;
  OptionReplaySummary summary{};
  OptionExecutionSessionSummary session_summary{};
  std::size_t working_orders{0U};
  std::size_t static_event_index{0U};
  std::size_t new_fill_begin{0U};
  std::size_t new_transition_begin{0U};
  std::size_t max_frontiers{0U};
  std::size_t max_transitions{0U};
  std::size_t reserved_transitions{0U};
  std::uint64_t command_epoch{0U};
  bool session_enabled{false};

  void clear() noexcept {
    contracts.clear();
    engine_ids.clear();
    positions.clear();
    books.clear();
    quotes.clear();
    orders.clear();
    order_runtime.clear();
    order_lookup.clear();
    cancellations.clear();
    cancellation_targets.clear();
    fee_schedules.clear();
    tick_schedules.clear();
    events.clear();
    fills.clear();
    pending_events.clear();
    command_orders.clear();
    command_cancellations.clear();
    command_cancel_targets.clear();
    command_exposure_deltas.clear();
    command_pending_cancel_values.clear();
    command_touched_contracts.clear();
    order_states.clear();
    exposures.clear();
    transitions.clear();
    summary = {};
    session_summary = {};
    working_orders = 0U;
    static_event_index = 0U;
    new_fill_begin = 0U;
    new_transition_begin = 0U;
    reserved_transitions = 0U;
  }

  [[nodiscard]] std::size_t contract_index(std::uint64_t contract_id) const noexcept {
    const auto found =
        std::lower_bound(positions.begin(), positions.end(), contract_id,
                         [](const OptionPositionSnapshot &position, std::uint64_t id) noexcept {
                           return position.contract_id < id;
                         });
    if (found == positions.end() || found->contract_id != contract_id) {
      return kNoIndex;
    }
    return static_cast<std::size_t>(found - positions.begin());
  }

  [[nodiscard]] const OptionReplayContract *
  contract_definition(std::uint64_t contract_id) const noexcept {
    const auto found =
        std::lower_bound(contracts.begin(), contracts.end(), contract_id,
                         [](const OptionReplayContract &contract, std::uint64_t id) noexcept {
                           return contract.contract_id < id;
                         });
    return found == contracts.end() || found->contract_id != contract_id ? nullptr : &*found;
  }

  [[nodiscard]] std::size_t order_index(OptionOrderId order_id) const noexcept {
    const auto found = std::lower_bound(
        order_lookup.begin(), order_lookup.end(), order_id,
        [](const OrderLookup &lookup, OptionOrderId id) noexcept { return lookup.id < id; });
    if (found == order_lookup.end() || found->id != order_id) {
      return kNoIndex;
    }
    return found->index;
  }

  [[nodiscard]] const OptionFeeSchedule *fee_at(std::uint32_t key,
                                                std::int64_t fill_ts_ns) const noexcept {
    const auto upper = std::upper_bound(
        fee_schedules.begin(), fee_schedules.end(), std::pair{key, fill_ts_ns},
        [](const auto &candidate, const OptionFeeSchedule &schedule) noexcept {
          return candidate < std::pair{schedule.key, schedule.effective_from_ts_ns};
        });
    if (upper == fee_schedules.begin()) {
      return nullptr;
    }
    const OptionFeeSchedule &candidate = *std::prev(upper);
    return candidate.key == key && candidate.effective_from_ts_ns <= fill_ts_ns &&
                   fill_ts_ns < candidate.effective_until_ts_ns
               ? &candidate
               : nullptr;
  }

  [[nodiscard]] const OptionTickSchedule *tick_at(std::uint32_t key,
                                                  std::int64_t ts_ns) const noexcept {
    const auto upper = std::upper_bound(
        tick_schedules.begin(), tick_schedules.end(), std::pair{key, ts_ns},
        [](const auto &candidate, const OptionTickSchedule &schedule) noexcept {
          return candidate < std::pair{schedule.key, schedule.effective_from_ts_ns};
        });
    if (upper == tick_schedules.begin()) {
      return nullptr;
    }
    const OptionTickSchedule &candidate = *std::prev(upper);
    return candidate.key == key && candidate.effective_from_ts_ns <= ts_ns &&
                   ts_ns < candidate.effective_until_ts_ns
               ? &candidate
               : nullptr;
  }

  [[nodiscard]] bool has_fee_key(std::uint32_t key) const noexcept {
    const auto found =
        std::lower_bound(fee_schedules.begin(), fee_schedules.end(), key,
                         [](const OptionFeeSchedule &schedule, std::uint32_t candidate) noexcept {
                           return schedule.key < candidate;
                         });
    return found != fee_schedules.end() && found->key == key;
  }

  [[nodiscard]] Result<void> validate_config(const OptionReplayConfig &candidate) const {
    ATX_TRY_VOID(validate_scenario(candidate.scenario));
    if (candidate.model_version != kOptionExecutionReplayModelVersion ||
        !has_identity(candidate.market_data_identity) || !candidate.sequence_continuity_verified ||
        !has_identity(candidate.sequence_validation_identity) ||
        candidate.displayed_size_fraction.raw() <= 0 ||
        candidate.displayed_size_fraction.raw() > Decimal::kScale ||
        candidate.adverse_price_bps.raw() < 0 ||
        candidate.adverse_price_bps >= Decimal::from_int(10'000) ||
        candidate.max_quote_age_ns < 0 || candidate.replay_end_ts_ns <= 0) {
      return Err(ErrorCode::InvalidArgument, "option replay configuration is invalid");
    }
    if ((candidate.scenario == OptionReplayScenario::Strict ||
         candidate.scenario == OptionReplayScenario::Stress) &&
        candidate.displayed_size_fraction > Decimal::from_raw(Decimal::kScale / 4)) {
      return Err(ErrorCode::InvalidArgument,
                 "strict and stress replay cannot use more than 25% of displayed size");
    }
    if (candidate.scenario == OptionReplayScenario::Calibrated &&
        !has_identity(candidate.calibration_identity)) {
      return Err(ErrorCode::InvalidArgument,
                 "calibrated option replay requires frozen calibration identity");
    }
    return Ok();
  }

  [[nodiscard]] Result<void> load_contracts(std::span<const OptionReplayContract> input_contracts) {
    if (input_contracts.empty() || input_contracts.size() > limits.max_contracts) {
      return Err(ErrorCode::OutOfRange, "option replay contract limit exceeded");
    }
    contracts.assign(input_contracts.begin(), input_contracts.end());
    std::sort(contracts.begin(), contracts.end(),
              [](const OptionReplayContract &left, const OptionReplayContract &right) noexcept {
                return left.contract_id < right.contract_id;
              });
    for (const OptionReplayContract &contract : contracts) {
      if (contract.contract_id == 0U || contract.engine_id.id == 0U || contract.multiplier <= 0 ||
          contract.initial_contracts == (std::numeric_limits<std::int64_t>::min)() ||
          contract.tick_schedule_key == 0U || contract.definition_effective_ts_ns < 0 ||
          contract.definition_available_ts_ns < contract.definition_effective_ts_ns ||
          contract.expiry_ts_ns <= contract.definition_available_ts_ns ||
          !has_identity(contract.definition_source_identity)) {
        return Err(ErrorCode::InvalidArgument, "option replay contract definition is invalid");
      }
      positions.push_back(OptionPositionSnapshot{contract.contract_id, contract.engine_id,
                                                 contract.multiplier, contract.initial_contracts});
    }
    for (std::size_t i = 1U; i < contracts.size(); ++i) {
      if (contracts[i - 1U].contract_id == contracts[i].contract_id) {
        return Err(ErrorCode::AlreadyExists, "duplicate option replay contract id");
      }
    }
    for (const OptionPositionSnapshot &position : positions) {
      engine_ids.push_back(position.engine_id.id);
    }
    std::sort(engine_ids.begin(), engine_ids.end());
    if (std::adjacent_find(engine_ids.begin(), engine_ids.end()) != engine_ids.end()) {
      return Err(ErrorCode::AlreadyExists,
                 "distinct option replay contracts cannot alias engine id");
    }
    books.resize(positions.size());
    return Ok();
  }

  [[nodiscard]] Result<void> load_quotes(std::span<const OptionTopOfBookEvent> input_quotes) {
    if (input_quotes.size() > limits.max_quote_events) {
      return Err(ErrorCode::OutOfRange, "option replay quote-event limit exceeded");
    }
    quotes.assign(input_quotes.begin(), input_quotes.end());
    for (const OptionTopOfBookEvent &quote : quotes) {
      const OptionReplayContract *definition = contract_definition(quote.contract_id);
      if (definition == nullptr || quote.engine_id != definition->engine_id) {
        return Err(ErrorCode::InvalidArgument, "option quote does not match the execution catalog");
      }
      if (quote.quote_event_ts_ns < definition->definition_effective_ts_ns ||
          quote.available_ts_ns < definition->definition_available_ts_ns ||
          quote.quote_event_ts_ns < 0 || quote.available_ts_ns < quote.quote_event_ts_ns ||
          quote.available_ts_ns > config.replay_end_ts_ns ||
          quote.quote_event_ts_ns >= definition->expiry_ts_ns ||
          quote.available_ts_ns >= definition->expiry_ts_ns || quote.order_key.stream_epoch == 0U ||
          quote.order_key.native_sequence == 0U || quote.order_key.stable_ingest_ordinal == 0U ||
          !has_identity(quote.source_identity)) {
        return Err(ErrorCode::InvalidArgument, "option quote clock or lineage is invalid");
      }
      ATX_TRY_VOID(validate_quote_shape(quote));
      const OptionTickSchedule *event_tick =
          tick_at(definition->tick_schedule_key, quote.quote_event_ts_ns);
      const OptionTickSchedule *available_tick =
          tick_at(definition->tick_schedule_key, quote.available_ts_ns);
      if (event_tick == nullptr || available_tick == nullptr) {
        return Err(ErrorCode::NotFound, "no effective option tick schedule at quote timestamp");
      }
      if (event_tick != available_tick) {
        return Err(ErrorCode::InvalidArgument,
                   "option quote availability straddles a tick-rule transition");
      }
      if ((quote.bid.raw() > 0 && !price_on_tick(*event_tick, quote.bid)) ||
          (quote.ask.raw() > 0 && !price_on_tick(*event_tick, quote.ask))) {
        return Err(ErrorCode::InvalidArgument, "option quote price is off its effective tick");
      }
    }
    std::sort(quotes.begin(), quotes.end(),
              [](const OptionTopOfBookEvent &left, const OptionTopOfBookEvent &right) noexcept {
                return std::tie(left.order_key.source_rank, left.order_key.channel_id,
                                left.order_key.stream_epoch, left.order_key.native_sequence,
                                left.order_key.packet_index, left.order_key.stable_ingest_ordinal) <
                       std::tie(right.order_key.source_rank, right.order_key.channel_id,
                                right.order_key.stream_epoch, right.order_key.native_sequence,
                                right.order_key.packet_index,
                                right.order_key.stable_ingest_ordinal);
              });
    for (std::size_t i = 1U; i < quotes.size(); ++i) {
      const OptionTopOfBookEvent &previous = quotes[i - 1U];
      const OptionTopOfBookEvent &current = quotes[i];
      const bool same_channel = previous.order_key.source_rank == current.order_key.source_rank &&
                                previous.order_key.channel_id == current.order_key.channel_id;
      const bool same_stream =
          same_channel && previous.order_key.stream_epoch == current.order_key.stream_epoch;
      if (same_stream && previous.order_key.native_sequence == current.order_key.native_sequence &&
          previous.order_key.packet_index == current.order_key.packet_index) {
        return Err(ErrorCode::AlreadyExists, "duplicate option native market record key");
      }
      if (same_channel && current.available_ts_ns < previous.available_ts_ns) {
        return Err(ErrorCode::InvalidArgument,
                   "option native market epoch or sequence regresses in availability time");
      }
    }
    std::sort(quotes.begin(), quotes.end(),
              [](const OptionTopOfBookEvent &left, const OptionTopOfBookEvent &right) noexcept {
                return std::tie(left.available_ts_ns, left.order_key.source_rank,
                                left.order_key.channel_id, left.order_key.stream_epoch,
                                left.order_key.native_sequence, left.order_key.packet_index,
                                left.order_key.stable_ingest_ordinal) <
                       std::tie(right.available_ts_ns, right.order_key.source_rank,
                                right.order_key.channel_id, right.order_key.stream_epoch,
                                right.order_key.native_sequence, right.order_key.packet_index,
                                right.order_key.stable_ingest_ordinal);
              });
    return Ok();
  }

  [[nodiscard]] Result<void>
  load_fee_schedules(std::span<const OptionFeeSchedule> input_schedules) {
    if (input_schedules.size() > limits.max_fee_rows) {
      return Err(ErrorCode::OutOfRange, "option replay fee-row limit exceeded");
    }
    fee_schedules.assign(input_schedules.begin(), input_schedules.end());
    for (const OptionFeeSchedule &schedule : fee_schedules) {
      if (schedule.key == 0U || schedule.available_ts_ns < 0 ||
          schedule.available_ts_ns > schedule.effective_from_ts_ns ||
          schedule.effective_from_ts_ns < 0 ||
          schedule.effective_until_ts_ns <= schedule.effective_from_ts_ns ||
          !decimal_nonnegative(schedule.clearing_per_contract) ||
          !decimal_nonnegative(schedule.regulatory_per_contract) ||
          !decimal_nonnegative(schedule.commission_per_contract) ||
          !decimal_nonnegative(schedule.commission_per_order) ||
          schedule.sell_premium_rate.raw() < 0 ||
          schedule.sell_premium_rate > Decimal::from_int(1) ||
          !has_identity(schedule.source_identity)) {
        return Err(ErrorCode::InvalidArgument, "option fee schedule row is invalid");
      }
    }
    std::sort(fee_schedules.begin(), fee_schedules.end(),
              [](const OptionFeeSchedule &left, const OptionFeeSchedule &right) noexcept {
                return std::tie(left.key, left.effective_from_ts_ns, left.effective_until_ts_ns) <
                       std::tie(right.key, right.effective_from_ts_ns, right.effective_until_ts_ns);
              });
    for (std::size_t i = 1U; i < fee_schedules.size(); ++i) {
      if (fee_schedules[i - 1U].key == fee_schedules[i].key &&
          fee_schedules[i - 1U].effective_until_ts_ns > fee_schedules[i].effective_from_ts_ns) {
        return Err(ErrorCode::AlreadyExists, "overlapping option fee schedule intervals");
      }
    }
    return Ok();
  }

  [[nodiscard]] Result<void>
  load_tick_schedules(std::span<const OptionTickSchedule> input_schedules) {
    if (input_schedules.empty() || input_schedules.size() > limits.max_tick_rows) {
      return Err(ErrorCode::OutOfRange, "option replay tick-row limit exceeded");
    }
    tick_schedules.assign(input_schedules.begin(), input_schedules.end());
    for (const OptionTickSchedule &schedule : tick_schedules) {
      if (schedule.key == 0U || schedule.available_ts_ns < 0 ||
          schedule.available_ts_ns > schedule.effective_from_ts_ns ||
          schedule.effective_from_ts_ns < 0 ||
          schedule.effective_until_ts_ns <= schedule.effective_from_ts_ns ||
          schedule.price_threshold.raw() < 0 || schedule.tick_below_threshold.raw() <= 0 ||
          schedule.tick_at_or_above_threshold.raw() <= 0 ||
          (schedule.price_threshold.raw() > 0 &&
           (schedule.price_threshold.raw() % schedule.tick_below_threshold.raw() != 0 ||
            schedule.price_threshold.raw() % schedule.tick_at_or_above_threshold.raw() != 0)) ||
          !has_identity(schedule.source_identity)) {
        return Err(ErrorCode::InvalidArgument, "option tick schedule row is invalid");
      }
    }
    std::sort(tick_schedules.begin(), tick_schedules.end(),
              [](const OptionTickSchedule &left, const OptionTickSchedule &right) noexcept {
                return std::tie(left.key, left.effective_from_ts_ns, left.effective_until_ts_ns) <
                       std::tie(right.key, right.effective_from_ts_ns, right.effective_until_ts_ns);
              });
    for (std::size_t i = 1U; i < tick_schedules.size(); ++i) {
      if (tick_schedules[i - 1U].key == tick_schedules[i].key &&
          tick_schedules[i - 1U].effective_until_ts_ns > tick_schedules[i].effective_from_ts_ns) {
        return Err(ErrorCode::AlreadyExists, "overlapping option tick schedule intervals");
      }
    }
    for (const OptionReplayContract &contract : contracts) {
      const auto found =
          std::lower_bound(tick_schedules.begin(), tick_schedules.end(), contract.tick_schedule_key,
                           [](const OptionTickSchedule &schedule, std::uint32_t key) noexcept {
                             return schedule.key < key;
                           });
      if (found == tick_schedules.end() || found->key != contract.tick_schedule_key) {
        return Err(ErrorCode::NotFound, "option contract tick schedule key is not configured");
      }
    }
    return Ok();
  }

  [[nodiscard]] Result<std::size_t>
  validate_order_request(const OptionOrderRequest &request) const {
    const OptionReplayContract *definition = contract_definition(request.contract_id);
    if (definition == nullptr || request.engine_id != definition->engine_id) {
      return Err(ErrorCode::InvalidArgument, "option order does not match the execution catalog");
    }
    ATX_TRY_VOID(validate_tif(request.time_in_force));
    if (request.order_id.value == 0U || request.strategy_id == 0U || request.basket_id == 0U ||
        request.quantity_contracts == 0 ||
        request.quantity_contracts == (std::numeric_limits<std::int64_t>::min)() ||
        request.limit_price.raw() <= 0 ||
        request.decision_ts_ns < definition->definition_available_ts_ns ||
        request.arrival_ts_ns <= request.decision_ts_ns ||
        request.arrival_ts_ns >= definition->expiry_ts_ns ||
        request.arrival_ts_ns > config.replay_end_ts_ns || request.fee_schedule_key == 0U) {
      return Err(ErrorCode::InvalidArgument, "option order request is invalid");
    }
    if ((request.time_in_force == OptionTimeInForce::Day &&
         (request.expire_ts_ns <= request.arrival_ts_ns ||
          request.expire_ts_ns > definition->expiry_ts_ns)) ||
        (request.time_in_force != OptionTimeInForce::Day && request.expire_ts_ns != 0)) {
      return Err(ErrorCode::InvalidArgument,
                 "option order expiry is inconsistent with time in force");
    }
    if (!has_fee_key(request.fee_schedule_key)) {
      return Err(ErrorCode::NotFound, "option order fee schedule key is not configured");
    }
    const OptionTickSchedule *tick = tick_at(definition->tick_schedule_key, request.arrival_ts_ns);
    if (tick == nullptr) {
      return Err(ErrorCode::NotFound, "no effective option tick schedule at order arrival");
    }
    if (tick->available_ts_ns > request.decision_ts_ns) {
      return Err(ErrorCode::InvalidArgument,
                 "option order uses a tick rule unavailable at decision time");
    }
    if (!price_on_tick(*tick, request.limit_price)) {
      return Err(ErrorCode::InvalidArgument, "option order limit is off its effective tick");
    }
    return magnitude_as_size(request.quantity_contracts);
  }

  [[nodiscard]] Result<void> load_orders(std::span<const OptionOrderRequest> input_orders) {
    if (input_orders.size() > limits.max_orders) {
      return Err(ErrorCode::OutOfRange, "option replay order limit exceeded");
    }
    for (const OptionOrderRequest &request : input_orders) {
      ATX_TRY(std::size_t requested, validate_order_request(request));
      ATX_TRY_VOID(add_size(summary.requested_contracts, requested));
      orders.push_back(OptionOrderAudit{request, 0, request.quantity_contracts, 0U, Decimal{}, 0, 0,
                                        OptionOrderDisposition::OpenAtEnd});
    }
    std::sort(orders.begin(), orders.end(),
              [](const OptionOrderAudit &left, const OptionOrderAudit &right) noexcept {
                return std::tie(left.request.arrival_ts_ns, left.request.priority_sequence,
                                left.request.order_id.value) <
                       std::tie(right.request.arrival_ts_ns, right.request.priority_sequence,
                                right.request.order_id.value);
              });
    order_runtime.resize(orders.size());
    for (std::size_t i = 0; i < orders.size(); ++i) {
      order_lookup.push_back(OrderLookup{orders[i].request.order_id, i});
    }
    std::sort(order_lookup.begin(), order_lookup.end(),
              [](const OrderLookup &left, const OrderLookup &right) noexcept {
                return left.id < right.id;
              });
    if (std::adjacent_find(order_lookup.begin(), order_lookup.end(),
                           [](const OrderLookup &left, const OrderLookup &right) noexcept {
                             return left.id == right.id;
                           }) != order_lookup.end()) {
      return Err(ErrorCode::AlreadyExists, "duplicate option replay order id");
    }
    summary.open_orders = orders.size();
    return Ok();
  }

  [[nodiscard]] Result<void>
  load_cancellations(std::span<const OptionCancelRequest> input_cancellations) {
    if (input_cancellations.size() > limits.max_cancellations) {
      return Err(ErrorCode::OutOfRange, "option replay cancellation limit exceeded");
    }
    for (const OptionCancelRequest &request : input_cancellations) {
      if (request.cancel_id.value == 0U || request.order_id.value == 0U ||
          request.event_ts_ns < 0 || request.available_ts_ns < request.event_ts_ns ||
          request.available_ts_ns > config.replay_end_ts_ns) {
        return Err(ErrorCode::InvalidArgument, "option cancel request is invalid");
      }
      const std::size_t target = order_index(request.order_id);
      if (target != kNoIndex && request.available_ts_ns <= orders[target].request.arrival_ts_ns) {
        return Err(ErrorCode::InvalidArgument,
                   "option cancel must become effective after order arrival");
      }
      cancellations.push_back(OptionCancelAudit{request, OptionCancelDisposition::AlreadyTerminal});
    }
    std::sort(cancellations.begin(), cancellations.end(),
              [](const OptionCancelAudit &left, const OptionCancelAudit &right) noexcept {
                return left.request.cancel_id < right.request.cancel_id;
              });
    for (std::size_t i = 1U; i < cancellations.size(); ++i) {
      if (cancellations[i - 1U].request.cancel_id == cancellations[i].request.cancel_id) {
        return Err(ErrorCode::AlreadyExists, "duplicate option cancel id");
      }
    }
    std::sort(cancellations.begin(), cancellations.end(),
              [](const OptionCancelAudit &left, const OptionCancelAudit &right) noexcept {
                return std::tie(left.request.available_ts_ns, left.request.priority_sequence,
                                left.request.cancel_id.value) <
                       std::tie(right.request.available_ts_ns, right.request.priority_sequence,
                                right.request.cancel_id.value);
              });
    for (const OptionCancelAudit &audit : cancellations) {
      cancellation_targets.push_back(order_index(audit.request.order_id));
    }
    return Ok();
  }

  void add_events() {
    for (std::size_t i = 0; i < cancellations.size(); ++i) {
      const auto &request = cancellations[i].request;
      events.push_back(ReplayEvent{request.available_ts_ns, ReplayEvent::Kind::Cancel, 0U, 0U, 0U,
                                   request.priority_sequence, 0U, request.cancel_id.value,
                                   request.order_id.value, i});
    }
    for (std::size_t i = 0; i < orders.size(); ++i) {
      const auto &request = orders[i].request;
      events.push_back(ReplayEvent{request.arrival_ts_ns, ReplayEvent::Kind::Submit, 0U, 0U, 0U,
                                   request.priority_sequence, 0U, request.order_id.value,
                                   request.contract_id, i});
      if (request.time_in_force == OptionTimeInForce::Day &&
          request.expire_ts_ns <= config.replay_end_ts_ns) {
        events.push_back(ReplayEvent{request.expire_ts_ns, ReplayEvent::Kind::OrderExpiry, 0U, 0U,
                                     0U, request.priority_sequence, 0U, request.order_id.value,
                                     request.contract_id, i});
      }
    }
    for (std::size_t i = 0; i < contracts.size(); ++i) {
      if (contracts[i].expiry_ts_ns <= config.replay_end_ts_ns) {
        events.push_back(ReplayEvent{contracts[i].expiry_ts_ns, ReplayEvent::Kind::ContractExpiry,
                                     0U, 0U, 0U, 0U, 0U, contracts[i].contract_id,
                                     contracts[i].contract_id, i});
      }
    }
    for (std::size_t i = 0; i < quotes.size(); ++i) {
      const auto &quote = quotes[i];
      events.push_back(ReplayEvent{quote.available_ts_ns, ReplayEvent::Kind::Quote,
                                   quote.order_key.source_rank, quote.order_key.channel_id,
                                   quote.order_key.stream_epoch, quote.order_key.native_sequence,
                                   quote.order_key.packet_index,
                                   quote.order_key.stable_ingest_ordinal, quote.contract_id, i});
    }
    std::sort(events.begin(), events.end(), event_less);
  }

  [[nodiscard]] static bool pending_event_greater(const ReplayEvent &left,
                                                  const ReplayEvent &right) noexcept {
    return event_less(right, left);
  }

  void push_pending_event(const ReplayEvent &event) {
    pending_events.push_back(event);
    std::push_heap(pending_events.begin(), pending_events.end(), pending_event_greater);
  }

  [[nodiscard]] ReplayEvent pop_pending_event() {
    std::pop_heap(pending_events.begin(), pending_events.end(), pending_event_greater);
    const ReplayEvent event = pending_events.back();
    pending_events.pop_back();
    return event;
  }

  [[nodiscard]] Result<void> process_through(std::int64_t frontier_ts_ns) {
    ATX_TRY(std::size_t max_steps, checked_add_size(events.size(), pending_events.size()));
    Result<void> status = Ok();
    for (std::size_t bounded = 0U; bounded < max_steps; ++bounded) {
      const bool has_static = static_event_index < events.size();
      const bool has_pending = !pending_events.empty();
      if (!has_static && !has_pending) {
        break;
      }
      const bool use_static =
          has_static &&
          (!has_pending || !event_less(pending_events.front(), events[static_event_index]));
      const ReplayEvent &next = use_static ? events[static_event_index] : pending_events.front();
      if (next.available_ts_ns > frontier_ts_ns) {
        break;
      }
      const ReplayEvent event = use_static ? events[static_event_index++] : pop_pending_event();
      process_event(event, status);
      if (!status) {
        return status;
      }
    }
    return Ok();
  }

  [[nodiscard]] bool better_order(std::size_t left_index, std::size_t right_index,
                                  bool buy) const noexcept {
    const OptionOrderRequest &left = orders[left_index].request;
    const OptionOrderRequest &right = orders[right_index].request;
    if (left.limit_price != right.limit_price) {
      return buy ? left.limit_price > right.limit_price : left.limit_price < right.limit_price;
    }
    return std::tie(left.arrival_ts_ns, left.priority_sequence, left.order_id.value) <
           std::tie(right.arrival_ts_ns, right.priority_sequence, right.order_id.value);
  }

  [[nodiscard]] OptionOrderLifecycleState
  lifecycle_state(std::size_t order_index_value) const noexcept {
    const OptionOrderAudit &audit = orders[order_index_value];
    const OrderRuntime &runtime = order_runtime[order_index_value];
    switch (audit.disposition) {
    case OptionOrderDisposition::Filled:
      return OptionOrderLifecycleState::Filled;
    case OptionOrderDisposition::Canceled:
      return OptionOrderLifecycleState::Canceled;
    case OptionOrderDisposition::Expired:
      return OptionOrderLifecycleState::Expired;
    case OptionOrderDisposition::OpenAtEnd:
      break;
    }
    if (runtime.cancel_pending) {
      return OptionOrderLifecycleState::PendingCancel;
    }
    if (!runtime.live) {
      return OptionOrderLifecycleState::Scheduled;
    }
    return audit.filled_contracts == 0 ? OptionOrderLifecycleState::Working
                                       : OptionOrderLifecycleState::PartiallyFilled;
  }

  void sync_order_state(std::size_t order_index_value) noexcept {
    if (!session_enabled) {
      return;
    }
    OptionOrderStateSnapshot &snapshot = order_states[order_index_value];
    snapshot.remaining_contracts = orders[order_index_value].remaining_contracts;
    snapshot.state = lifecycle_state(order_index_value);
    snapshot.cancel_pending = order_runtime[order_index_value].cancel_pending;
  }

  [[nodiscard]] Result<void>
  append_transition(std::int64_t event_ts_ns, std::int64_t available_ts_ns,
                    std::size_t order_index_value, OptionCancelId cancel_id,
                    OptionOrderTransitionKind kind, OptionOrderLifecycleState state_before,
                    std::int64_t last_fill_contracts = 0, std::size_t fill_index = 0U,
                    bool consumes_reservation = false) {
    if (!session_enabled) {
      return Ok();
    }
    if ((consumes_reservation &&
         (reserved_transitions == 0U || transitions.size() >= max_transitions)) ||
        (!consumes_reservation && (reserved_transitions > max_transitions ||
                                   transitions.size() >= max_transitions - reserved_transitions))) {
      return Err(ErrorCode::OutOfRange, "option execution transition limit exceeded");
    }
    const OptionOrderAudit &audit = orders[order_index_value];
    transitions.push_back(OptionOrderTransition{
        static_cast<std::uint64_t>(transitions.size() + 1U), event_ts_ns, available_ts_ns,
        audit.request.order_id, cancel_id, kind, state_before, lifecycle_state(order_index_value),
        last_fill_contracts, audit.filled_contracts, audit.remaining_contracts, fill_index});
    if (consumes_reservation) {
      --reserved_transitions;
    }
    session_summary.transition_count = transitions.size();
    return Ok();
  }

  [[nodiscard]] Result<void> append_cancel_only_transition(const OptionCancelRequest &request,
                                                           OptionOrderTransitionKind kind) {
    if (!session_enabled) {
      return Ok();
    }
    const bool consumes_reservation = kind != OptionOrderTransitionKind::CancelRequested;
    if ((consumes_reservation &&
         (reserved_transitions == 0U || transitions.size() >= max_transitions)) ||
        (!consumes_reservation && (reserved_transitions > max_transitions ||
                                   transitions.size() >= max_transitions - reserved_transitions))) {
      return Err(ErrorCode::OutOfRange, "option execution transition limit exceeded");
    }
    const std::int64_t transition_available_ts_ns =
        kind == OptionOrderTransitionKind::CancelRequested ? request.event_ts_ns
                                                           : request.available_ts_ns;
    transitions.push_back(
        OptionOrderTransition{static_cast<std::uint64_t>(transitions.size() + 1U),
                              request.event_ts_ns, transition_available_ts_ns, request.order_id,
                              request.cancel_id, kind, OptionOrderLifecycleState::NotApplicable,
                              OptionOrderLifecycleState::NotApplicable, 0, 0, 0, 0U});
    if (consumes_reservation) {
      --reserved_transitions;
    }
    session_summary.transition_count = transitions.size();
    return Ok();
  }

  [[nodiscard]] Result<void> move_scheduled_to_working(std::size_t order_index_value) {
    if (!session_enabled) {
      return Ok();
    }
    const std::size_t contract = contract_index(orders[order_index_value].request.contract_id);
    OptionContractExposureSnapshot &exposure = exposures[contract];
    const std::int64_t leaves = orders[order_index_value].remaining_contracts;
    ATX_TRY(exposure.scheduled_contracts,
            atx::core::checked_sub(exposure.scheduled_contracts, leaves));
    ATX_TRY(exposure.working_contracts, atx::core::checked_add(exposure.working_contracts, leaves));
    return Ok();
  }

  [[nodiscard]] std::size_t meld_order_roots(std::size_t left, std::size_t right,
                                             bool buy) noexcept {
    if (left == kNoIndex) {
      return right;
    }
    if (right == kNoIndex) {
      return left;
    }
    if (better_order(right, left, buy)) {
      std::swap(left, right);
    }
    order_runtime[right].heap_sibling = order_runtime[left].heap_child;
    order_runtime[left].heap_child = right;
    return left;
  }

  void append_live_order(BookState &book, std::size_t order_index_value) noexcept {
    OrderRuntime &runtime = order_runtime[order_index_value];
    runtime.live_previous = book.live_tail;
    runtime.live_next = kNoIndex;
    if (book.live_tail == kNoIndex) {
      book.live_head = order_index_value;
    } else {
      order_runtime[book.live_tail].live_next = order_index_value;
    }
    book.live_tail = order_index_value;
  }

  void remove_live_order(BookState &book, std::size_t order_index_value) noexcept {
    OrderRuntime &runtime = order_runtime[order_index_value];
    if (runtime.live_previous == kNoIndex) {
      book.live_head = runtime.live_next;
    } else {
      order_runtime[runtime.live_previous].live_next = runtime.live_next;
    }
    if (runtime.live_next == kNoIndex) {
      book.live_tail = runtime.live_previous;
    } else {
      order_runtime[runtime.live_next].live_previous = runtime.live_previous;
    }
    runtime.live_previous = kNoIndex;
    runtime.live_next = kNoIndex;
  }

  [[nodiscard]] Result<void> insert_order(std::size_t order_index_value) {
    const OptionOrderRequest &request = orders[order_index_value].request;
    const std::size_t contract = contract_index(request.contract_id);
    BookState &book = books[contract];
    const bool buy = is_buy(request.quantity_contracts);
    std::size_t &root = buy ? book.buy_root : book.sell_root;
    OrderRuntime &runtime = order_runtime[order_index_value];
    const OptionOrderLifecycleState before = lifecycle_state(order_index_value);
    runtime.heap_child = kNoIndex;
    runtime.heap_sibling = kNoIndex;
    root = meld_order_roots(root, order_index_value, buy);
    runtime.live = true;
    append_live_order(book, order_index_value);
    ATX_TRY_VOID(move_scheduled_to_working(order_index_value));
    sync_order_state(order_index_value);
    ATX_TRY_VOID(append_transition(request.decision_ts_ns, request.arrival_ts_ns, order_index_value,
                                   {}, OptionOrderTransitionKind::Submitted, before, 0, 0U, true));
    if (request.time_in_force == OptionTimeInForce::FirstFutureQuoteOrCancel) {
      if (book.ioc_tail == kNoIndex) {
        book.ioc_head = order_index_value;
      } else {
        order_runtime[book.ioc_tail].ioc_next = order_index_value;
      }
      book.ioc_tail = order_index_value;
    }
    ++working_orders;
    summary.peak_working_orders = (std::max)(summary.peak_working_orders, working_orders);
    return Ok();
  }

  void pop_order_root(BookState &book, bool buy) noexcept {
    std::size_t &root = buy ? book.buy_root : book.sell_root;
    if (root == kNoIndex) {
      return;
    }
    std::size_t child = order_runtime[root].heap_child;
    order_runtime[root].heap_child = kNoIndex;
    order_runtime[root].heap_sibling = kNoIndex;

    std::size_t paired_roots = kNoIndex;
    for (std::size_t bounded = 0U; child != kNoIndex && bounded < orders.size(); ++bounded) {
      const std::size_t left = child;
      const std::size_t right = order_runtime[left].heap_sibling;
      std::size_t next = kNoIndex;
      order_runtime[left].heap_sibling = kNoIndex;
      if (right != kNoIndex) {
        next = order_runtime[right].heap_sibling;
        order_runtime[right].heap_sibling = kNoIndex;
      }
      const std::size_t merged = meld_order_roots(left, right, buy);
      order_runtime[merged].heap_sibling = paired_roots;
      paired_roots = merged;
      child = next;
    }

    root = kNoIndex;
    for (std::size_t bounded = 0U; paired_roots != kNoIndex && bounded < orders.size(); ++bounded) {
      const std::size_t current = paired_roots;
      paired_roots = order_runtime[current].heap_sibling;
      order_runtime[current].heap_sibling = kNoIndex;
      root = meld_order_roots(root, current, buy);
    }
  }

  [[nodiscard]] Result<void> terminalize(std::size_t order_index_value,
                                         OptionOrderDisposition disposition) {
    OrderRuntime &runtime = order_runtime[order_index_value];
    if (!runtime.live || order_terminal(orders[order_index_value].disposition)) {
      return Ok();
    }
    if (session_enabled) {
      const std::size_t contract = contract_index(orders[order_index_value].request.contract_id);
      OptionContractExposureSnapshot &exposure = exposures[contract];
      const std::int64_t leaves = orders[order_index_value].remaining_contracts;
      ATX_TRY(exposure.working_contracts,
              atx::core::checked_sub(exposure.working_contracts, leaves));
      ATX_TRY(exposure.projected_contracts,
              atx::core::checked_sub(exposure.projected_contracts, leaves));
      if (runtime.cancel_pending) {
        ATX_TRY(exposure.pending_cancel_contracts,
                atx::core::checked_sub(exposure.pending_cancel_contracts, leaves));
      }
    }
    runtime.live = false;
    orders[order_index_value].disposition = disposition;
    if (summary.open_orders > 0U) {
      --summary.open_orders;
    }
    if (disposition == OptionOrderDisposition::Canceled) {
      ++summary.canceled_orders;
    } else if (disposition == OptionOrderDisposition::Expired) {
      ++summary.expired_orders;
    }
    BookState &book = books[contract_index(orders[order_index_value].request.contract_id)];
    remove_live_order(book, order_index_value);
    if (working_orders > 0U) {
      --working_orders;
    }
    sync_order_state(order_index_value);
    return Ok();
  }

  [[nodiscard]] Result<void> update_side(SideState &side, Decimal price, std::int64_t observed_size,
                                         std::uint16_t participant_id, bool updated,
                                         std::size_t quote_index_value) {
    if (!updated) {
      if (side.valid && (side.price != price || side.participant_id != participant_id ||
                         side.observed_size_contracts != observed_size)) {
        return Err(ErrorCode::InvalidArgument,
                   "unchanged option quote side conflicts with prior state");
      }
      return Ok();
    }

    ATX_TRY(std::int64_t new_modeled_size,
            modeled_size(observed_size, config.displayed_size_fraction));
    if (!side.valid || side.price != price || side.participant_id != participant_id) {
      side.price = price;
      side.observed_size_contracts = observed_size;
      side.observed_modeled_size = new_modeled_size;
      side.remaining_size = new_modeled_size;
      side.participant_id = participant_id;
      side.quote_index = quote_index_value;
      side.valid = true;
      return Ok();
    }

    if (new_modeled_size > side.observed_modeled_size) {
      const std::int64_t increment = new_modeled_size - side.observed_modeled_size;
      ATX_TRY(side.remaining_size, atx::core::checked_add(side.remaining_size, increment));
    } else {
      side.remaining_size = (std::min)(side.remaining_size, new_modeled_size);
    }
    side.observed_size_contracts = observed_size;
    side.observed_modeled_size = new_modeled_size;
    side.quote_index = quote_index_value;
    return Ok();
  }

  [[nodiscard]] Result<OptionFeeBreakdown>
  calculate_fees(const OptionOrderRequest &request, std::int64_t count, Decimal premium,
                 bool first_fill, std::int64_t fill_ts_ns,
                 const OptionFeeSchedule *&used_schedule) const {
    used_schedule = fee_at(request.fee_schedule_key, fill_ts_ns);
    if (used_schedule == nullptr) {
      return Err(ErrorCode::NotFound, "no effective option fee schedule at fill timestamp");
    }
    OptionFeeBreakdown fees;
    ATX_TRY(fees.exchange, checked_scale_money(used_schedule->exchange_per_contract, count));
    ATX_TRY(fees.clearing, checked_scale_money(used_schedule->clearing_per_contract, count));
    ATX_TRY(fees.regulatory, checked_scale_money(used_schedule->regulatory_per_contract, count));
    ATX_TRY(fees.commission, checked_scale_money(used_schedule->commission_per_contract, count));
    if (first_fill) {
      ATX_TRY(fees.commission, fees.commission.checked_add(used_schedule->commission_per_order));
    }
    if (!is_buy(request.quantity_contracts)) {
      ATX_TRY(fees.sales_value, checked_money_product(premium, used_schedule->sell_premium_rate));
    }
    ATX_TRY_VOID(add_money(fees.total, fees.exchange));
    ATX_TRY_VOID(add_money(fees.total, fees.clearing));
    ATX_TRY_VOID(add_money(fees.total, fees.regulatory));
    ATX_TRY_VOID(add_money(fees.total, fees.commission));
    ATX_TRY_VOID(add_money(fees.total, fees.sales_value));
    return Ok(fees);
  }

  [[nodiscard]] Result<bool> try_fill(std::size_t order_index_value,
                                      std::size_t contract_index_value, SideState &side,
                                      std::int64_t fill_ts_ns) {
    OptionOrderAudit &audit = orders[order_index_value];
    OrderRuntime &runtime = order_runtime[order_index_value];
    const OptionOrderRequest &request = audit.request;
    if (!runtime.live || side.remaining_size <= 0 || side.quote_index == kNoIndex) {
      return Ok(false);
    }
    const OptionTopOfBookEvent &quote = quotes[side.quote_index];
    if (config.max_quote_age_ns > 0 &&
        fill_ts_ns - quote.quote_event_ts_ns > config.max_quote_age_ns) {
      ATX_TRY_VOID(add_size(summary.stale_match_attempts, 1U));
      return Ok(false);
    }
    const bool buy = is_buy(request.quantity_contracts);
    const OptionTickSchedule *tick =
        tick_at(contracts[contract_index_value].tick_schedule_key, fill_ts_ns);
    if (tick == nullptr) {
      return Err(ErrorCode::NotFound, "no effective option tick schedule at fill timestamp");
    }
    ATX_TRY(Decimal fill_price,
            modeled_fill_price(side.price, buy, config.adverse_price_bps, *tick));
    const bool marketable =
        buy ? fill_price <= request.limit_price : fill_price >= request.limit_price;
    if (!marketable) {
      return Ok(false);
    }

    ATX_TRY(std::size_t remaining_magnitude, magnitude_as_size(audit.remaining_contracts));
    const std::size_t available = static_cast<std::size_t>(side.remaining_size);
    const std::size_t fill_magnitude = (std::min)(remaining_magnitude, available);
    if (fill_magnitude == 0U ||
        fill_magnitude > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
      return Ok(false);
    }
    const std::int64_t count = static_cast<std::int64_t>(fill_magnitude);
    const std::int64_t signed_fill = buy ? count : -count;
    const OptionPositionSnapshot &position = positions[contract_index_value];
    ATX_TRY(Decimal premium, checked_premium(fill_price, position.multiplier, count));
    const OptionFeeSchedule *used_schedule = nullptr;
    ATX_TRY(OptionFeeBreakdown fee_breakdown,
            calculate_fees(request, count, premium, !runtime.first_fill_charged, fill_ts_ns,
                           used_schedule));
    ATX_TRY(Decimal slippage, absolute_difference(fill_price, side.price));

    Decimal cash_delta{};
    if (buy) {
      const Decimal negative_premium = Decimal::from_raw(-premium.raw());
      ATX_TRY(cash_delta, negative_premium.checked_sub(fee_breakdown.total));
    } else {
      ATX_TRY(cash_delta, premium.checked_sub(fee_breakdown.total));
    }
    ATX_TRY(Decimal new_cash, summary.final_cash.checked_add(cash_delta));
    ATX_TRY(std::int64_t new_position, atx::core::checked_add(position.contracts, signed_fill));
    ATX_TRY(std::int64_t new_filled, atx::core::checked_add(audit.filled_contracts, signed_fill));
    ATX_TRY(std::int64_t new_remaining,
            atx::core::checked_sub(audit.remaining_contracts, signed_fill));
    ATX_TRY(Decimal new_order_fees, audit.total_fees.checked_add(fee_breakdown.total));
    ATX_TRY(Decimal new_total_fees, summary.total_fees.checked_add(fee_breakdown.total));
    ATX_TRY(Decimal new_turnover, summary.gross_premium_turnover.checked_add(premium));
    ATX_TRY(std::size_t new_fill_count, checked_add_size(audit.fill_count, 1U));
    ATX_TRY(std::size_t new_summary_filled,
            checked_add_size(summary.filled_contracts, fill_magnitude));
    if (fills.size() >= limits.max_fills) {
      return Err(ErrorCode::OutOfRange, "option replay fill-record limit exceeded");
    }
    if (session_enabled && transitions.size() >= max_transitions) {
      return Err(ErrorCode::OutOfRange, "option execution transition limit exceeded");
    }

    std::int64_t new_working_contracts = 0;
    std::int64_t new_pending_cancel_contracts = 0;
    if (session_enabled) {
      const OptionContractExposureSnapshot &exposure = exposures[contract_index_value];
      ATX_TRY(new_working_contracts,
              atx::core::checked_sub(exposure.working_contracts, signed_fill));
      new_pending_cancel_contracts = exposure.pending_cancel_contracts;
      if (runtime.cancel_pending) {
        ATX_TRY(new_pending_cancel_contracts,
                atx::core::checked_sub(exposure.pending_cancel_contracts, signed_fill));
      }
    }

    const OptionOrderLifecycleState state_before = lifecycle_state(order_index_value);
    const std::int64_t displayed_before = side.remaining_size;
    side.remaining_size -= count;
    positions[contract_index_value].contracts = new_position;
    summary.final_cash = new_cash;
    summary.total_fees = new_total_fees;
    summary.gross_premium_turnover = new_turnover;
    summary.filled_contracts = new_summary_filled;
    audit.filled_contracts = new_filled;
    audit.remaining_contracts = new_remaining;
    audit.fill_count = new_fill_count;
    audit.total_fees = new_order_fees;
    if (audit.first_fill_ts_ns == 0) {
      audit.first_fill_ts_ns = fill_ts_ns;
    }
    audit.last_fill_ts_ns = fill_ts_ns;
    runtime.first_fill_charged = true;
    if (session_enabled) {
      OptionContractExposureSnapshot &exposure = exposures[contract_index_value];
      exposure.position_contracts = new_position;
      exposure.working_contracts = new_working_contracts;
      exposure.pending_cancel_contracts = new_pending_cancel_contracts;
    }
    if (new_remaining == 0) {
      ATX_TRY_VOID(terminalize(order_index_value, OptionOrderDisposition::Filled));
    }

    fills.push_back(OptionFill{request.order_id,
                               request.strategy_id,
                               request.basket_id,
                               request.contract_id,
                               request.engine_id,
                               signed_fill,
                               position.multiplier,
                               side.price,
                               fill_price,
                               slippage,
                               premium,
                               fee_breakdown,
                               cash_delta,
                               request.decision_ts_ns,
                               request.arrival_ts_ns,
                               fill_ts_ns,
                               quote.quote_event_ts_ns,
                               quote.available_ts_ns,
                               quote.order_key,
                               displayed_before,
                               side.remaining_size,
                               side.participant_id,
                               quote.cross_stream_ordering_ambiguous,
                               quote.source_identity,
                               used_schedule->source_identity,
                               tick->source_identity});
    sync_order_state(order_index_value);
    ATX_TRY_VOID(append_transition(quote.quote_event_ts_ns, fill_ts_ns, order_index_value, {},
                                   new_remaining == 0 ? OptionOrderTransitionKind::Filled
                                                      : OptionOrderTransitionKind::PartiallyFilled,
                                   state_before, signed_fill, fills.size()));
    summary.cross_stream_ordering_ambiguous =
        summary.cross_stream_ordering_ambiguous || quote.cross_stream_ordering_ambiguous;
    return Ok(true);
  }

  [[nodiscard]] Result<void> match_side(std::size_t contract_index_value, bool buy,
                                        std::int64_t fill_ts_ns) {
    BookState &book = books[contract_index_value];
    std::size_t &root = buy ? book.buy_root : book.sell_root;
    SideState &side = buy ? book.ask : book.bid;

    for (std::size_t bounded = 0U; bounded <= orders.size() && root != kNoIndex; ++bounded) {
      const std::size_t current = root;
      if (!order_runtime[current].live) {
        pop_order_root(book, buy);
        continue;
      }
      ATX_TRY(bool filled, try_fill(current, contract_index_value, side, fill_ts_ns));
      if (!order_runtime[current].live) {
        pop_order_root(book, buy);
        continue;
      }
      if (!filled || side.remaining_size == 0) {
        break;
      }
    }
    return Ok();
  }

  [[nodiscard]] Result<void> resolve_ioc(std::size_t contract_index_value, std::int64_t event_ts_ns,
                                         std::int64_t available_ts_ns) {
    BookState &book = books[contract_index_value];
    std::size_t current = book.ioc_head;
    book.ioc_head = kNoIndex;
    book.ioc_tail = kNoIndex;
    for (std::size_t bounded = 0U; bounded < orders.size() && current != kNoIndex; ++bounded) {
      const std::size_t next = order_runtime[current].ioc_next;
      order_runtime[current].ioc_next = kNoIndex;
      if (order_runtime[current].live) {
        const OptionOrderLifecycleState before = lifecycle_state(current);
        ATX_TRY_VOID(terminalize(current, OptionOrderDisposition::Canceled));
        ATX_TRY_VOID(append_transition(event_ts_ns, available_ts_ns, current, {},
                                       OptionOrderTransitionKind::Canceled, before));
      }
      current = next;
    }
    return Ok();
  }

  [[nodiscard]] Result<void> process_quote(std::size_t quote_index_value) {
    const OptionTopOfBookEvent &quote = quotes[quote_index_value];
    const std::size_t contract = contract_index(quote.contract_id);
    BookState &book = books[contract];
    ++summary.quote_events;
    summary.cross_stream_ordering_ambiguous =
        summary.cross_stream_ordering_ambiguous || quote.cross_stream_ordering_ambiguous;

    const bool executable = quote_status_executable(quote.status, config.allow_locked_market);
    if (!executable) {
      ++summary.non_executable_quote_events;
      book.bid.valid = false;
      book.ask.valid = false;
      book.bid.remaining_size = 0;
      book.ask.remaining_size = 0;
      ATX_TRY_VOID(resolve_ioc(contract, quote.quote_event_ts_ns, quote.available_ts_ns));
      return Ok();
    }
    ++summary.firm_quote_events;
    ATX_TRY_VOID(update_side(book.bid, quote.bid, quote.bid_size_contracts,
                             quote.bid_participant_id, quote.bid_updated, quote_index_value));
    ATX_TRY_VOID(update_side(book.ask, quote.ask, quote.ask_size_contracts,
                             quote.ask_participant_id, quote.ask_updated, quote_index_value));
    if (quote.ask_updated) {
      ATX_TRY_VOID(match_side(contract, true, quote.available_ts_ns));
    }
    if (quote.bid_updated) {
      ATX_TRY_VOID(match_side(contract, false, quote.available_ts_ns));
    }
    ATX_TRY_VOID(resolve_ioc(contract, quote.quote_event_ts_ns, quote.available_ts_ns));
    return Ok();
  }

  [[nodiscard]] Result<void> expire_contract(std::size_t contract_index_value,
                                             std::int64_t expiry_ts_ns) {
    if (session_enabled && positions[contract_index_value].contracts != 0) {
      return Err(ErrorCode::NotImplemented,
                 "option session requires an authoritative settlement or assignment "
                 "event for a nonzero position at expiry");
    }
    BookState &book = books[contract_index_value];
    std::size_t current = book.live_head;
    for (std::size_t bounded = 0U; current != kNoIndex && bounded < orders.size(); ++bounded) {
      const std::size_t next = order_runtime[current].live_next;
      const OptionOrderLifecycleState before = lifecycle_state(current);
      ATX_TRY_VOID(terminalize(current, OptionOrderDisposition::Expired));
      ATX_TRY_VOID(append_transition(expiry_ts_ns, expiry_ts_ns, current, {},
                                     OptionOrderTransitionKind::Expired, before));
      current = next;
    }
    return Ok();
  }

  void process_event(const ReplayEvent &event, Result<void> &status) {
    if (!status) {
      return;
    }
    switch (event.kind) {
    case ReplayEvent::Kind::Cancel: {
      OptionCancelAudit &audit = cancellations[event.index];
      const std::size_t target = cancellation_targets[event.index];
      if (target == kNoIndex) {
        audit.disposition = OptionCancelDisposition::UnknownOrder;
        status = append_cancel_only_transition(audit.request,
                                               OptionOrderTransitionKind::CancelUnknownOrder);
      } else if (!order_runtime[target].live || order_terminal(orders[target].disposition)) {
        audit.disposition = OptionCancelDisposition::AlreadyTerminal;
        order_runtime[target].cancel_pending = false;
        sync_order_state(target);
        status = append_transition(audit.request.event_ts_ns, audit.request.available_ts_ns, target,
                                   audit.request.cancel_id,
                                   OptionOrderTransitionKind::CancelAlreadyTerminal,
                                   lifecycle_state(target), 0, 0U, true);
      } else {
        const OptionOrderLifecycleState before = lifecycle_state(target);
        status = terminalize(target, OptionOrderDisposition::Canceled);
        if (!status) {
          break;
        }
        order_runtime[target].cancel_pending = false;
        sync_order_state(target);
        status = append_transition(audit.request.event_ts_ns, audit.request.available_ts_ns, target,
                                   audit.request.cancel_id, OptionOrderTransitionKind::Canceled,
                                   before, 0, 0U, true);
        audit.disposition = OptionCancelDisposition::Applied;
      }
      break;
    }
    case ReplayEvent::Kind::OrderExpiry: {
      const OptionOrderLifecycleState before = lifecycle_state(event.index);
      status = terminalize(event.index, OptionOrderDisposition::Expired);
      if (status && lifecycle_state(event.index) == OptionOrderLifecycleState::Expired) {
        status = append_transition(event.available_ts_ns, event.available_ts_ns, event.index, {},
                                   OptionOrderTransitionKind::Expired, before);
      }
      break;
    }
    case ReplayEvent::Kind::ContractExpiry:
      status = expire_contract(event.index, event.available_ts_ns);
      break;
    case ReplayEvent::Kind::Quote:
      status = process_quote(event.index);
      break;
    case ReplayEvent::Kind::Submit:
      status = insert_order(event.index);
      break;
    }
  }

  [[nodiscard]] Result<void> finalize() {
    for (std::size_t i = 0; i < orders.size(); ++i) {
      if (order_runtime[i].live &&
          orders[i].request.time_in_force == OptionTimeInForce::FirstFutureQuoteOrCancel) {
        const OptionOrderLifecycleState before = lifecycle_state(i);
        ATX_TRY_VOID(terminalize(i, OptionOrderDisposition::Canceled));
        ATX_TRY_VOID(append_transition(config.replay_end_ts_ns, config.replay_end_ts_ns, i, {},
                                       OptionOrderTransitionKind::Canceled, before));
      }
    }
    return Ok();
  }

  [[nodiscard]] Result<void> initialize(const OptionReplayInputs &inputs,
                                        const OptionReplayConfig &candidate) {
    clear();
    ATX_TRY_VOID(validate_config(candidate));
    config = candidate;
    summary.scenario = candidate.scenario;
    summary.model_version = candidate.model_version;
    summary.ordering_version = kOptionExecutionReplayOrderingVersion;
    summary.market_data_identity = candidate.market_data_identity;
    summary.sequence_validation_identity = candidate.sequence_validation_identity;
    summary.calibration_identity = candidate.calibration_identity;
    summary.sequence_continuity_verified = candidate.sequence_continuity_verified;
    summary.allow_locked_market = candidate.allow_locked_market;
    summary.displayed_size_fraction = candidate.displayed_size_fraction;
    summary.adverse_price_bps = candidate.adverse_price_bps;
    summary.max_quote_age_ns = candidate.max_quote_age_ns;
    summary.replay_end_ts_ns = candidate.replay_end_ts_ns;
    summary.initial_cash = inputs.initial_cash;
    summary.final_cash = inputs.initial_cash;

    ATX_TRY_VOID(load_contracts(inputs.contracts));
    ATX_TRY_VOID(load_fee_schedules(inputs.fee_schedules));
    ATX_TRY_VOID(load_tick_schedules(inputs.tick_schedules));
    ATX_TRY_VOID(load_orders(inputs.orders));
    ATX_TRY_VOID(load_cancellations(inputs.cancellations));
    ATX_TRY_VOID(load_quotes(inputs.quotes));
    add_events();
    return Ok();
  }

  [[nodiscard]] Result<void> apply_session_commands(const OptionCommandBatch &commands,
                                                    std::int64_t frontier_ts_ns,
                                                    std::uint64_t &last_order_id,
                                                    std::uint64_t &last_cancel_id) {
    if (commands.orders.size() > limits.max_orders - orders.size() ||
        commands.cancellations.size() > limits.max_cancellations - cancellations.size() ||
        commands.orders.size() > command_orders.capacity() ||
        commands.cancellations.size() > command_cancellations.capacity()) {
      return Err(ErrorCode::OutOfRange, "option session command capacity exceeded");
    }
    ATX_TRY(std::size_t transition_additions,
            checked_add_size(commands.orders.size(), commands.cancellations.size()));
    ATX_TRY(std::size_t transition_commitment, checked_mul_size(transition_additions, 2U));
    if (reserved_transitions > max_transitions ||
        transitions.size() > max_transitions - reserved_transitions ||
        transition_commitment > max_transitions - reserved_transitions - transitions.size()) {
      return Err(ErrorCode::OutOfRange, "option execution transition limit exceeded");
    }

    command_orders.assign(commands.orders.begin(), commands.orders.end());
    command_cancellations.assign(commands.cancellations.begin(), commands.cancellations.end());
    std::sort(command_orders.begin(), command_orders.end(),
              [](const OptionOrderRequest &left, const OptionOrderRequest &right) noexcept {
                return std::tie(left.arrival_ts_ns, left.priority_sequence, left.order_id.value) <
                       std::tie(right.arrival_ts_ns, right.priority_sequence, right.order_id.value);
              });
    std::sort(
        command_cancellations.begin(), command_cancellations.end(),
        [](const OptionCancelRequest &left, const OptionCancelRequest &right) noexcept {
          return std::tie(left.available_ts_ns, left.priority_sequence, left.cancel_id.value) <
                 std::tie(right.available_ts_ns, right.priority_sequence, right.cancel_id.value);
        });

    command_touched_contracts.clear();
    ++command_epoch;
    if (command_epoch == 0U) {
      std::fill(command_contract_epochs.begin(), command_contract_epochs.end(), 0U);
      command_epoch = 1U;
    }
    const auto touch_contract = [this](std::size_t contract) {
      if (command_contract_epochs[contract] != command_epoch) {
        command_contract_epochs[contract] = command_epoch;
        command_exposure_deltas[contract] = 0;
        command_pending_cancel_values[contract] = 0;
        command_touched_contracts.push_back(contract);
      }
    };
    std::size_t requested_addition = 0U;
    std::size_t pending_event_addition = command_cancellations.size();
    std::uint64_t previous_order_id = last_order_id;
    for (const OptionOrderRequest &request : command_orders) {
      if (request.decision_ts_ns != frontier_ts_ns || request.order_id.value <= previous_order_id) {
        return Err(ErrorCode::InvalidArgument,
                   "option session orders must use the current frontier and monotone ids");
      }
      ATX_TRY(std::size_t requested, validate_order_request(request));
      ATX_TRY_VOID(add_size(requested_addition, requested));
      const std::size_t contract = contract_index(request.contract_id);
      touch_contract(contract);
      ATX_TRY(
          command_exposure_deltas[contract],
          atx::core::checked_add(command_exposure_deltas[contract], request.quantity_contracts));
      ATX_TRY(pending_event_addition,
              checked_add_size(pending_event_addition,
                               request.time_in_force == OptionTimeInForce::Day ? 2U : 1U));
      previous_order_id = request.order_id.value;
    }
    ATX_TRY(std::size_t new_requested_contracts,
            checked_add_size(summary.requested_contracts, requested_addition));
    ATX_TRY(std::size_t new_open_orders,
            checked_add_size(summary.open_orders, command_orders.size()));
    if (pending_event_addition > pending_events.capacity() - pending_events.size()) {
      return Err(ErrorCode::OutOfRange, "option session pending-event capacity exceeded");
    }

    command_cancel_targets.clear();
    std::uint64_t previous_cancel_id = last_cancel_id;
    for (const OptionCancelRequest &request : command_cancellations) {
      if (request.cancel_id.value == 0U || request.order_id.value == 0U ||
          request.cancel_id.value <= previous_cancel_id || request.event_ts_ns != frontier_ts_ns ||
          request.available_ts_ns <= frontier_ts_ns ||
          request.available_ts_ns > config.replay_end_ts_ns) {
        return Err(ErrorCode::InvalidArgument,
                   "option session cancellations must use the current frontier, future "
                   "availability, and monotone ids");
      }
      const auto new_order =
          std::lower_bound(command_orders.begin(), command_orders.end(), request.order_id.value,
                           [](const OptionOrderRequest &candidate, std::uint64_t id) noexcept {
                             return candidate.order_id.value < id;
                           });
      if (new_order != command_orders.end() &&
          new_order->order_id.value == request.order_id.value) {
        return Err(ErrorCode::InvalidArgument,
                   "option session cannot submit and cancel one order in the same batch");
      }
      const std::size_t target = order_index(request.order_id);
      if (target != kNoIndex) {
        if (request.available_ts_ns <= orders[target].request.arrival_ts_ns) {
          return Err(ErrorCode::InvalidArgument,
                     "option cancel must become effective after order arrival");
        }
        if (order_runtime[target].cancel_pending) {
          return Err(ErrorCode::AlreadyExists, "option order already has a pending cancellation");
        }
        if (!order_terminal(orders[target].disposition)) {
          const std::size_t contract = contract_index(orders[target].request.contract_id);
          touch_contract(contract);
          ATX_TRY(command_pending_cancel_values[contract],
                  atx::core::checked_add(command_pending_cancel_values[contract],
                                         orders[target].remaining_contracts));
        }
      }
      command_cancel_targets.push_back(request.order_id.value);
      previous_cancel_id = request.cancel_id.value;
    }
    std::sort(command_cancel_targets.begin(), command_cancel_targets.end());
    if (std::adjacent_find(command_cancel_targets.begin(), command_cancel_targets.end()) !=
        command_cancel_targets.end()) {
      return Err(ErrorCode::AlreadyExists,
                 "option command batch contains duplicate cancellation targets");
    }
    for (const std::size_t contract : command_touched_contracts) {
      const std::int64_t order_delta = command_exposure_deltas[contract];
      ATX_TRY(std::int64_t ignored_scheduled,
              atx::core::checked_add(exposures[contract].scheduled_contracts, order_delta));
      ATX_TRY(std::int64_t ignored_projected,
              atx::core::checked_add(exposures[contract].projected_contracts, order_delta));
      ATX_TRY(command_pending_cancel_values[contract],
              atx::core::checked_add(exposures[contract].pending_cancel_contracts,
                                     command_pending_cancel_values[contract]));
      static_cast<void>(ignored_scheduled);
      static_cast<void>(ignored_projected);
    }

    for (const OptionOrderRequest &request : command_orders) {
      const std::size_t index = orders.size();
      orders.push_back(OptionOrderAudit{request, 0, request.quantity_contracts, 0U, Decimal{}, 0, 0,
                                        OptionOrderDisposition::OpenAtEnd});
      order_runtime.push_back(OrderRuntime{});
      order_lookup.push_back(OrderLookup{request.order_id, index});
      order_states.push_back(OptionOrderStateSnapshot{request.order_id, request.contract_id,
                                                      request.engine_id, request.quantity_contracts,
                                                      OptionOrderLifecycleState::Scheduled, false});
      push_pending_event(ReplayEvent{request.arrival_ts_ns, ReplayEvent::Kind::Submit, 0U, 0U, 0U,
                                     request.priority_sequence, 0U, request.order_id.value,
                                     request.contract_id, index});
      if (request.time_in_force == OptionTimeInForce::Day &&
          request.expire_ts_ns <= config.replay_end_ts_ns) {
        push_pending_event(ReplayEvent{request.expire_ts_ns, ReplayEvent::Kind::OrderExpiry, 0U, 0U,
                                       0U, request.priority_sequence, 0U, request.order_id.value,
                                       request.contract_id, index});
      }
      ATX_TRY_VOID(append_transition(frontier_ts_ns, frontier_ts_ns, index, {},
                                     OptionOrderTransitionKind::Scheduled,
                                     OptionOrderLifecycleState::Scheduled));
    }
    summary.requested_contracts = new_requested_contracts;
    summary.open_orders = new_open_orders;
    for (const std::size_t contract : command_touched_contracts) {
      const std::int64_t delta = command_exposure_deltas[contract];
      if (delta != 0) {
        ATX_TRY(exposures[contract].scheduled_contracts,
                atx::core::checked_add(exposures[contract].scheduled_contracts, delta));
        ATX_TRY(exposures[contract].projected_contracts,
                atx::core::checked_add(exposures[contract].projected_contracts, delta));
      }
      exposures[contract].pending_cancel_contracts = command_pending_cancel_values[contract];
    }

    for (const OptionCancelRequest &request : command_cancellations) {
      const std::size_t index = cancellations.size();
      cancellations.push_back(OptionCancelAudit{request, OptionCancelDisposition::AlreadyTerminal});
      const std::size_t target = order_index(request.order_id);
      cancellation_targets.push_back(target);
      push_pending_event(ReplayEvent{request.available_ts_ns, ReplayEvent::Kind::Cancel, 0U, 0U, 0U,
                                     request.priority_sequence, 0U, request.cancel_id.value,
                                     request.order_id.value, index});
      if (target == kNoIndex) {
        ATX_TRY_VOID(
            append_cancel_only_transition(request, OptionOrderTransitionKind::CancelRequested));
        continue;
      }
      const OptionOrderLifecycleState before = lifecycle_state(target);
      if (!order_terminal(orders[target].disposition)) {
        order_runtime[target].cancel_pending = true;
        sync_order_state(target);
      }
      ATX_TRY_VOID(append_transition(request.event_ts_ns, request.event_ts_ns, target,
                                     request.cancel_id, OptionOrderTransitionKind::CancelRequested,
                                     before));
    }

    last_order_id = previous_order_id;
    last_cancel_id = previous_cancel_id;
    reserved_transitions += transition_additions;
    std::uint64_t trace = session_summary.command_trace_hash;
    trace = fold_i64(trace, frontier_ts_ns);
    trace = fold_u64(trace, static_cast<std::uint64_t>(command_orders.size()));
    trace = fold_u64(trace, static_cast<std::uint64_t>(command_cancellations.size()));
    for (const OptionOrderRequest &request : command_orders) {
      trace = fold_order_request(trace, request);
    }
    for (const OptionCancelRequest &request : command_cancellations) {
      trace = fold_cancel_request(trace, request);
    }
    session_summary.command_trace_hash = trace;
    ++session_summary.command_batch_count;
    return Ok();
  }

  [[nodiscard]] OptionReplayView view() const noexcept {
    return OptionReplayView{std::span<const OptionFill>{fills},
                            std::span<const OptionOrderAudit>{orders},
                            std::span<const OptionCancelAudit>{cancellations},
                            std::span<const OptionPositionSnapshot>{positions},
                            std::span<const OptionFeeSchedule>{fee_schedules},
                            std::span<const OptionTickSchedule>{tick_schedules},
                            summary};
  }
};

struct OptionExecutionReplay::Impl final : ReplayCore {
  using ReplayCore::ReplayCore;
};

struct OptionExecutionSession::Impl final : ReplayCore {
  explicit Impl(OptionExecutionSessionLimits configured_limits)
      : ReplayCore{configured_limits.replay}, session_limits{configured_limits} {
    session_enabled = true;
    max_frontiers = configured_limits.max_frontiers;
    max_transitions = configured_limits.max_transitions;
  }

  OptionExecutionSessionLimits session_limits{};
  OptionExecutionSessionState phase{OptionExecutionSessionState::Empty};
  std::uint64_t last_order_id{0};
  std::uint64_t last_cancel_id{0};
};

Result<std::size_t> option_replay_required_workspace_bytes(const OptionReplayLimits &limits) {
  if (limits.max_contracts == 0U || limits.max_quote_events == 0U || limits.max_orders == 0U ||
      limits.max_cancellations == 0U || limits.max_fee_rows == 0U || limits.max_fills == 0U ||
      limits.max_tick_rows == 0U || limits.max_workspace_bytes == 0U) {
    return Err(ErrorCode::InvalidArgument, "option replay workspace limits must be positive");
  }
  ATX_TRY(std::size_t twice_orders, checked_mul_size(limits.max_orders, 2U));
  ATX_TRY(std::size_t event_capacity, checked_add_size(limits.max_quote_events, twice_orders));
  ATX_TRY(event_capacity, checked_add_size(event_capacity, limits.max_cancellations));
  ATX_TRY(event_capacity, checked_add_size(event_capacity, limits.max_contracts));
  return workspace_bytes(limits, event_capacity);
}

[[nodiscard]] Result<std::size_t>
option_execution_session_required_workspace_bytes(const OptionExecutionSessionLimits &limits) {
  if (limits.max_frontiers == 0U || limits.max_transitions == 0U ||
      limits.max_workspace_bytes == 0U) {
    return Err(ErrorCode::InvalidArgument, "option session workspace limits must be positive");
  }
  ATX_TRY(std::size_t bytes, option_replay_required_workspace_bytes(limits.replay));
  ATX_TRY(std::size_t twice_orders, checked_mul_size(limits.replay.max_orders, 2U));
  const auto add_array = [&bytes](std::size_t count, std::size_t element_size) -> Result<void> {
    ATX_TRY(std::size_t block, checked_mul_size(count, element_size));
    ATX_TRY(bytes, checked_add_size(bytes, block));
    return Ok();
  };
  ATX_TRY(std::size_t pending_capacity,
          checked_add_size(twice_orders, limits.replay.max_cancellations));
  ATX_TRY_VOID(add_array(pending_capacity, sizeof(ReplayEvent)));
  ATX_TRY_VOID(add_array(limits.replay.max_orders, sizeof(OptionOrderRequest)));
  ATX_TRY_VOID(add_array(limits.replay.max_cancellations, sizeof(OptionCancelRequest)));
  ATX_TRY_VOID(add_array(limits.replay.max_cancellations, sizeof(std::uint64_t)));
  ATX_TRY_VOID(add_array(limits.replay.max_contracts, sizeof(std::int64_t)));
  ATX_TRY_VOID(add_array(limits.replay.max_contracts, sizeof(std::int64_t)));
  ATX_TRY_VOID(add_array(limits.replay.max_contracts, sizeof(std::uint64_t)));
  ATX_TRY_VOID(add_array(limits.replay.max_contracts, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(limits.replay.max_orders, sizeof(OptionOrderStateSnapshot)));
  ATX_TRY_VOID(add_array(limits.replay.max_contracts, sizeof(OptionContractExposureSnapshot)));
  ATX_TRY_VOID(add_array(limits.max_transitions, sizeof(OptionOrderTransition)));
  return Ok(bytes);
}

Result<OptionExecutionReplay> OptionExecutionReplay::create(OptionReplayLimits limits) {
  ATX_TRY(std::size_t required_bytes, option_replay_required_workspace_bytes(limits));
  if (required_bytes > limits.max_workspace_bytes) {
    return Err(ErrorCode::OutOfRange, "option replay workspace byte limit exceeded");
  }

  try {
    auto impl = std::make_unique<Impl>(limits);
    impl->contracts.reserve(limits.max_contracts);
    impl->engine_ids.reserve(limits.max_contracts);
    impl->positions.reserve(limits.max_contracts);
    impl->books.reserve(limits.max_contracts);
    impl->quotes.reserve(limits.max_quote_events);
    impl->orders.reserve(limits.max_orders);
    impl->order_runtime.reserve(limits.max_orders);
    impl->order_lookup.reserve(limits.max_orders);
    impl->cancellations.reserve(limits.max_cancellations);
    impl->cancellation_targets.reserve(limits.max_cancellations);
    impl->fee_schedules.reserve(limits.max_fee_rows);
    impl->tick_schedules.reserve(limits.max_tick_rows);
    ATX_TRY(std::size_t twice_orders, checked_mul_size(limits.max_orders, 2U));
    ATX_TRY(std::size_t event_capacity, checked_add_size(limits.max_quote_events, twice_orders));
    ATX_TRY(event_capacity, checked_add_size(event_capacity, limits.max_cancellations));
    ATX_TRY(event_capacity, checked_add_size(event_capacity, limits.max_contracts));
    impl->events.reserve(event_capacity);
    impl->fills.reserve(limits.max_fills);
    return Ok(OptionExecutionReplay{std::move(impl)});
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "option replay workspace allocation failed");
  } catch (const std::length_error &) {
    return Err(ErrorCode::OutOfRange, "option replay workspace capacity is invalid");
  }
}

OptionExecutionReplay::OptionExecutionReplay(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

OptionExecutionReplay::~OptionExecutionReplay() = default;
OptionExecutionReplay::OptionExecutionReplay(OptionExecutionReplay &&) noexcept = default;
OptionExecutionReplay &
OptionExecutionReplay::operator=(OptionExecutionReplay &&) noexcept = default;

Result<OptionReplayView> OptionExecutionReplay::run(const OptionReplayInputs &inputs,
                                                    const OptionReplayConfig &config) {
  if (impl_ == nullptr) {
    return Err(ErrorCode::InvalidArgument, "moved-from option replay workspace cannot run");
  }
  Impl &state = *impl_;
  ATX_TRY_VOID(state.initialize(inputs, config));

  Result<void> replay_status = Ok();
  for (const ReplayEvent &event : state.events) {
    state.process_event(event, replay_status);
    if (!replay_status) {
      return tl::unexpected<atx::core::Error>(replay_status.error());
    }
  }
  ATX_TRY_VOID(state.finalize());
  return Ok(state.view());
}

Result<OptionExecutionSession> OptionExecutionSession::create(OptionExecutionSessionLimits limits) {
  ATX_TRY(std::size_t required_bytes, option_execution_session_required_workspace_bytes(limits));
  ATX_TRY(std::size_t replay_bytes, option_replay_required_workspace_bytes(limits.replay));
  if (required_bytes > limits.max_workspace_bytes ||
      replay_bytes > limits.replay.max_workspace_bytes) {
    return Err(ErrorCode::OutOfRange, "option session workspace byte limit exceeded");
  }

  try {
    auto impl = std::make_unique<Impl>(limits);
    const OptionReplayLimits &replay_limits = limits.replay;
    impl->contracts.reserve(replay_limits.max_contracts);
    impl->engine_ids.reserve(replay_limits.max_contracts);
    impl->positions.reserve(replay_limits.max_contracts);
    impl->books.reserve(replay_limits.max_contracts);
    impl->quotes.reserve(replay_limits.max_quote_events);
    impl->orders.reserve(replay_limits.max_orders);
    impl->order_runtime.reserve(replay_limits.max_orders);
    impl->order_lookup.reserve(replay_limits.max_orders);
    impl->cancellations.reserve(replay_limits.max_cancellations);
    impl->cancellation_targets.reserve(replay_limits.max_cancellations);
    impl->fee_schedules.reserve(replay_limits.max_fee_rows);
    impl->tick_schedules.reserve(replay_limits.max_tick_rows);
    ATX_TRY(std::size_t twice_orders, checked_mul_size(replay_limits.max_orders, 2U));
    ATX_TRY(std::size_t event_capacity,
            checked_add_size(replay_limits.max_quote_events, twice_orders));
    ATX_TRY(event_capacity, checked_add_size(event_capacity, replay_limits.max_cancellations));
    ATX_TRY(event_capacity, checked_add_size(event_capacity, replay_limits.max_contracts));
    ATX_TRY(std::size_t pending_capacity,
            checked_add_size(twice_orders, replay_limits.max_cancellations));
    impl->events.reserve(event_capacity);
    impl->pending_events.reserve(pending_capacity);
    impl->fills.reserve(replay_limits.max_fills);
    impl->command_orders.reserve(replay_limits.max_orders);
    impl->command_cancellations.reserve(replay_limits.max_cancellations);
    impl->command_cancel_targets.reserve(replay_limits.max_cancellations);
    impl->command_exposure_deltas.reserve(replay_limits.max_contracts);
    impl->command_pending_cancel_values.reserve(replay_limits.max_contracts);
    impl->command_contract_epochs.resize(replay_limits.max_contracts);
    impl->command_touched_contracts.reserve(replay_limits.max_contracts);
    impl->order_states.reserve(replay_limits.max_orders);
    impl->exposures.reserve(replay_limits.max_contracts);
    impl->transitions.reserve(limits.max_transitions);
    return Ok(OptionExecutionSession{std::move(impl)});
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "option session workspace allocation failed");
  } catch (const std::length_error &) {
    return Err(ErrorCode::OutOfRange, "option session workspace capacity is invalid");
  }
}

OptionExecutionSession::OptionExecutionSession(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

OptionExecutionSession::~OptionExecutionSession() = default;
OptionExecutionSession::OptionExecutionSession(OptionExecutionSession &&) noexcept = default;
OptionExecutionSession &
OptionExecutionSession::operator=(OptionExecutionSession &&) noexcept = default;

Result<void> OptionExecutionSession::start(const OptionReplayInputs &inputs,
                                           const OptionReplayConfig &config) {
  if (impl_ == nullptr) {
    return Err(ErrorCode::InvalidArgument, "moved-from option session cannot start");
  }
  Impl &state = *impl_;
  if (state.phase == OptionExecutionSessionState::ReadyToAdvance ||
      state.phase == OptionExecutionSessionState::AtFrontier) {
    return Err(ErrorCode::InvalidArgument, "active option session cannot be restarted");
  }
  if (!inputs.orders.empty() || !inputs.cancellations.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "option session start accepts market state only; apply commands at a frontier");
  }
  state.phase = OptionExecutionSessionState::Empty;
  const Result<void> initialized = state.initialize(inputs, config);
  if (!initialized) {
    return initialized;
  }
  state.exposures.reserve(state.limits.max_contracts);
  for (const OptionPositionSnapshot &position : state.positions) {
    state.exposures.push_back(OptionContractExposureSnapshot{
        position.contract_id, position.engine_id, position.contracts, 0, 0, 0, position.contracts});
  }
  state.command_exposure_deltas.resize(state.positions.size());
  state.command_pending_cancel_values.resize(state.positions.size());
  state.session_summary = {};
  state.session_summary.command_trace_hash =
      fold_u64(fold_u64(kFnvOffset, kOptionExecutionSessionModelVersion),
               kOptionExecutionSessionOrderingVersion);
  state.last_order_id = 0U;
  state.last_cancel_id = 0U;
  state.phase = OptionExecutionSessionState::ReadyToAdvance;
  return Ok();
}

Result<OptionExecutionFrontierView>
OptionExecutionSession::advance_to(std::int64_t frontier_ts_ns) {
  if (impl_ == nullptr) {
    return Err(ErrorCode::InvalidArgument, "moved-from option session cannot advance");
  }
  Impl &state = *impl_;
  if (state.phase != OptionExecutionSessionState::ReadyToAdvance) {
    return Err(ErrorCode::InvalidArgument,
               "option session must apply exactly one command batch before advancing");
  }
  if (frontier_ts_ns < 0 || frontier_ts_ns <= state.session_summary.frontier_ts_ns ||
      frontier_ts_ns > state.config.replay_end_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "option session frontier must strictly increase within replay horizon");
  }
  if (state.session_summary.frontier_count >= state.max_frontiers) {
    return Err(ErrorCode::OutOfRange, "option session frontier limit exceeded");
  }

  const std::size_t delta_fill_begin = state.new_fill_begin;
  const std::size_t delta_transition_begin = state.new_transition_begin;
  const Result<void> advanced = state.process_through(frontier_ts_ns);
  if (!advanced) {
    state.phase = OptionExecutionSessionState::Failed;
    return tl::unexpected<atx::core::Error>(advanced.error());
  }
  ++state.session_summary.frontier_count;
  state.session_summary.frontier_ts_ns = frontier_ts_ns;
  state.phase = OptionExecutionSessionState::AtFrontier;
  const std::span<const OptionFill> all_fills{state.fills};
  const std::span<const OptionOrderTransition> all_transitions{state.transitions};
  state.new_fill_begin = state.fills.size();
  state.new_transition_begin = state.transitions.size();
  return Ok(
      OptionExecutionFrontierView{frontier_ts_ns, all_fills.subspan(delta_fill_begin),
                                  all_transitions.subspan(delta_transition_begin), all_fills,
                                  std::span<const OptionOrderAudit>{state.orders},
                                  std::span<const OptionCancelAudit>{state.cancellations},
                                  std::span<const OptionPositionSnapshot>{state.positions},
                                  std::span<const OptionOrderStateSnapshot>{state.order_states},
                                  std::span<const OptionContractExposureSnapshot>{state.exposures},
                                  all_transitions, state.summary, state.session_summary});
}

Result<void> OptionExecutionSession::apply_commands(const OptionCommandBatch &commands) {
  if (impl_ == nullptr) {
    return Err(ErrorCode::InvalidArgument, "moved-from option session cannot apply commands");
  }
  Impl &state = *impl_;
  if (state.phase != OptionExecutionSessionState::AtFrontier) {
    return Err(ErrorCode::InvalidArgument, "option commands require one current observed frontier");
  }
  const Result<void> applied = state.apply_session_commands(
      commands, state.session_summary.frontier_ts_ns, state.last_order_id, state.last_cancel_id);
  if (!applied) {
    return applied;
  }
  state.phase = OptionExecutionSessionState::ReadyToAdvance;
  return Ok();
}

Result<OptionExecutionSessionResult> OptionExecutionSession::finish() {
  if (impl_ == nullptr) {
    return Err(ErrorCode::InvalidArgument, "moved-from option session cannot finish");
  }
  Impl &state = *impl_;
  if (state.phase != OptionExecutionSessionState::ReadyToAdvance) {
    return Err(ErrorCode::InvalidArgument,
               "option session must acknowledge its frontier before finish");
  }
  const Result<void> advanced = state.process_through(state.config.replay_end_ts_ns);
  if (!advanced) {
    state.phase = OptionExecutionSessionState::Failed;
    return tl::unexpected<atx::core::Error>(advanced.error());
  }
  const Result<void> finalized = state.finalize();
  if (!finalized) {
    state.phase = OptionExecutionSessionState::Failed;
    return tl::unexpected<atx::core::Error>(finalized.error());
  }
  state.phase = OptionExecutionSessionState::Finished;
  return Ok(OptionExecutionSessionResult{
      state.view(), std::span<const OptionOrderStateSnapshot>{state.order_states},
      std::span<const OptionContractExposureSnapshot>{state.exposures},
      std::span<const OptionOrderTransition>{state.transitions}, state.session_summary});
}

OptionExecutionSessionState OptionExecutionSession::state() const noexcept {
  return impl_ == nullptr ? OptionExecutionSessionState::Failed : impl_->phase;
}

Result<void> make_option_order_batch_into(const atx::options::research::OptionResearchPanel &panel,
                                          std::size_t date_index,
                                          const atx::options::research::OptionTargetBook &targets,
                                          const OptionOrderBatchSpec &spec,
                                          std::vector<OptionOrderRequest> &orders) {
  orders.clear();
  if (date_index >= panel.dataset().num_dates() ||
      targets.targets.size() != panel.instruments().size() ||
      targets.decision_ts_ns != panel.dataset().dates()[date_index] || spec.first_order_id == 0U ||
      spec.strategy_id == 0U || spec.basket_id == 0U || spec.first_priority_sequence == 0U ||
      spec.fee_schedule_key == 0U || spec.arrival_latency_ns <= 0 ||
      spec.limit_offset_bps.raw() < 0 || spec.limit_offset_bps >= Decimal::from_int(10'000) ||
      spec.limit_price_increment.raw() < 0 ||
      (spec.limit_offset_bps.raw() > 0 && spec.limit_price_increment.raw() == 0)) {
    return Err(ErrorCode::InvalidArgument, "option order-batch specification is invalid");
  }
  ATX_TRY_VOID(validate_tif(spec.time_in_force));
  const std::int64_t decision_ts_ns = targets.decision_ts_ns;

  std::size_t order_count = 0U;
  for (std::size_t index = 0; index < targets.targets.size(); ++index) {
    const auto &target = targets.targets[index];
    if (target.contract_id != panel.instruments()[index].contract_id ||
        target.engine_id != panel.instruments()[index].engine_id) {
      return Err(ErrorCode::InvalidArgument,
                 "option target book does not align with panel catalog");
    }
    if (target.order_contracts != 0) {
      ++order_count;
    }
  }
  if (order_count == 0U) {
    return Ok();
  }
  ATX_TRY(std::int64_t arrival_ts_ns,
          atx::core::checked_add(decision_ts_ns, spec.arrival_latency_ns));
  if ((spec.time_in_force == OptionTimeInForce::Day && spec.expire_ts_ns <= arrival_ts_ns) ||
      (spec.time_in_force != OptionTimeInForce::Day && spec.expire_ts_ns != 0)) {
    return Err(ErrorCode::InvalidArgument,
               "option order-batch expiry is inconsistent with time in force");
  }
  if (order_count > 0U) {
    const std::uint64_t offset = static_cast<std::uint64_t>(order_count - 1U);
    if (spec.first_order_id > (std::numeric_limits<std::uint64_t>::max)() - offset ||
        spec.first_priority_sequence > (std::numeric_limits<std::uint64_t>::max)() - offset) {
      return Err(ErrorCode::OutOfRange, "option order-batch identifiers exceed uint64 range");
    }
  }
  if (orders.capacity() < order_count) {
    return Err(ErrorCode::OutOfRange, "option order output capacity is too small");
  }

  const std::size_t row_offset = date_index * panel.instruments().size();
  const auto bid = panel.column(atx::options::research::OptionPanelField::Bid);
  const auto ask = panel.column(atx::options::research::OptionPanelField::Ask);
  std::uint64_t ordinal = 0U;
  for (std::size_t i = 0; i < targets.targets.size(); ++i) {
    const auto &target = targets.targets[i];
    if (target.order_contracts == 0) {
      continue;
    }
    const bool buy = target.order_contracts > 0;
    const double touch = buy ? ask[row_offset + i] : bid[row_offset + i];
    if (!std::isfinite(touch) || touch <= 0.0) {
      return Err(ErrorCode::Unavailable, "option target lacks executable decision-time touch");
    }
    ATX_TRY(Decimal touch_price, Decimal::from_double(touch));
    Decimal limit_price = touch_price;
    if (spec.limit_offset_bps.raw() > 0) {
      OptionTickSchedule batch_tick;
      batch_tick.tick_below_threshold = spec.limit_price_increment;
      batch_tick.tick_at_or_above_threshold = spec.limit_price_increment;
      ATX_TRY(limit_price, modeled_fill_price(touch_price, buy, spec.limit_offset_bps, batch_tick));
    }
    orders.push_back(OptionOrderRequest{
        OptionOrderId{spec.first_order_id + ordinal}, spec.strategy_id, spec.basket_id,
        target.contract_id, target.engine_id, target.order_contracts, limit_price, decision_ts_ns,
        arrival_ts_ns, spec.time_in_force == OptionTimeInForce::Day ? spec.expire_ts_ns : 0,
        spec.first_priority_sequence + ordinal, spec.fee_schedule_key, spec.time_in_force});
    ++ordinal;
  }
  return Ok();
}

Result<std::vector<OptionOrderRequest>> make_option_order_batch(
    const atx::options::research::OptionResearchPanel &panel, std::size_t date_index,
    const atx::options::research::OptionTargetBook &targets, const OptionOrderBatchSpec &spec) {
  std::vector<OptionOrderRequest> orders;
  orders.reserve(targets.targets.size());
  ATX_TRY_VOID(make_option_order_batch_into(panel, date_index, targets, spec, orders));
  return Ok(std::move(orders));
}

} // namespace atx::options::execution

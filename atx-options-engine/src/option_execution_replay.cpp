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
  std::size_t buy_heap_offset{0U};
  std::size_t buy_heap_size{0U};
  std::size_t buy_heap_capacity{0U};
  std::size_t sell_heap_offset{0U};
  std::size_t sell_heap_size{0U};
  std::size_t sell_heap_capacity{0U};
  std::size_t ioc_head{kNoIndex};
  std::size_t ioc_tail{kNoIndex};
};

struct OrderRuntime {
  std::size_t ioc_next{kNoIndex};
  bool live{false};
  bool first_fill_charged{false};
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
  ATX_TRY_VOID(add_array(limits.max_orders, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(limits.max_cancellations, sizeof(OptionCancelAudit)));
  ATX_TRY_VOID(add_array(limits.max_fee_rows, sizeof(OptionFeeSchedule)));
  ATX_TRY_VOID(add_array(limits.max_tick_rows, sizeof(OptionTickSchedule)));
  ATX_TRY_VOID(add_array(event_capacity, sizeof(ReplayEvent)));
  ATX_TRY_VOID(add_array(limits.max_fills, sizeof(OptionFill)));
  return Ok(bytes);
}

} // namespace

struct OptionExecutionReplay::Impl {
  explicit Impl(OptionReplayLimits configured_limits) : limits{configured_limits} {}

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
  std::vector<std::size_t> order_heap;
  std::vector<OptionCancelAudit> cancellations;
  std::vector<OptionFeeSchedule> fee_schedules;
  std::vector<OptionTickSchedule> tick_schedules;
  std::vector<ReplayEvent> events;
  std::vector<OptionFill> fills;
  OptionReplaySummary summary{};
  std::size_t working_orders{0U};

  void clear() noexcept {
    contracts.clear();
    engine_ids.clear();
    positions.clear();
    books.clear();
    quotes.clear();
    orders.clear();
    order_runtime.clear();
    order_lookup.clear();
    order_heap.clear();
    cancellations.clear();
    fee_schedules.clear();
    tick_schedules.clear();
    events.clear();
    fills.clear();
    summary = {};
    working_orders = 0U;
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
    summary.quote_events = quotes.size();
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

  [[nodiscard]] Result<void> load_orders(std::span<const OptionOrderRequest> input_orders) {
    if (input_orders.size() > limits.max_orders) {
      return Err(ErrorCode::OutOfRange, "option replay order limit exceeded");
    }
    for (const OptionOrderRequest &request : input_orders) {
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
      const OptionTickSchedule *tick =
          tick_at(definition->tick_schedule_key, request.arrival_ts_ns);
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
      ATX_TRY(std::size_t requested, magnitude_as_size(request.quantity_contracts));
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
    for (const OptionOrderAudit &audit : orders) {
      BookState &book = books[contract_index(audit.request.contract_id)];
      if (is_buy(audit.request.quantity_contracts)) {
        ++book.buy_heap_capacity;
      } else {
        ++book.sell_heap_capacity;
      }
    }
    std::size_t heap_offset = 0U;
    for (BookState &book : books) {
      book.buy_heap_offset = heap_offset;
      ATX_TRY(heap_offset, checked_add_size(heap_offset, book.buy_heap_capacity));
      book.sell_heap_offset = heap_offset;
      ATX_TRY(heap_offset, checked_add_size(heap_offset, book.sell_heap_capacity));
    }
    order_heap.resize(heap_offset, kNoIndex);
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

  [[nodiscard]] Result<void> insert_order(std::size_t order_index_value) {
    const OptionOrderRequest &request = orders[order_index_value].request;
    const std::size_t contract = contract_index(request.contract_id);
    BookState &book = books[contract];
    const bool buy = is_buy(request.quantity_contracts);
    const std::size_t offset = buy ? book.buy_heap_offset : book.sell_heap_offset;
    std::size_t &size = buy ? book.buy_heap_size : book.sell_heap_size;
    const std::size_t capacity = buy ? book.buy_heap_capacity : book.sell_heap_capacity;
    if (size >= capacity) {
      return Err(ErrorCode::Internal, "option order heap capacity invariant failed");
    }
    std::size_t position = size;
    ++size;
    while (position > 0U) {
      const std::size_t parent = (position - 1U) / 2U;
      if (!better_order(order_index_value, order_heap[offset + parent], buy)) {
        break;
      }
      order_heap[offset + position] = order_heap[offset + parent];
      position = parent;
    }
    order_heap[offset + position] = order_index_value;
    order_runtime[order_index_value].live = true;
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

  void pop_heap_root(BookState &book, bool buy) noexcept {
    const std::size_t offset = buy ? book.buy_heap_offset : book.sell_heap_offset;
    std::size_t &size = buy ? book.buy_heap_size : book.sell_heap_size;
    if (size == 0U) {
      return;
    }
    --size;
    if (size == 0U) {
      return;
    }
    const std::size_t replacement = order_heap[offset + size];
    std::size_t position = 0U;
    while (true) {
      const std::size_t left = position * 2U + 1U;
      if (left >= size) {
        break;
      }
      const std::size_t right = left + 1U;
      std::size_t better_child = left;
      if (right < size &&
          better_order(order_heap[offset + right], order_heap[offset + left], buy)) {
        better_child = right;
      }
      if (!better_order(order_heap[offset + better_child], replacement, buy)) {
        break;
      }
      order_heap[offset + position] = order_heap[offset + better_child];
      position = better_child;
    }
    order_heap[offset + position] = replacement;
  }

  void terminalize(std::size_t order_index_value, OptionOrderDisposition disposition) noexcept {
    OrderRuntime &runtime = order_runtime[order_index_value];
    if (!runtime.live || order_terminal(orders[order_index_value].disposition)) {
      return;
    }
    runtime.live = false;
    orders[order_index_value].disposition = disposition;
    if (working_orders > 0U) {
      --working_orders;
    }
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
    if (new_remaining == 0) {
      terminalize(order_index_value, OptionOrderDisposition::Filled);
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
    summary.cross_stream_ordering_ambiguous =
        summary.cross_stream_ordering_ambiguous || quote.cross_stream_ordering_ambiguous;
    return Ok(true);
  }

  [[nodiscard]] Result<void> match_side(std::size_t contract_index_value, bool buy,
                                        std::int64_t fill_ts_ns) {
    BookState &book = books[contract_index_value];
    const std::size_t offset = buy ? book.buy_heap_offset : book.sell_heap_offset;
    std::size_t &heap_size = buy ? book.buy_heap_size : book.sell_heap_size;
    SideState &side = buy ? book.ask : book.bid;

    for (std::size_t bounded = 0U; bounded <= orders.size() && heap_size > 0U; ++bounded) {
      const std::size_t current = order_heap[offset];
      if (!order_runtime[current].live) {
        pop_heap_root(book, buy);
        continue;
      }
      ATX_TRY(bool filled, try_fill(current, contract_index_value, side, fill_ts_ns));
      if (!order_runtime[current].live) {
        pop_heap_root(book, buy);
        continue;
      }
      if (!filled || side.remaining_size == 0) {
        break;
      }
    }
    return Ok();
  }

  void resolve_ioc(std::size_t contract_index_value) noexcept {
    BookState &book = books[contract_index_value];
    std::size_t current = book.ioc_head;
    book.ioc_head = kNoIndex;
    book.ioc_tail = kNoIndex;
    for (std::size_t bounded = 0U; bounded < orders.size() && current != kNoIndex; ++bounded) {
      const std::size_t next = order_runtime[current].ioc_next;
      order_runtime[current].ioc_next = kNoIndex;
      if (order_runtime[current].live) {
        terminalize(current, OptionOrderDisposition::Canceled);
      }
      current = next;
    }
  }

  [[nodiscard]] Result<void> process_quote(std::size_t quote_index_value) {
    const OptionTopOfBookEvent &quote = quotes[quote_index_value];
    const std::size_t contract = contract_index(quote.contract_id);
    BookState &book = books[contract];
    summary.cross_stream_ordering_ambiguous =
        summary.cross_stream_ordering_ambiguous || quote.cross_stream_ordering_ambiguous;

    const bool executable = quote_status_executable(quote.status, config.allow_locked_market);
    if (!executable) {
      ++summary.non_executable_quote_events;
      book.bid.valid = false;
      book.ask.valid = false;
      book.bid.remaining_size = 0;
      book.ask.remaining_size = 0;
      resolve_ioc(contract);
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
    resolve_ioc(contract);
    return Ok();
  }

  void expire_contract(std::size_t contract_index_value) noexcept {
    const BookState &book = books[contract_index_value];
    const auto expire_heap = [this](std::size_t offset, std::size_t size) noexcept {
      for (std::size_t i = 0U; i < size; ++i) {
        const std::size_t current = order_heap[offset + i];
        if (current != kNoIndex && order_runtime[current].live) {
          terminalize(current, OptionOrderDisposition::Expired);
        }
      }
    };
    expire_heap(book.buy_heap_offset, book.buy_heap_size);
    expire_heap(book.sell_heap_offset, book.sell_heap_size);
  }

  void process_event(const ReplayEvent &event, Result<void> &status) {
    if (!status) {
      return;
    }
    switch (event.kind) {
    case ReplayEvent::Kind::Cancel: {
      OptionCancelAudit &audit = cancellations[event.index];
      const std::size_t target = order_index(audit.request.order_id);
      if (target == kNoIndex) {
        audit.disposition = OptionCancelDisposition::UnknownOrder;
      } else if (!order_runtime[target].live || order_terminal(orders[target].disposition)) {
        audit.disposition = OptionCancelDisposition::AlreadyTerminal;
      } else {
        terminalize(target, OptionOrderDisposition::Canceled);
        audit.disposition = OptionCancelDisposition::Applied;
      }
      break;
    }
    case ReplayEvent::Kind::OrderExpiry:
      terminalize(event.index, OptionOrderDisposition::Expired);
      break;
    case ReplayEvent::Kind::ContractExpiry:
      expire_contract(event.index);
      break;
    case ReplayEvent::Kind::Quote:
      status = process_quote(event.index);
      break;
    case ReplayEvent::Kind::Submit:
      status = insert_order(event.index);
      break;
    }
  }

  void finalize() noexcept {
    for (std::size_t i = 0; i < orders.size(); ++i) {
      if (order_runtime[i].live &&
          orders[i].request.time_in_force == OptionTimeInForce::FirstFutureQuoteOrCancel) {
        terminalize(i, OptionOrderDisposition::Canceled);
      }
    }
    for (const OptionOrderAudit &audit : orders) {
      switch (audit.disposition) {
      case OptionOrderDisposition::Filled:
        break;
      case OptionOrderDisposition::Canceled:
        ++summary.canceled_orders;
        break;
      case OptionOrderDisposition::Expired:
        ++summary.expired_orders;
        break;
      case OptionOrderDisposition::OpenAtEnd:
        ++summary.open_orders;
        break;
      }
    }
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
    impl->order_heap.reserve(limits.max_orders);
    impl->cancellations.reserve(limits.max_cancellations);
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
  state.clear();
  ATX_TRY_VOID(state.validate_config(config));
  state.config = config;
  state.summary.scenario = config.scenario;
  state.summary.model_version = config.model_version;
  state.summary.ordering_version = kOptionExecutionReplayOrderingVersion;
  state.summary.market_data_identity = config.market_data_identity;
  state.summary.sequence_validation_identity = config.sequence_validation_identity;
  state.summary.calibration_identity = config.calibration_identity;
  state.summary.sequence_continuity_verified = config.sequence_continuity_verified;
  state.summary.allow_locked_market = config.allow_locked_market;
  state.summary.displayed_size_fraction = config.displayed_size_fraction;
  state.summary.adverse_price_bps = config.adverse_price_bps;
  state.summary.max_quote_age_ns = config.max_quote_age_ns;
  state.summary.replay_end_ts_ns = config.replay_end_ts_ns;
  state.summary.initial_cash = inputs.initial_cash;
  state.summary.final_cash = inputs.initial_cash;

  ATX_TRY_VOID(state.load_contracts(inputs.contracts));
  ATX_TRY_VOID(state.load_fee_schedules(inputs.fee_schedules));
  ATX_TRY_VOID(state.load_tick_schedules(inputs.tick_schedules));
  ATX_TRY_VOID(state.load_orders(inputs.orders));
  ATX_TRY_VOID(state.load_cancellations(inputs.cancellations));
  ATX_TRY_VOID(state.load_quotes(inputs.quotes));
  state.add_events();

  Result<void> replay_status = Ok();
  for (const ReplayEvent &event : state.events) {
    state.process_event(event, replay_status);
    if (!replay_status) {
      return tl::unexpected<atx::core::Error>(replay_status.error());
    }
  }
  state.finalize();
  return Ok(state.view());
}

Result<std::vector<OptionOrderRequest>> make_option_order_batch(
    const atx::options::research::OptionResearchPanel &panel, std::size_t date_index,
    const atx::options::research::OptionTargetBook &targets, const OptionOrderBatchSpec &spec) {
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
  ATX_TRY(std::int64_t arrival_ts_ns,
          atx::core::checked_add(decision_ts_ns, spec.arrival_latency_ns));
  if ((spec.time_in_force == OptionTimeInForce::Day && spec.expire_ts_ns <= arrival_ts_ns) ||
      (spec.time_in_force != OptionTimeInForce::Day && spec.expire_ts_ns != 0)) {
    return Err(ErrorCode::InvalidArgument,
               "option order-batch expiry is inconsistent with time in force");
  }

  std::size_t order_count = 0U;
  for (const auto &target : targets.targets) {
    if (target.order_contracts != 0) {
      ++order_count;
    }
  }
  if (order_count > 0U) {
    const std::uint64_t offset = static_cast<std::uint64_t>(order_count - 1U);
    if (spec.first_order_id > (std::numeric_limits<std::uint64_t>::max)() - offset ||
        spec.first_priority_sequence > (std::numeric_limits<std::uint64_t>::max)() - offset) {
      return Err(ErrorCode::OutOfRange, "option order-batch identifiers exceed uint64 range");
    }
  }

  std::vector<OptionOrderRequest> orders;
  orders.reserve(order_count);
  const std::size_t row_offset = date_index * panel.instruments().size();
  const auto bid = panel.column(atx::options::research::OptionPanelField::Bid);
  const auto ask = panel.column(atx::options::research::OptionPanelField::Ask);
  std::uint64_t ordinal = 0U;
  for (std::size_t i = 0; i < targets.targets.size(); ++i) {
    const auto &target = targets.targets[i];
    if (target.contract_id != panel.instruments()[i].contract_id ||
        target.engine_id != panel.instruments()[i].engine_id) {
      return Err(ErrorCode::InvalidArgument,
                 "option target book does not align with panel catalog");
    }
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
  return Ok(std::move(orders));
}

} // namespace atx::options::execution

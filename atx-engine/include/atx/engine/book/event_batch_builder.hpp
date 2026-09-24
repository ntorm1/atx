#pragma once

// atx::engine::book -- mandatory-event inputs for the scheduled replay.
//
// Two builders, both pure and header-only:
//
// 1. Delisting events for ReplayConfig::delistings.
//    delisting_events_from_records() turns external delisting records (a
//    CRSP-style delist date plus delisting return) into DelistingEvents anchored
//    on the panel's own final valid close. A record whose formal delist date is
//    LATER than the last print (the name stopped trading, then was formally
//    delisted) anchors on the last print, so the first held valuation with no
//    close liquidates instead of aborting the replay.
//    detect_terminal_delistings() derives the same events from the close panel
//    alone for names that stop printing before the final observation.
//    A missing delisting return stays NaN unless the caller supplies an explicit
//    fill (Shumway 1997 documents -30% for performance delistings on NYSE/AMEX);
//    DelistingPolicy::CrspDelistReturn then rejects any NaN left behind.
//
// 2. A ReplayMandatoryEventPolicy that serves pre-scheduled transitions and
//    payments by observation, for replay_scheduled_intents_with_events. It owns
//    its batches (shared, immutable), so the returned std::function is cheap to
//    copy and safe to call from several replays at once.

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/replay.hpp"

namespace atx::engine::book {

struct DelistingRecord {
  atx::usize instrument{};
  atx::usize delist_period{}; // Observation index of the formal delisting date.
  atx::f64 delist_return{std::numeric_limits<atx::f64>::quiet_NaN()};
};

namespace detail {
[[nodiscard]] inline bool valid_close(atx::f64 price) noexcept {
  return std::isfinite(price) && price > 0.0;
}
[[nodiscard]] inline atx::f64 filled_return(atx::f64 value, atx::f64 fill) noexcept {
  return std::isnan(value) ? fill : value;
}
} // namespace detail

// One event per record, in record order. Rejects: shape mismatch, an
// instrument/period out of range, a duplicate instrument, a name with no valid
// close at or before its delist date, or a valid close after it.
[[nodiscard]] inline atx::core::Result<std::vector<DelistingEvent>>
delisting_events_from_records(std::span<const atx::f64> close, atx::usize dates,
                              atx::usize instruments, std::span<const DelistingRecord> records,
                              atx::f64 missing_return_fill =
                                  std::numeric_limits<atx::f64>::quiet_NaN()) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  if (dates == 0 || instruments == 0 || close.size() != dates * instruments) {
    return Err(ErrorCode::InvalidArgument, "delisting: close shape mismatch");
  }
  std::vector<atx::u8> seen(instruments, atx::u8{0});
  std::vector<DelistingEvent> events;
  events.reserve(records.size());
  for (const auto &record : records) {
    const auto i = record.instrument;
    if (i >= instruments || record.delist_period >= dates || seen[i] != 0) {
      return Err(ErrorCode::InvalidArgument,
                 "delisting: invalid or duplicate record for instrument=" + std::to_string(i));
    }
    seen[i] = 1;
    for (atx::usize p = record.delist_period + 1; p < dates; ++p) {
      if (detail::valid_close(close[p * instruments + i])) {
        return Err(ErrorCode::InvalidArgument,
                   "delisting: close printed after delisting for instrument=" +
                       std::to_string(i));
      }
    }
    // Bounded scan backwards to the final print at or before the delist date.
    atx::usize last = dates;
    for (atx::usize k = 0; k <= record.delist_period; ++k) {
      const auto p = record.delist_period - k;
      if (detail::valid_close(close[p * instruments + i])) {
        last = p;
        break;
      }
    }
    if (last == dates) {
      return Err(ErrorCode::InvalidArgument,
                 "delisting: no valid close before delisting for instrument=" +
                     std::to_string(i));
    }
    events.push_back(DelistingEvent{
        i, last, detail::filled_return(record.delist_return, missing_return_fill)});
  }
  return atx::core::Ok(std::move(events));
}

// Names whose final valid close precedes the final observation, ascending by
// instrument. A name that never prints, or prints at the final observation,
// yields no event. Interior gaps are not delistings and yield nothing.
[[nodiscard]] inline atx::core::Result<std::vector<DelistingEvent>>
detect_terminal_delistings(std::span<const atx::f64> close, atx::usize dates,
                           atx::usize instruments,
                           atx::f64 missing_return_fill =
                               std::numeric_limits<atx::f64>::quiet_NaN()) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  if (dates == 0 || instruments == 0 || close.size() != dates * instruments) {
    return Err(ErrorCode::InvalidArgument, "delisting: close shape mismatch");
  }
  constexpr auto kNever = std::numeric_limits<atx::usize>::max();
  std::vector<atx::usize> last(instruments, kNever);
  for (atx::usize p = 0; p < dates; ++p) {
    for (atx::usize i = 0; i < instruments; ++i) {
      if (detail::valid_close(close[p * instruments + i])) last[i] = p;
    }
  }
  std::vector<DelistingEvent> events;
  for (atx::usize i = 0; i < instruments; ++i) {
    if (last[i] == kNever || last[i] + 1 == dates) continue;
    events.push_back(DelistingEvent{i, last[i], missing_return_fill});
  }
  return atx::core::Ok(std::move(events));
}

struct ScheduledTransition {
  atx::usize period{};
  ReplayTransitionRequest request;
};

struct ScheduledPayment {
  atx::usize period{};
  data::CashClaimPayment payment;
};

// Groups scheduled events into one ReplayEventBatch per observation, preserving
// input order inside a batch. Every period must lie in [1, dates - 1), the only
// observations the replay ever queries; a batch beyond its fixed capacity
// rejects. Periods without events receive an empty batch.
[[nodiscard]] inline atx::core::Result<ReplayMandatoryEventPolicy>
make_event_batch_policy(atx::usize dates, std::span<const ScheduledTransition> transitions,
                        std::span<const ScheduledPayment> payments) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  using Keyed = std::pair<atx::usize, ReplayEventBatch>;
  auto batches = std::make_shared<std::vector<Keyed>>();
  auto batch_for = [&](atx::usize period) -> atx::core::Result<ReplayEventBatch *> {
    if (period == 0 || period + 1 >= dates) {
      return Err(ErrorCode::InvalidArgument,
                 "event batch: period outside [1, dates - 1): " + std::to_string(period));
    }
    auto it = std::lower_bound(batches->begin(), batches->end(), period,
                               [](const Keyed &k, atx::usize p) { return k.first < p; });
    if (it == batches->end() || it->first != period) {
      it = batches->insert(it, Keyed{period, ReplayEventBatch{}});
    }
    return atx::core::Ok(&it->second);
  };
  for (const auto &scheduled : transitions) {
    ATX_TRY(auto *batch, batch_for(scheduled.period));
    if (batch->transition_count == kMaxEventsPerObservation) {
      return Err(ErrorCode::OutOfRange, "event batch: too many transitions at period=" +
                                            std::to_string(scheduled.period));
    }
    batch->transitions[batch->transition_count++] = scheduled.request;
  }
  for (const auto &scheduled : payments) {
    ATX_TRY(auto *batch, batch_for(scheduled.period));
    if (batch->payment_count == kMaxPaymentsPerObservation) {
      return Err(ErrorCode::OutOfRange, "event batch: too many payments at period=" +
                                            std::to_string(scheduled.period));
    }
    batch->payments[batch->payment_count++] = scheduled.payment;
  }
  std::shared_ptr<const std::vector<Keyed>> frozen = std::move(batches);
  ReplayMandatoryEventPolicy policy =
      [frozen](const ReplayEventContext &context) -> atx::core::Result<ReplayEventBatch> {
    const auto it = std::lower_bound(frozen->begin(), frozen->end(), context.period,
                                     [](const Keyed &k, atx::usize p) { return k.first < p; });
    if (it == frozen->end() || it->first != context.period) {
      return atx::core::Ok(ReplayEventBatch{});
    }
    return atx::core::Ok(it->second);
  };
  return atx::core::Ok(std::move(policy));
}

} // namespace atx::engine::book

#pragma once
#include "atx/engine/factory/execution_cash_claim_streams.hpp"
#include "atx/engine/factory/execution_stock_transition.hpp"
namespace atx::engine::factory {
struct ExecutionStockTransitionRecognition {
  atx::usize event_index{}, predecessor_index{}, successor_index{}, period{};
  atx::u64 predecessor_id{}, successor_id{};
  atx::i64 recognition_mark_ns{};
  atx::f64 removed_equity_dollars{}, predecessor_share_equivalents{};
  atx::f64 delivered_successor_share_equivalents{}, delivered_successor_dollars{};
  atx::f64 fixed_cash_claim_dollars{}, recognition_pnl_dollars{};
  atx::f64 continued_cash_annual_borrow_rate{};
};
struct ExecutionStockTransitionEventUse {
  atx::usize event_index{}, predecessor_index{}, successor_index{};
  ExecutionCashClaimUse use{ExecutionCashClaimUse::InRole};
};
struct ExecutionStockTransitionStreams {
  // Cash recognitions/uses stay indexed ONLY to cash_events. Balances and carry
  // include optional stock-event fixed cash. cash.streams owns the single output.
  ExecutionCashClaimStreams cash;
  std::vector<ExecutionStockTransitionRecognition> stock_recognitions{};
  std::vector<ExecutionStockTransitionEventUse> stock_event_uses{};
  // D-sized, NaN outside mature intervals; empty for no stock events. Flows at
  // recognition, not persistent positions. Bridge includes fixed cash component;
  // cash.recognition_pnl_dollars remains cash-event-only (no double attribution).
  std::vector<atx::f64> signed_delivered_dollars{}, recognition_pnl_dollars{};
  std::vector<atx::f64> fixed_cash_component_dollars{};
  bool physical_delivery_and_fraction_cash_unresolved{true};
};
} // namespace atx::engine::factory

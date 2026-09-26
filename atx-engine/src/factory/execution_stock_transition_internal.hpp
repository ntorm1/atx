#pragma once
#include "atx/engine/factory/execution_stock_transition_streams.hpp"
namespace atx::engine::factory::execution_stock_transition_detail {
[[nodiscard]] atx::core::Status validate(const ExecutionStockTransitionEvent& event);
[[nodiscard]] atx::core::Result<ExecutionStockTransitionRecognition> recognize(
    const ExecutionStockTransitionEvent& event,atx::usize event_index,
    atx::usize predecessor_index,atx::usize successor_index,atx::usize period,
    atx::f64 held_dollars);
} // namespace atx::engine::factory::execution_stock_transition_detail

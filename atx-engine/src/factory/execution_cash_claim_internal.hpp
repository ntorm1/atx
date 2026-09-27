#pragma once
#include "atx/engine/factory/execution_cash_claim_streams.hpp"
namespace atx::engine::factory::execution_cash_claim_detail {
[[nodiscard]] atx::core::Status validate(const ExecutionCashClaimEvent& event);
[[nodiscard]] atx::core::Result<ExecutionCashClaimRecognition> recognize(
    const ExecutionCashClaimEvent& event,atx::usize event_index,atx::usize instrument_index,
    atx::usize period,atx::f64 held_dollars);
} // namespace atx::engine::factory::execution_cash_claim_detail

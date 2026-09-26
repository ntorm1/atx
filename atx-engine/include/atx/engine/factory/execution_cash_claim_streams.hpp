#pragma once
#include <vector>
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/factory/execution_cash_claim.hpp"

namespace atx::engine::factory {
enum class ExecutionCashClaimUse : atx::u8 { InRole=1, PreRoleRetired=2, OutsideAxis=3, AfterRole=4 };
struct ExecutionCashClaimEventUse {
  atx::usize event_index{};
  atx::usize instrument_index{}; // instruments() sentinel for OutsideAxis
  ExecutionCashClaimUse use{ExecutionCashClaimUse::InRole};
};
struct ExecutionCashClaimRecognition {
  atx::usize event_index{};
  atx::usize instrument_index{};
  atx::u64 instrument_id{};
  atx::usize period{};
  atx::i64 recognition_mark_ns{};
  atx::f64 removed_equity_dollars{};
  atx::f64 research_share_equivalents{};
  atx::f64 signed_claim_dollars{};
  atx::f64 recognition_pnl_dollars{};
  atx::f64 continued_annual_borrow_rate{};
};
// Same full calendar as streams; NaN outside mature intervals. Arrays are
// D-sized (one signal), empty for an empty event context. Fixed claim marks are
// separate from market equity returns; gross_flat includes their recognition
// bridge, detailed here. No claim recognition turnover or execution cost.
struct ExecutionCashClaimStreams {
  alpha::AlphaStreams streams;
  std::vector<atx::f64> signed_claim_dollars{}, receivable_dollars{}, payable_dollars{};
  std::vector<atx::f64> recognition_pnl_dollars{}, claim_borrow_dollars{}, settled_cash_dollars{};
  std::vector<ExecutionCashClaimRecognition> recognitions{};
  std::vector<ExecutionCashClaimEventUse> event_uses{};
  // This flag describes the policy, not an assertion that every run has claims.
  bool payment_dates_unknown{true};
};
} // namespace atx::engine::factory

// atx::engine::cost — Scaffold test (S6-0).
//
// Purpose: Verify that the seams Sprint S6 builds on have the assumed shape.
// Every assertion here documents a structural contract; if any breaks, S6 has
// drifted from reality before a single calibration line is written.
//
// Coverage:
//   1. exec::ImpactCfg default coefficients (Y, delta, gamma).
//   2. risk::OptimizerConfig::turnover_penalty default == 0.0 (κ starts free).
//   3. Normal (qty != 0) fill through a single-name Portfolio debits cash by
//      notional + fee.  This proves the cash-ledger path that S6-5 borrow
//      accrual relies on, using a normal fill rather than the forbidden
//      zero-qty path (see NOTE below).
//   4. cost/fwd.hpp compiles and its forward-declared structs are visible.
//
// NOTE — apply_fill(qty=0) ABORTS in Debug.
//   Portfolio::apply_fill (portfolio.hpp:173) has ATX_ASSERT(f.qty != 0).
//   A synthetic qty=0 fee-only fill would compute notional=0 and debit only
//   the fee — the correct semantic — but the assert fires first in a Debug
//   build, aborting the whole test binary.  We therefore do NOT test qty=0.
//   Instead this note records the finding and the ledger decision:
//   S6-5 uses Portfolio::accrue_financing(core::Decimal) (the ONE reviewed
//   engine touch) to debit borrow cost directly, bypassing apply_fill.

#include <gtest/gtest.h>

#include <array>
#include <span>

#include "atx/core/decimal.hpp"  // Decimal
#include "atx/core/types.hpp"    // f64, i64, u32

// Cost layer scaffold (S6-0) — forward declarations only; must compile clean.
#include "atx/engine/cost/fwd.hpp"

// Seam headers.
#include "atx/engine/exec/execution_sim.hpp" // ImpactCfg, SlippageCfg, CommissionCfg
#include "atx/engine/exec/payloads.hpp"      // FillPayload
#include "atx/engine/loop/types.hpp"         // InstrumentId
#include "atx/engine/portfolio/portfolio.hpp" // Portfolio
#include "atx/engine/risk/optimizer.hpp"     // OptimizerConfig

namespace {

using atx::core::Decimal;
using atx::engine::InstrumentId;
using atx::engine::Portfolio;
using atx::engine::exec::FillPayload;
using atx::engine::exec::ImpactCfg;
using atx::engine::risk::OptimizerConfig;

// ---- helpers ----------------------------------------------------------------

[[nodiscard]] InstrumentId inst(atx::u32 id) noexcept { return InstrumentId{id}; }
[[nodiscard]] Decimal dec(atx::i64 whole) noexcept { return Decimal::from_int(whole); }

// A fill with whole-unit price and an explicit fee (all Decimal-exact).
[[nodiscard]] FillPayload make_fill(atx::u32 id, atx::i64 qty, atx::i64 price,
                                    atx::i64 fee) noexcept {
  using atx::core::time::Timestamp;
  return FillPayload{inst(id), qty, dec(price), dec(fee), 0.0,
                     Timestamp::from_unix_nanos(1)};
}

// =============================================================================
//  Suite: CostScaffold
// =============================================================================

// 1. exec::ImpactCfg default coefficients — calibration target (S6-1).
//    Confirms the literature-mix defaults the spec says S6 will replace with
//    fitted values:  Y=1.0 (unit scale),  delta=0.5 (√ impact),  gamma=0.314.
TEST(CostScaffold, ImpactCfgDefaults) {
  const ImpactCfg cfg{};
  EXPECT_DOUBLE_EQ(cfg.Y, 1.0);
  EXPECT_DOUBLE_EQ(cfg.delta, 0.5);
  EXPECT_DOUBLE_EQ(cfg.gamma, 0.314);
}

// 2. risk::OptimizerConfig::turnover_penalty default == 0.0.
//    κ starts at zero (no turnover penalty).  S6-3 derives a non-zero κ from
//    calibrated cost and writes it into a fresh OptimizerConfig.
TEST(CostScaffold, OptimizerConfigTurnoverPenaltyDefault) {
  const OptimizerConfig cfg{};
  EXPECT_DOUBLE_EQ(cfg.turnover_penalty, 0.0);
}

// 3. Normal (qty != 0) fill debits cash by notional + fee.
//    Verifies the exact cash-ledger arithmetic that S6-5 borrow accrual relies
//    on.  We buy 100 shares at $50 with a $5 fee:
//      notional = 100 * 50 = 5000
//      debit    = notional + fee = 5005
//      cash     = 10000 - 5005 = 4995
//
//    The universe array MUST outlive the Portfolio (non-owning span).
TEST(CostScaffold, NormalFillDebitsNotionalPlusFee) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio port{dec(10000), std::span<const InstrumentId>{uni}};

  // Precondition: cash starts at 10,000 exactly.
  EXPECT_EQ(port.cash(), dec(10000));

  // Apply a buy of 100 shares at $50 with a $5 commission.
  port.apply_fill(make_fill(10, /*qty=*/100, /*price=*/50, /*fee=*/5));

  // notional = 100 * 50 = 5000;  debit = 5000 + 5 = 5005
  EXPECT_EQ(port.cash(), dec(4995));
}

// 4. cost/fwd.hpp compiles and forward-declared structs are name-visible.
//    A pointer-to-incomplete proves each name resolves in the namespace without
//    requiring a complete type.
TEST(CostScaffold, FwdHppForwardDeclarationsVisible) {
  // Pointer-to-incomplete is valid C++ and proves each name resolves in the
  // namespace without requiring a complete type. No sizeof (tidy-clean).
  [[maybe_unused]] atx::engine::cost::CalibratedCost *a = nullptr;
  [[maybe_unused]] atx::engine::cost::FitReport *b = nullptr;
  [[maybe_unused]] atx::engine::cost::CostKnobs *c = nullptr;
  [[maybe_unused]] atx::engine::cost::BorrowModel *d = nullptr;
  SUCCEED();
}

} // namespace

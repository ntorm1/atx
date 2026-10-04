// atx::engine::alpha -- the per-opcode table (op_traits.hpp, P9 lane S2) classifies every opcode
// exactly as the hand-written lists it replaced.
//
// Before S2 the typechecker (typecheck.{hpp,cpp}), the VM (vm.hpp is_cs_op / is_instrument_op /
// chunk_axis), the StreamingEngine (streaming_engine.hpp is_ts_op / is_cs_op / is_grouped_cs) and
// the oracle (oracle.cpp's grouped list) each kept their own copy. The golden sets below are those
// pre-S2 lists, copied from the base commit 1239a5ff (the VM's instrument rule kept as its
// numeric opcode range, as written). Every predicate now derived from the table must agree with
// them on every opcode value 0 .. kOpCodeCount - 1; the Library's lookahead_safe rail is the
// family's, and the table's own invariants (one row per opcode, axis per family, stateful iff a
// recurrence) hold.
//
// Naming: Subject_Condition_ExpectedResult (the brief's id: OpSig.PredicatesAgree).

#include <algorithm>
#include <array>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/op_traits.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streaming_engine.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx_test_alpha_op_traits {

namespace alpha = atx::engine::alpha;
using alpha::ChunkAxis;
using alpha::OpCode;
using alpha::OpFamily;
using atx::u8;
using atx::usize;

using OpList = std::span<const OpCode>;

[[nodiscard]] bool in(OpList list, OpCode op) {
  return std::find(list.begin(), list.end(), op) != list.end();
}

// ---- the pre-S2 lists (base 1239a5ff) ----------------------------------------------------------

// typecheck.hpp detail::is_cross_section; vm.hpp Engine::is_cs_op (the same 19).
constexpr std::array kCrossSection{
    OpCode::CsRank,    OpCode::CsZscore,  OpCode::CsScale,       OpCode::CsNormalize,
    OpCode::CsWinsorize, OpCode::CsDemeanG, OpCode::CsNeutG,     OpCode::CsRankG,
    OpCode::CsZscoreG, OpCode::CsCountG,  OpCode::CsMeanG,       OpCode::CsScaleG,
    OpCode::CsResidualize, OpCode::CsQuantile, OpCode::CsVecSum, OpCode::CsVecAvg,
    OpCode::CsBucket,  OpCode::CsResidOn, OpCode::CsSumG};

// typecheck.hpp needs_group_arg, streaming_engine.hpp is_grouped_cs, vm.hpp / oracle.cpp `grouped`.
constexpr std::array kGrouped{OpCode::CsDemeanG, OpCode::CsNeutG,  OpCode::CsRankG,
                          OpCode::CsZscoreG, OpCode::CsCountG, OpCode::CsMeanG,
                          OpCode::CsScaleG,  OpCode::CsResidualize, OpCode::CsSumG};

// typecheck.hpp is_shift_ts.
constexpr std::array kShift{OpCode::TsDelay, OpCode::TsDelta, OpCode::GroupDelay};

// typecheck.cpp is_rolling_ts (its `return true` cases).
constexpr std::array kRolling{
    OpCode::TsSum,         OpCode::TsMean,       OpCode::TsStd,        OpCode::TsVar,
    OpCode::TsMin,         OpCode::TsMax,        OpCode::TsArgMin,     OpCode::TsArgMax,
    OpCode::TsRank,        OpCode::TsCorr,       OpCode::TsCov,        OpCode::TsProduct,
    OpCode::TsDecayLinear, OpCode::TsEma,        OpCode::TsWma,        OpCode::TsSkew,
    OpCode::TsKurt,        OpCode::TsMed,        OpCode::TsMad,        OpCode::TsSlope,
    OpCode::TsRsquare,     OpCode::TsResid,      OpCode::TsZscore,     OpCode::TsBackfill,
    OpCode::TsAvDiff,      OpCode::TsQuantile,   OpCode::TsScale,      OpCode::TsCountNans,
    OpCode::TsRegression,  OpCode::TsDecayExp,   OpCode::TsEntropy,    OpCode::TsMoment,
    OpCode::OuTheta,       OpCode::OuHalflife,   OpCode::OuMean,       OpCode::OuZscore,
    OpCode::TsTopkMean,    OpCode::TsResidOn,    OpCode::TsBetaOn,     OpCode::TsCountIncreases,
    OpCode::TsSumMp,       OpCode::TsMeanMp,     OpCode::TsStdMp,      OpCode::TsZscoreMp,
    OpCode::TsMinMp,       OpCode::TsMaxMp,      OpCode::TsDecayLinearMp, OpCode::TsCorrMp};

// streaming_engine.hpp is_ts_op.
constexpr std::array kStreamingTs{
    OpCode::TsDelay,      OpCode::TsDelta,    OpCode::TsSum,      OpCode::TsMean,
    OpCode::TsStd,        OpCode::TsVar,      OpCode::TsMin,      OpCode::TsMax,
    OpCode::TsArgMin,     OpCode::TsArgMax,   OpCode::TsRank,     OpCode::TsCorr,
    OpCode::TsCov,        OpCode::TsProduct,  OpCode::TsDecayLinear, OpCode::TsEma,
    OpCode::TsWma,        OpCode::TsSkew,     OpCode::TsKurt,     OpCode::TsMed,
    OpCode::TsMad,        OpCode::TsSlope,    OpCode::TsRsquare,  OpCode::TsResid,
    OpCode::TsZscore,     OpCode::TsBackfill, OpCode::TsAvDiff,   OpCode::TsQuantile,
    OpCode::TsScale,      OpCode::TsCountNans, OpCode::TsRegression, OpCode::TsDecayExp,
    OpCode::TsEntropy,    OpCode::TsMoment,   OpCode::OuTheta,    OpCode::OuHalflife,
    OpCode::OuMean,       OpCode::OuZscore,   OpCode::GroupDelay};

// streaming_engine.hpp is_cs_op: the cross-sectional list without the W2 rows.
constexpr std::array kStreamingCs{
    OpCode::CsRank,    OpCode::CsZscore,  OpCode::CsScale,       OpCode::CsNormalize,
    OpCode::CsWinsorize, OpCode::CsDemeanG, OpCode::CsNeutG,     OpCode::CsRankG,
    OpCode::CsZscoreG, OpCode::CsCountG,  OpCode::CsMeanG,       OpCode::CsScaleG,
    OpCode::CsResidualize, OpCode::CsQuantile, OpCode::CsVecSum, OpCode::CsVecAvg,
    OpCode::CsSumG};

// vm.hpp Engine::is_instrument_op, as written (a numeric opcode range plus named extras).
[[nodiscard]] bool old_vm_instrument_op(OpCode op) {
  const auto v = static_cast<u8>(op);
  const bool ts_block =
      v >= static_cast<u8>(OpCode::TsDelay) && v <= static_cast<u8>(OpCode::OuFilter);
  return ts_block || op == OpCode::KalmanReg || op == OpCode::OuTheta ||
         op == OpCode::OuHalflife || op == OpCode::OuMean || op == OpCode::OuZscore ||
         alpha::detail::is_lit_ts_op(op) || op == OpCode::GroupDelay;
}

// vm.hpp Engine::chunk_axis, as written.
[[nodiscard]] ChunkAxis old_vm_chunk_axis(OpCode op) {
  if (op == OpCode::LoadField || in(kCrossSection, op) || alpha::detail::is_asof_op(op)) {
    return ChunkAxis::Dates;
  }
  if (old_vm_instrument_op(op)) {
    return ChunkAxis::Instruments;
  }
  return ChunkAxis::Cells;
}

constexpr std::array kRecurrence{OpCode::TradeWhen, OpCode::Hump, OpCode::KalmanLevel,
                             OpCode::OuFilter, OpCode::KalmanReg};

[[nodiscard]] std::string label(usize v) { return "opcode " + std::to_string(v); }

// ---- tests -------------------------------------------------------------------------------------

TEST(OpSig, PredicatesAgree) {
  ASSERT_EQ(alpha::kOpTraits.size(), static_cast<usize>(OpCode::AsofRankCov) + 1U);
  for (usize v = 0; v < alpha::kOpTraits.size(); ++v) {
    const auto op = static_cast<OpCode>(static_cast<u8>(v));
    SCOPED_TRACE(label(v));
    // typecheck
    EXPECT_EQ(alpha::detail::is_cross_section(op), in(kCrossSection, op));
    EXPECT_EQ(alpha::detail::needs_group_arg(op), in(kGrouped, op));
    EXPECT_EQ(alpha::detail::is_shift_ts(op), in(kShift, op));
    EXPECT_EQ(alpha::detail::is_rolling_ts(op), in(kRolling, op));
    EXPECT_EQ(alpha::detail::is_time_series(op), in(kShift, op) || in(kRolling, op));
    // VM
    EXPECT_EQ(alpha::Engine::chunk_axis(op), old_vm_chunk_axis(op));
    EXPECT_EQ(alpha::detail::is_instrument_axis_op(op), old_vm_instrument_op(op));
    // StreamingEngine
    EXPECT_EQ(alpha::streaming_detail::is_ts_op(op), in(kStreamingTs, op));
    EXPECT_EQ(alpha::streaming_detail::is_cs_op(op), in(kStreamingCs, op));
    EXPECT_EQ(alpha::streaming_detail::is_grouped_cs(op), in(kGrouped, op));
    // oracle.cpp's grouped list and the table's own derived predicates
    EXPECT_EQ(alpha::detail::needs_group_operand(op), in(kGrouped, op));
    EXPECT_EQ(alpha::detail::is_cross_section_op(op), in(kCrossSection, op));
    EXPECT_EQ(alpha::detail::is_shift_op(op), in(kShift, op));
    EXPECT_EQ(alpha::detail::is_rolling_op(op), in(kRolling, op));
    EXPECT_EQ(alpha::detail::is_recurrence_op(op), in(kRecurrence, op));
    // Every built-in family reads only rows <= t.
    EXPECT_TRUE(alpha::detail::op_lookahead_safe(op));
  }
}

// The table's invariants: stateful iff a recurrence, a group operand only for cross-sectional ops,
// each family on its own axis, and an opcode value outside the enumeration reads as Housekeeping.
TEST(OpSig, TableRowsFollowTheirFamily) {
  for (usize v = 0; v < alpha::kOpTraits.size(); ++v) {
    const auto op = static_cast<OpCode>(static_cast<u8>(v));
    SCOPED_TRACE(label(v));
    const alpha::OpTraits t = alpha::op_traits(op);
    EXPECT_EQ(t.stateful, t.family == OpFamily::Recurrence);
    EXPECT_TRUE(!t.needs_group || t.family == OpFamily::CrossSection);
    switch (t.family) {
    case OpFamily::Leaf:
      EXPECT_EQ(t.axis, op == OpCode::LoadField ? ChunkAxis::Dates : ChunkAxis::Cells);
      break;
    case OpFamily::CrossSection:
    case OpFamily::AsOf:
      EXPECT_EQ(t.axis, ChunkAxis::Dates);
      break;
    case OpFamily::Shift:
    case OpFamily::Rolling:
    case OpFamily::Recurrence:
      EXPECT_EQ(t.axis, ChunkAxis::Instruments);
      break;
    case OpFamily::ElementWise:
    case OpFamily::Record:
    case OpFamily::Housekeeping:
      EXPECT_EQ(t.axis, ChunkAxis::Cells);
      break;
    }
  }
  EXPECT_EQ(alpha::op_traits(OpCode::StoreAlpha).family, OpFamily::Housekeeping);
  EXPECT_EQ(alpha::op_traits(OpCode::Free).family, OpFamily::Housekeeping);
  EXPECT_EQ(alpha::op_traits(OpCode::Pin).family, OpFamily::Record);
  EXPECT_EQ(alpha::op_traits(OpCode::GroupCross).family, OpFamily::ElementWise);
  EXPECT_EQ(alpha::op_traits(OpCode::AsofRankCov).family, OpFamily::AsOf);
  const alpha::OpTraits outside = alpha::op_traits(static_cast<OpCode>(u8{250}));
  EXPECT_EQ(outside.family, OpFamily::Housekeeping);
  EXPECT_EQ(outside.axis, ChunkAxis::Cells);
  EXPECT_FALSE(outside.needs_group);
  EXPECT_FALSE(outside.stateful);
}

// OpSig::lookahead_safe of every Library row is derived from its opcode's family (all true for the
// built-in, literature and formulaic tables, as their literals say), and a register_op row that
// declares itself unsafe stays unsafe.
TEST(OpSig, LookaheadSafeDerivedFromTheTable) {
  const alpha::Library lib;
  usize rows = 0;
  for (const auto table : {alpha::detail::builtin_ops(), alpha::detail::literature_ops(),
                           alpha::detail::formulaic_ops()}) {
    for (const alpha::OpSig &sig : table) {
      SCOPED_TRACE(std::string(sig.name));
      const alpha::OpSig *row = lib.find(sig.name);
      ASSERT_NE(row, nullptr);
      EXPECT_EQ(row->lookahead_safe, alpha::detail::op_lookahead_safe(sig.opcode));
      EXPECT_EQ(row->lookahead_safe, sig.lookahead_safe);
      ++rows;
    }
  }
  EXPECT_EQ(rows, lib.size());
  alpha::Library custom;
  alpha::OpSig unsafe = alpha::detail::builtin_ops().front();
  unsafe.name = "p9_s2_unsafe_probe";
  unsafe.lookahead_safe = false;
  ASSERT_TRUE(custom.register_op(unsafe));
  const alpha::OpSig *registered = custom.find("p9_s2_unsafe_probe");
  ASSERT_NE(registered, nullptr);
  EXPECT_FALSE(registered->lookahead_safe);
}

} // namespace atx_test_alpha_op_traits

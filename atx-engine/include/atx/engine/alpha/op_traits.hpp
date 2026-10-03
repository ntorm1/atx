#pragma once

// atx::engine::alpha — the per-opcode semantic table (P9 lane S2, DSL/IC review §7 item 5).
//
// One row per OpCode, indexed by the opcode's u8 value: the op's family, the panel axis its kernel
// is independent along (the range-execution and worker-band axis), whether operand 1 must be a
// Group classifier, and whether it carries state from the panel's first date. Before this table
// the typechecker, the VM, the StreamingEngine and the oracle each kept their own hand-written
// lists (the cross-sectional list x4, the grouped list x4, the rolling and shift families, the
// instrument axis as a numeric opcode range), and the copies had already drifted. Every one of
// those predicates is now derived from this table, so a new opcode is classified exactly once:
//   * op_traits_row() is an exhaustive switch with no default, so an opcode without a row is a
//     -Wswitch error under /W4 /WX;
//   * kOpTraits materialises it as a constexpr array of exactly AsofRankCov + 1 rows, one per
//     opcode (static_assert), so a lookup is one indexed load;
//   * the kernel DISPATCH switches stay exhaustive and separate (vm.hpp dispatch_range, oracle.hpp
//     run): this table classifies, it does not dispatch.
// The Library's OpSig::lookahead_safe rail is derived from the family as well (op_lookahead_safe,
// registry.cpp), instead of a hand-set bool per catalogue row.
//
// Header-only; everything constexpr. No dependency beyond the opcode enumeration.

#include <array>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/registry.hpp" // OpCode

namespace atx::engine::alpha {

// The panel axis an opcode's kernel is independent along (vm.hpp "Range execution vocabulary"):
//   * Cells       -- element-wise maps, Const, Select, the record plumbing: cell i depends only on
//                    input cell i;
//   * Dates       -- LoadField, every cross-sectional op and the v8 as-of family: date row t
//                    depends only on input rows <= t (the inputs are complete before the op runs);
//   * Instruments -- every shift, rolling and recurrence op: instrument column j depends only on
//                    input column j.
// A kernel run over a sub-range [lo, hi) of its axis writes exactly the cells the full-range call
// writes for that range, with the same per-cell arithmetic in the same order, so any partition of
// the axis across workers is bit-identical to the serial full-range call.
enum class ChunkAxis : atx::u8 { Cells, Dates, Instruments };

// What an opcode computes, by the rows of the panel a value at (t, j) reads.
enum class OpFamily : atx::u8 {
  Leaf,         // LoadField / Const: no operand
  ElementWise,  // arithmetic, comparison, logic, select, group_cross: cell (t, j) only
  CrossSection, // every Cs* op, bucket, cs_resid_on, group_sum: date row t only
  Shift,        // delay, delta, group_delay: the value d rows ago (lookback d + child)
  Rolling,      // trailing window [t-d+1, t] (lookback (d-1) + child)
  Recurrence,   // forward scan from the panel's first date: prior state + inputs <= t
  AsOf,         // the v8 as-of rank family: rows t+1-d-j .. t (lookback (d-1) + j + child)
  Record,       // multi-output plumbing (pack2/pack3, pin, split2): cell (t, j) only
  Housekeeping, // StoreAlpha / Free: no kernel
};

struct OpTraits {
  OpFamily family{OpFamily::Housekeeping};
  ChunkAxis axis{ChunkAxis::Cells};
  bool needs_group{false}; // operand 1 must carry a Group classifier
  bool stateful{false};    // carries state from the panel's first date (a recurrence)
};

namespace detail {

// The one row of every opcode. Exhaustive: no default, so a new enumerator fails to compile here.
[[nodiscard]] constexpr OpTraits op_traits_row(OpCode op) noexcept {
  constexpr OpTraits kCells{OpFamily::ElementWise, ChunkAxis::Cells, false, false};
  constexpr OpTraits kCs{OpFamily::CrossSection, ChunkAxis::Dates, false, false};
  constexpr OpTraits kCsGroup{OpFamily::CrossSection, ChunkAxis::Dates, true, false};
  constexpr OpTraits kShift{OpFamily::Shift, ChunkAxis::Instruments, false, false};
  constexpr OpTraits kRolling{OpFamily::Rolling, ChunkAxis::Instruments, false, false};
  constexpr OpTraits kRecurrence{OpFamily::Recurrence, ChunkAxis::Instruments, false, true};
  constexpr OpTraits kAsOf{OpFamily::AsOf, ChunkAxis::Dates, false, false};
  constexpr OpTraits kRecord{OpFamily::Record, ChunkAxis::Cells, false, false};
  constexpr OpTraits kHousekeeping{OpFamily::Housekeeping, ChunkAxis::Cells, false, false};
  switch (op) {
  case OpCode::LoadField:
    return OpTraits{OpFamily::Leaf, ChunkAxis::Dates, false, false};
  case OpCode::Const:
    return OpTraits{OpFamily::Leaf, ChunkAxis::Cells, false, false};
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::Div:
  case OpCode::Neg:
  case OpCode::Abs:
  case OpCode::Sign:
  case OpCode::Log:
  case OpCode::Sigmoid:
  case OpCode::Tanh:
  case OpCode::Pow:
  case OpCode::Spow:
  case OpCode::MinP:
  case OpCode::MaxP:
  case OpCode::CmpLt:
  case OpCode::CmpGt:
  case OpCode::CmpLe:
  case OpCode::CmpGe:
  case OpCode::CmpEq:
  case OpCode::CmpNe:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::Not:
  case OpCode::Select:
  case OpCode::GroupCross: // W2: the product label of two classifiers, cell by cell
    return kCells;
  case OpCode::CsRank:
  case OpCode::CsZscore:
  case OpCode::CsScale:
  case OpCode::CsNormalize:
  case OpCode::CsWinsorize:
  case OpCode::CsQuantile:
  case OpCode::CsVecSum:
  case OpCode::CsVecAvg:
  case OpCode::CsBucket:  // W2
  case OpCode::CsResidOn: // W2
    return kCs;
  case OpCode::CsDemeanG:
  case OpCode::CsNeutG:
  case OpCode::CsRankG:
  case OpCode::CsZscoreG:
  case OpCode::CsCountG:
  case OpCode::CsMeanG:
  case OpCode::CsScaleG:
  case OpCode::CsResidualize:
  case OpCode::CsSumG: // v8 YOPS group_sum
    return kCsGroup;
  case OpCode::TsDelay:
  case OpCode::TsDelta:
  case OpCode::GroupDelay: // v8 YOPS: delay's shift on a classifier
    return kShift;
  case OpCode::TsSum:
  case OpCode::TsMean:
  case OpCode::TsStd:
  case OpCode::TsVar:
  case OpCode::TsMin:
  case OpCode::TsMax:
  case OpCode::TsArgMin:
  case OpCode::TsArgMax:
  case OpCode::TsRank:
  case OpCode::TsCorr:
  case OpCode::TsCov:
  case OpCode::TsProduct:
  case OpCode::TsDecayLinear:
  case OpCode::TsEma:
  case OpCode::TsWma:
  case OpCode::TsSkew:
  case OpCode::TsKurt:
  case OpCode::TsMed:
  case OpCode::TsMad:
  case OpCode::TsSlope:
  case OpCode::TsRsquare:
  case OpCode::TsResid:
  case OpCode::TsZscore:
  case OpCode::TsBackfill:
  case OpCode::TsAvDiff:
  case OpCode::TsQuantile:
  case OpCode::TsScale:
  case OpCode::TsCountNans:
  case OpCode::TsRegression:
  case OpCode::TsDecayExp:
  case OpCode::TsEntropy:
  case OpCode::TsMoment:
  case OpCode::OuTheta:
  case OpCode::OuHalflife:
  case OpCode::OuMean:
  case OpCode::OuZscore:
  case OpCode::TsTopkMean:       // W2
  case OpCode::TsResidOn:        // W2
  case OpCode::TsBetaOn:         // W2
  case OpCode::TsCountIncreases: // W2
  case OpCode::TsSumMp:          // W2 min-periods family
  case OpCode::TsMeanMp:
  case OpCode::TsStdMp:
  case OpCode::TsZscoreMp:
  case OpCode::TsMinMp:
  case OpCode::TsMaxMp:
  case OpCode::TsDecayLinearMp:
  case OpCode::TsCorrMp:
    return kRolling;
  case OpCode::TradeWhen:
  case OpCode::Hump:
  case OpCode::KalmanLevel:
  case OpCode::OuFilter:
  case OpCode::KalmanReg:
    return kRecurrence;
  case OpCode::AsofRankTsRank:
  case OpCode::AsofRankTsMin:
  case OpCode::AsofRankDecayLinear:
  case OpCode::AsofRankCorr:
  case OpCode::AsofRankCov:
    return kAsOf;
  case OpCode::Pin:
  case OpCode::Split2:
  case OpCode::ArgPack:
    return kRecord;
  case OpCode::StoreAlpha:
  case OpCode::Free:
    return kHousekeeping;
  }
  return kHousekeeping; // unreachable for a valid OpCode (the switch is exhaustive)
}

// The number of opcodes: AsofRankCov is the last enumerator (registry.hpp freezes its id).
inline constexpr atx::usize kOpCodeCount = static_cast<atx::usize>(OpCode::AsofRankCov) + 1U;

[[nodiscard]] consteval std::array<OpTraits, kOpCodeCount> make_op_traits() noexcept {
  std::array<OpTraits, kOpCodeCount> table{};
  for (atx::usize i = 0; i < kOpCodeCount; ++i) {
    table[i] = op_traits_row(static_cast<OpCode>(static_cast<atx::u8>(i)));
  }
  return table;
}

} // namespace detail

// The table: kOpTraits[u8(op)] is op's row.
inline constexpr std::array<OpTraits, detail::kOpCodeCount> kOpTraits = detail::make_op_traits();
static_assert(kOpTraits.size() == static_cast<atx::usize>(OpCode::AsofRankCov) + 1U,
              "one OpTraits row per opcode");

// op's row. A value outside the enumeration (a hand-built instruction) reads as Housekeeping on
// the Cells axis: no family predicate holds for it and every kernel dispatch refuses it.
[[nodiscard]] constexpr OpTraits op_traits(OpCode op) noexcept {
  const auto i = static_cast<atx::usize>(op);
  return i < kOpTraits.size() ? kOpTraits[i] : OpTraits{};
}

namespace detail {

// Every family reads only rows <= t of its operands; a family that read a later row would return
// false here and make every op of it lookahead-unsafe. Exhaustive: a new family must decide.
[[nodiscard]] constexpr bool family_reads_past_only(OpFamily f) noexcept {
  switch (f) {
  case OpFamily::Leaf:
  case OpFamily::ElementWise:
  case OpFamily::CrossSection:
  case OpFamily::Shift:
  case OpFamily::Rolling:
  case OpFamily::Recurrence:
  case OpFamily::AsOf:
  case OpFamily::Record:
  case OpFamily::Housekeeping:
    return true;
  }
  return false;
}

// The axis rule each family implies (Leaf: LoadField is per date row, Const per cell).
[[nodiscard]] constexpr bool axis_fits_family(const OpTraits &t) noexcept {
  switch (t.family) {
  case OpFamily::Leaf:
    return t.axis != ChunkAxis::Instruments;
  case OpFamily::ElementWise:
  case OpFamily::Record:
  case OpFamily::Housekeeping:
    return t.axis == ChunkAxis::Cells;
  case OpFamily::CrossSection:
  case OpFamily::AsOf:
    return t.axis == ChunkAxis::Dates;
  case OpFamily::Shift:
  case OpFamily::Rolling:
  case OpFamily::Recurrence:
    return t.axis == ChunkAxis::Instruments;
  }
  return false;
}

[[nodiscard]] consteval bool op_traits_consistent() noexcept {
  for (const OpTraits &t : kOpTraits) {
    if (t.stateful != (t.family == OpFamily::Recurrence) ||
        (t.needs_group && t.family != OpFamily::CrossSection) || !axis_fits_family(t)) {
      return false;
    }
  }
  return true;
}
static_assert(op_traits_consistent(),
              "OpTraits: stateful iff a recurrence; only cross-sectional ops need a group; "
              "each family on its own axis");

// ---- derived predicates (the only classification lists; see the file comment) -------------
[[nodiscard]] constexpr bool is_cross_section_op(OpCode op) noexcept {
  return op_traits(op).family == OpFamily::CrossSection;
}
[[nodiscard]] constexpr bool is_shift_op(OpCode op) noexcept {
  return op_traits(op).family == OpFamily::Shift;
}
[[nodiscard]] constexpr bool is_rolling_op(OpCode op) noexcept {
  return op_traits(op).family == OpFamily::Rolling;
}
[[nodiscard]] constexpr bool is_recurrence_op(OpCode op) noexcept {
  return op_traits(op).family == OpFamily::Recurrence;
}
[[nodiscard]] constexpr bool needs_group_operand(OpCode op) noexcept {
  return op_traits(op).needs_group;
}
[[nodiscard]] constexpr ChunkAxis op_chunk_axis(OpCode op) noexcept { return op_traits(op).axis; }
[[nodiscard]] constexpr bool is_instrument_axis_op(OpCode op) noexcept {
  return op_traits(op).axis == ChunkAxis::Instruments;
}
// The OpSig::lookahead_safe rail of every catalogue row (registry.cpp).
[[nodiscard]] constexpr bool op_lookahead_safe(OpCode op) noexcept {
  return family_reads_past_only(op_traits(op).family);
}

} // namespace detail

} // namespace atx::engine::alpha

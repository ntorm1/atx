#pragma once

// atx::engine::alpha — StreamingEngine: one-date-at-a-time evaluation of a
// compiled Program (Lane 1).
//
// The batch Engine (vm.hpp) evaluates a Program over a whole Panel; a live
// signal that re-runs it every day pays O(lookback) per day. StreamingEngine
// instead keeps, per instruction, exactly the state the batch kernel carries
// down a column, and advances it by ONE date per step():
//
//   * element-wise / logical / select / Cs*   — today's row only (the SAME
//     scalar kernels and cs_ops row kernels the VM calls).
//   * Ts* windowed ops                        — a ring of the input's last d+1
//     rows per instruction, plus the op's online state where the VM carries one:
//       - ts_sum / ts_mean       Neumaier running Σ + missing count (ResearchFast;
//                                under AuditExact they are Generic, W0-A0 A-13)
//       - ts_min / ts_max / ts_scale  per-instrument monotonic deque (VM deque)
//       - variance family         Welford/Neumaier state (ResearchFast only)
//       - decay_linear / wma / slope / rsquare / resid  sliding lanes
//                                 (ResearchFast only; ts_sliding.hpp)
//       - everything else         the batch per-cell kernel (ts_value_at /
//                                 ts_pair_at / ou_value_at) on the ring window.
//   * recurrences (trade_when / hump / kalman_level / ou_filter / kalman_reg) —
//     the per-instrument carried state of the VM's forward scan.
//
// EQUIVALENCE CONTRACT: after warm(P) over dates [0, T0) and step() over dates
// [T0, T), each step's output row equals the corresponding row of the batch
// Engine::evaluate over the concatenated panel [0, T) in the SAME EvalMode —
// bit-for-bit. Every state machine above performs the identical floating-point
// operations in the identical order as its batch counterpart (they share the
// kernels, or restate a trivially identical add/subtract), and the online
// states are anchored at the SAME first date (the first warm date == batch
// date 0). Per-step cost is independent of the lookback length: it is
// O(ops * instruments * d) at worst (the generic windowed ops), O(ops *
// instruments) for the online ones.
//
// SCOPE / REFUSALS (create() returns NotImplemented): a scalar operand (a Ts
// window, cs_scale/winsorize/quantile's factor, hump's threshold) that is not a
// Const — the batch VM reads such operands from cell (date 0, instrument 0), a
// value a streaming evaluator cannot know in general — and a window of 0.
//
// Header-only. Owns its Program copy and every buffer; step() allocates nothing
// once the first step has sized the scratch.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/cs_ops.hpp"
#include "atx/engine/alpha/lit_ops.hpp" // W2 literature kernels (shared with the VM)
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/state_ops.hpp"
#include "atx/engine/alpha/ts_ops.hpp"
#include "atx/engine/alpha/ts_sliding.hpp"
#include "atx/engine/alpha/vm.hpp" // EvalMode + the vm_* scalar kernels

namespace atx::engine::alpha {

// One date's raw inputs. `fields[i]` is the row (length == instruments) of
// Program::fields[i]; `universe` is the in-universe mask row (1 == in), empty ==
// every instrument in-universe. Non-owning; valid for the step() call only.
struct CrossSection {
  std::vector<std::span<const atx::f64>> fields;
  std::span<const std::uint8_t> universe;
};

namespace streaming_detail {

// How a Ts instruction advances (mirrors vm.hpp eval_time_series routing).
enum class TsKind : atx::u8 {
  Lookback,  // delay / delta: x[t-d]
  RunSum,    // ts_sum / ts_mean under ResearchFast (Neumaier slide; W0-A0 A-13)
  Extreme,   // ts_min / ts_max / ts_scale (monotonic deque, every mode)
  Welford,   // var / std / zscore / av_diff under ResearchFast
  Decay,     // decay_linear / wma under ResearchFast (sliding lane)
  TimeReg,   // slope / rsquare / resid under ResearchFast (sliding lane)
  CoMoment,  // corr / cov / pair-regression under ResearchFast
  ExpDecay,  // finite-window exponential decay under ResearchFast
  Generic,   // batch per-cell kernel over the ring window
};

// Per-Ts-instruction state. Rings hold the op's input rows for dates
// t-d .. t (capacity d+1), row r at (date % cap).
struct TsState {
  TsKind kind{TsKind::Generic};
  atx::usize d{0};
  atx::usize cap{1};
  bool pair{false}; // corr / cov / regression
  bool ou{false};   // ou_theta / ou_halflife / ou_mean / ou_zscore
  std::vector<atx::f64> ring_x;
  std::vector<atx::f64> ring_y;
  // RunSum (W0-A0: Neumaier TsvRunSum) / Extreme bookkeeping (per instrument).
  std::vector<detail::TsvRunSum> runsum;
  std::vector<atx::usize> miss;
  // Extreme deques: per instrument a ring of `cap` date indices.
  std::vector<atx::u64> dq_lo;
  std::vector<atx::u64> dq_hi;
  std::vector<atx::usize> lo_head;
  std::vector<atx::usize> lo_size;
  std::vector<atx::usize> hi_head;
  std::vector<atx::usize> hi_size;
  // ResearchFast lanes.
  std::vector<detail::TsvWelfordState> welford;
  std::vector<sliding::LinDecayLane> decay;
  std::vector<sliding::TimeRegLane> timereg;
  std::vector<sliding::CoMomentLane> comoment;
  sliding::ExpDecayCoefficients exp_coeff;
  std::vector<sliding::ExpDecayLane> exp_decay;
};

// Per-W2-trailing-window-instruction state (lit_ops.hpp): a ring of the last `d`
// rows of each of the op's `n_in` inputs, row of date s at (s % d); the cell
// kernel then runs on exactly the window the batch VM gathers.
struct LitTsState {
  atx::usize d{0};
  atx::usize n_in{0};
  std::vector<atx::f64> ring; // d * n_in * instruments
};

// Per-recurrence-instruction carried state.
struct RecState {
  std::vector<atx::f64> prior;               // trade_when
  std::vector<detail::HumpState> hump;       // hump (W0-A0 A-02: prior + NaN-run)
  std::vector<detail::KalmanLevelState> kl;  // kalman_level
  std::vector<atx::f64> xhat;                // ou_filter
  std::vector<detail::KalmanRegState> kr;    // kalman_reg
  std::vector<std::uint8_t> seeded;          // kalman_level / ou_filter / kalman_reg
};

[[nodiscard]] inline bool is_ts_op(OpCode op) noexcept {
  switch (op) {
  case OpCode::TsDelay:
  case OpCode::TsDelta:
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
    return true;
  default:
    return false;
  }
}

[[nodiscard]] inline bool is_cs_op(OpCode op) noexcept {
  switch (op) {
  case OpCode::CsRank:
  case OpCode::CsZscore:
  case OpCode::CsScale:
  case OpCode::CsNormalize:
  case OpCode::CsWinsorize:
  case OpCode::CsDemeanG:
  case OpCode::CsNeutG:
  case OpCode::CsRankG:
  case OpCode::CsZscoreG:
  case OpCode::CsCountG:
  case OpCode::CsMeanG:
  case OpCode::CsScaleG:
  case OpCode::CsResidualize:
  case OpCode::CsQuantile:
  case OpCode::CsVecSum:
  case OpCode::CsVecAvg:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] inline bool is_grouped_cs(OpCode op) noexcept {
  return op == OpCode::CsDemeanG || op == OpCode::CsNeutG || op == OpCode::CsRankG ||
         op == OpCode::CsZscoreG || op == OpCode::CsCountG || op == OpCode::CsMeanG ||
         op == OpCode::CsScaleG || op == OpCode::CsResidualize;
}

// Classify a Ts op exactly as vm.hpp eval_time_series routes it.
[[nodiscard]] inline TsKind classify(OpCode op, EvalMode mode) noexcept {
  if (op == OpCode::TsDelay || op == OpCode::TsDelta) {
    return TsKind::Lookback;
  }
  // W0-A0 (A-13): AuditExact ts_sum/ts_mean are the batch per-window recompute
  // (Generic below); only ResearchFast slides them online.
  if ((op == OpCode::TsSum || op == OpCode::TsMean) && mode == EvalMode::ResearchFast) {
    return TsKind::RunSum;
  }
  if (op == OpCode::TsMin || op == OpCode::TsMax || op == OpCode::TsScale) {
    return TsKind::Extreme;
  }
  if (mode == EvalMode::ResearchFast && sliding::is_comoment_op(op)) return TsKind::CoMoment;
  if (mode == EvalMode::ResearchFast && op == OpCode::TsDecayExp) return TsKind::ExpDecay;
  // Order-stat online ops (TsRank/Med/Quantile) are bit-exact with the batch
  // per-cell kernel, so the Generic recompute reproduces them exactly.
  if (mode == EvalMode::ResearchFast && detail::ts_is_online_variance_op(op)) {
    if (sliding::is_decay_op(op)) {
      return TsKind::Decay;
    }
    if (sliding::is_timereg_op(op)) {
      return TsKind::TimeReg;
    }
    return TsKind::Welford;
  }
  return TsKind::Generic;
}

[[nodiscard]] inline detail::TsvVarOut welford_out(OpCode op) noexcept {
  switch (op) {
  case OpCode::TsVar:
    return detail::TsvVarOut::Var;
  case OpCode::TsStd:
    return detail::TsvVarOut::Std;
  case OpCode::TsZscore:
    return detail::TsvVarOut::Zscore;
  default:
    return detail::TsvVarOut::AvDiff;
  }
}

} // namespace streaming_detail

// =========================================================================
//  StreamingEngine
// =========================================================================
class StreamingEngine {
public:
  // Validate `prog` for streaming and size every per-instruction state for
  // `n_instruments`. Err(NotImplemented) for a non-Const scalar operand or a
  // zero window; Err(InvalidArgument) for n_instruments == 0.
  [[nodiscard]] static atx::core::Result<StreamingEngine> create(const Program &prog,
                                                                 atx::u32 n_instruments,
                                                                 EvalMode mode) {
    if (n_instruments == 0) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "StreamingEngine: n_instruments must be > 0");
    }
    StreamingEngine se;
    se.prog_ = prog;
    se.inst_ = n_instruments;
    se.mode_ = mode;
    ATX_TRY_VOID(se.analyze());
    return atx::core::Ok(std::move(se));
  }

  // Prime the state from history: steps every date of `lookback` in order. The
  // panel must carry every Program field and have n_instruments columns.
  [[nodiscard]] atx::core::Status warm(const Panel &lookback) {
    if (lookback.instruments() != inst_) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "StreamingEngine::warm: instrument count mismatch");
    }
    std::vector<FieldId> fid(prog_.fields.size());
    for (atx::usize i = 0; i < prog_.fields.size(); ++i) {
      ATX_TRY(const FieldId f, lookback.field_id(prog_.fields[i]));
      fid[i] = f;
    }
    CrossSection cs;
    cs.fields.resize(fid.size());
    std::vector<std::uint8_t> uni(inst_);
    for (atx::usize d = 0; d < lookback.dates(); ++d) {
      for (atx::usize i = 0; i < fid.size(); ++i) {
        cs.fields[i] = lookback.field_all(fid[i]).subspan(d * inst_, inst_);
      }
      for (atx::usize j = 0; j < inst_; ++j) {
        uni[j] = lookback.in_universe(d, j) ? 1 : 0;
      }
      cs.universe = uni;
      if (auto r = step(cs); !r) {
        return atx::core::Err(r.error());
      }
    }
    return atx::core::Ok();
  }

  // Advance one date. Returns root 0's output row (see output() for others).
  [[nodiscard]] atx::core::Result<std::span<const atx::f64>> step(const CrossSection &today) {
    if (today.fields.size() != prog_.fields.size() ||
        (!today.universe.empty() && today.universe.size() != inst_)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "StreamingEngine::step: cross-section shape mismatch");
    }
    for (const std::span<const atx::f64> f : today.fields) {
      if (f.size() != inst_) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "StreamingEngine::step: field row length != instruments");
      }
    }
    for (atx::usize k = 0; k < prog_.code.size(); ++k) {
      exec(k, today);
    }
    ++t_;
    return atx::core::Ok(output(0));
  }

  // Root `r`'s output row for the last stepped date (NaN before any step).
  [[nodiscard]] std::span<const atx::f64> output(atx::usize r) const noexcept {
    return std::span<const atx::f64>{out_.data() + r * inst_, inst_};
  }
  [[nodiscard]] atx::usize num_outputs() const noexcept { return prog_.roots.size(); }
  [[nodiscard]] atx::usize dates_seen() const noexcept { return t_; }

private:
  using TsState = streaming_detail::TsState;
  using TsKind = streaming_detail::TsKind;
  using RecState = streaming_detail::RecState;

  StreamingEngine() = default;

  [[nodiscard]] std::span<atx::f64> row(SlotId s) noexcept {
    return std::span<atx::f64>{slots_.data() + static_cast<atx::usize>(s) * inst_, inst_};
  }
  [[nodiscard]] std::span<const atx::f64> src(const Instr &in, atx::usize k) noexcept {
    return row(in.src.at(k));
  }

  // ---- create-time analysis ----------------------------------------------
  [[nodiscard]] atx::core::Status analyze() {
    const atx::usize n = prog_.code.size();
    std::vector<atx::usize> producer(prog_.num_slots + 3, n); // slot -> producing instr
    scalar_.assign(n, std::array<atx::f64, 3>{0.0, 0.0, 0.0});
    ts_.assign(n, TsState{});
    rec_.assign(n, RecState{});
    lit_.assign(n, streaming_detail::LitTsState{});
    const auto const_of = [&](SlotId s, atx::f64 &v) {
      if (s == kNoSlot || s >= producer.size() || producer[s] >= n ||
          prog_.code[producer[s]].op != OpCode::Const) {
        return false;
      }
      v = prog_.code[producer[s]].imm[0];
      return true;
    };
    for (atx::usize k = 0; k < n; ++k) {
      const Instr &in = prog_.code[k];
      if (streaming_detail::is_ts_op(in.op)) {
        ATX_TRY_VOID(analyze_ts(k, in, const_of));
      } else if (detail::is_lit_ts_op(in.op)) {
        ATX_TRY_VOID(analyze_lit_ts(k, in, const_of));
      } else if (streaming_detail::is_cs_op(in.op)) {
        const bool needs_scalar = in.op == OpCode::CsScale || in.op == OpCode::CsWinsorize ||
                                  in.op == OpCode::CsQuantile;
        if (needs_scalar && !const_of(in.src[1], scalar_[k][0])) {
          return not_const("cross-sectional scalar operand");
        }
      } else if (in.op == OpCode::Hump) {
        scalar_[k][0] = 0.01;
        if (in.src[1] != kNoSlot && !const_of(in.src[1], scalar_[k][0])) {
          return not_const("hump threshold");
        }
      }
      if (in.dst != kNoSlot && in.op != OpCode::Free && in.op != OpCode::StoreAlpha) {
        const atx::usize width = in.n_out < 1 ? 1 : in.n_out;
        for (atx::usize w = 0; w < width && in.dst + w < producer.size(); ++w) {
          producer[in.dst + w] = k;
        }
      }
    }
    slots_.assign((static_cast<atx::usize>(prog_.num_slots) + 3) * inst_, detail::kVmNaN);
    out_.assign(prog_.roots.size() * inst_, detail::kVmNaN);
    return atx::core::Ok();
  }

  [[nodiscard]] static atx::core::Status not_const(const char *what) {
    return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                          std::string("StreamingEngine: non-Const ") + what);
  }

  // W2 trailing-window op: window from the last operand (a Const, as analyze_ts),
  // then a ring of the last d rows of each input.
  template <class ConstOf>
  [[nodiscard]] atx::core::Status analyze_lit_ts(atx::usize k, const Instr &in,
                                                 const ConstOf &const_of) {
    atx::usize last = 0;
    for (atx::usize s = 0; s < in.src.size(); ++s) {
      if (in.src.at(s) != kNoSlot) {
        last = s;
      }
    }
    atx::f64 wv = 0.0;
    if (!const_of(in.src.at(last), wv)) {
      return not_const("time-series window");
    }
    const atx::usize d = detail::tsv_window_of(std::span<const atx::f64>{&wv, 1});
    if (d == 0) {
      return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                            "StreamingEngine: zero time-series window");
    }
    streaming_detail::LitTsState &st = lit_[k];
    st.d = d;
    st.n_in = detail::lit_ts_inputs(in.op, in.param);
    st.ring.assign(d * st.n_in * inst_, detail::kLitNaN);
    if (lit_win_.size() < st.n_in * d) {
      lit_win_.resize(st.n_in * d);
    }
    return atx::core::Ok();
  }

  template <class ConstOf>
  [[nodiscard]] atx::core::Status analyze_ts(atx::usize k, const Instr &in, const ConstOf &const_of) {
    atx::usize last = 0;
    for (atx::usize s = 0; s < in.src.size(); ++s) {
      if (in.src.at(s) != kNoSlot) {
        last = s;
      }
    }
    atx::f64 wv = 0.0;
    if (!const_of(in.src.at(last), wv)) {
      return not_const("time-series window");
    }
    const atx::usize d = detail::tsv_window_of(std::span<const atx::f64>{&wv, 1});
    if (d == 0) {
      return atx::core::Err(atx::core::ErrorCode::NotImplemented,
                            "StreamingEngine: zero time-series window");
    }
    TsState &st = ts_[k];
    st.kind = streaming_detail::classify(in.op, mode_);
    st.d = d;
    st.cap = d + 1;
    st.pair = in.op == OpCode::TsCorr || in.op == OpCode::TsCov || in.op == OpCode::TsRegression;
    st.ou = in.op == OpCode::OuTheta || in.op == OpCode::OuHalflife || in.op == OpCode::OuMean ||
            in.op == OpCode::OuZscore;
    st.ring_x.assign(st.cap * inst_, detail::kVmNaN);
    if (st.pair) {
      st.ring_y.assign(st.cap * inst_, detail::kVmNaN);
    }
    switch (st.kind) {
    case TsKind::RunSum:
      st.runsum.assign(inst_, detail::TsvRunSum{});
      break;
    case TsKind::Extreme:
      st.miss.assign(inst_, 0);
      st.dq_lo.assign(st.cap * inst_, 0);
      st.dq_hi.assign(st.cap * inst_, 0);
      st.lo_head.assign(inst_, 0);
      st.lo_size.assign(inst_, 0);
      st.hi_head.assign(inst_, 0);
      st.hi_size.assign(inst_, 0);
      break;
    case TsKind::Welford:
      st.welford.assign(inst_, detail::TsvWelfordState{});
      break;
    case TsKind::Decay:
      st.decay.assign(inst_, sliding::LinDecayLane{});
      break;
    case TsKind::TimeReg:
      st.timereg.assign(inst_, sliding::TimeRegLane{});
      break;
    case TsKind::CoMoment:
      st.comoment.assign(inst_, sliding::CoMomentLane{});
      break;
    case TsKind::ExpDecay:
      st.exp_coeff.prepare(d, in.imm[0]);
      st.exp_decay.assign(inst_, sliding::ExpDecayLane{});
      break;
    case TsKind::Lookback:
    case TsKind::Generic:
      break;
    }
    if (st.kind == TsKind::Generic && gather_.size() < st.cap) {
      gather_.resize(st.cap);
      gather_b_.resize(st.cap);
      scratch_a_.resize(st.cap);
      scratch_b_.resize(st.cap);
    }
    return atx::core::Ok();
  }

  // ---- per-date execution ----------------------------------------------------
  void exec(atx::usize k, const CrossSection &today) {
    const Instr &in = prog_.code[k];
    switch (in.op) {
    case OpCode::Free:
      return;
    case OpCode::StoreAlpha: {
      const std::span<const atx::f64> s = src(in, 0);
      std::copy(s.begin(), s.end(), out_.begin() + static_cast<std::ptrdiff_t>(in.param * inst_));
      return;
    }
    case OpCode::Pin: {
      const std::span<const atx::f64> s = row(in.src[0] + in.param);
      const std::span<atx::f64> o = row(in.dst);
      std::copy(s.begin(), s.end(), o.begin());
      return;
    }
    case OpCode::LoadField:
      exec_load(in, today);
      return;
    case OpCode::Const: {
      const std::span<atx::f64> o = row(in.dst);
      std::fill(o.begin(), o.end(), in.imm[0]);
      return;
    }
    default:
      break;
    }
    if (detail::is_lit_op(in.op)) {
      exec_lit(k, in);
    } else if (streaming_detail::is_ts_op(in.op)) {
      exec_ts(k, in);
    } else if (streaming_detail::is_cs_op(in.op)) {
      exec_cs(k, in);
    } else if (in.op == OpCode::TradeWhen || in.op == OpCode::Hump ||
               in.op == OpCode::KalmanLevel || in.op == OpCode::OuFilter ||
               in.op == OpCode::KalmanReg) {
      exec_rec(k, in);
    } else {
      exec_map(in);
    }
  }

  // ---- platform-v7 W2 literature ops (lit_ops.hpp, the VM's kernels) --------
  void exec_lit(atx::usize k, const Instr &in) {
    const std::span<atx::f64> o = row(in.dst);
    switch (in.op) {
    case OpCode::ArgPack:
      for (atx::u32 c = 0; c < in.n_out; ++c) {
        const std::span<const atx::f64> s = row(in.src[c]);
        const std::span<atx::f64> dst = row(in.dst + c);
        std::copy(s.begin(), s.end(), dst.begin());
      }
      return;
    case OpCode::GroupCross: {
      const std::span<const atx::f64> g1 = src(in, 0);
      const std::span<const atx::f64> g2 = src(in, 1);
      for (atx::usize j = 0; j < inst_; ++j) {
        o[j] = detail::lit_group_cross(g1[j], g2[j]);
      }
      return;
    }
    case OpCode::CsBucket:
    case OpCode::CsResidOn:
      exec_lit_cs(in, o);
      return;
    default:
      exec_lit_ts(k, in, o);
      return;
    }
  }

  // bucket / cs_resid_on for today's row (valid set = non-NaN x, as exec_cs).
  void exec_lit_cs(const Instr &in, std::span<atx::f64> o) {
    const std::span<const atx::f64> x = src(in, 0);
    cs_valid_.clear();
    for (atx::usize i = 0; i < inst_; ++i) {
      o[i] = detail::kLitNaN;
      if (!std::isnan(x[i])) {
        cs_valid_.push_back(i);
      }
    }
    if (in.op == OpCode::CsBucket) {
      // SAFETY: analyze_lit_call proved n an integer in [2, 65535].
      detail::lit_bucket_row(x, cs_valid_, static_cast<int>(in.imm[0]), o, cs_scratch_);
      return;
    }
    std::array<std::span<const atx::f64>, detail::kLitMaxReg> cov{};
    atx::usize kc = 0;
    for (atx::u32 c = 0; c < detail::lit_reg_wb(in.param) && kc < cov.size(); ++c) {
      cov[kc++] = row(in.src[1] + c);
    }
    for (atx::u32 c = 0; c < detail::lit_reg_wc(in.param) && kc < cov.size(); ++c) {
      cov[kc++] = row(in.src[2] + c);
    }
    detail::lit_cs_resid_row(x, std::span<const std::span<const atx::f64>>{cov.data(), kc},
                             cs_valid_, o, lit_scratch_);
  }

  // Input c of a W2 Ts op: operand 0; ts_corr_mp's operand 1; else regressor
  // c - 1 of operand 1's block.
  [[nodiscard]] std::span<const atx::f64> lit_input(const Instr &in, atx::usize c) noexcept {
    if (c == 0) {
      return row(in.src[0]);
    }
    if (in.op == OpCode::TsCorrMp) {
      return row(in.src[1]);
    }
    return row(in.src[1] + static_cast<SlotId>(c - 1));
  }

  // Append today's input rows to the ring, then evaluate every instrument on the
  // window of the last min(t+1, d) dates — the window eval_lit_ts gathers.
  void exec_lit_ts(atx::usize k, const Instr &in, std::span<atx::f64> o) {
    streaming_detail::LitTsState &st = lit_[k];
    const atx::usize stride = st.n_in * inst_;
    const atx::usize base = static_cast<atx::usize>(t_ % st.d) * stride;
    for (atx::usize c = 0; c < st.n_in; ++c) {
      const std::span<const atx::f64> s = lit_input(in, c);
      std::copy(s.begin(), s.end(),
                st.ring.begin() + static_cast<std::ptrdiff_t>(base + c * inst_));
    }
    const atx::usize len = static_cast<atx::usize>(std::min<atx::u64>(t_ + 1, st.d));
    const atx::u64 first = t_ + 1 - len;
    const bool partial_ok = detail::is_lit_mp_op(in.op);
    for (atx::usize j = 0; j < inst_; ++j) {
      if (len < st.d && !partial_ok) {
        o[j] = detail::kLitNaN;
        continue;
      }
      for (atx::usize c = 0; c < st.n_in; ++c) {
        for (atx::usize i = 0; i < len; ++i) {
          const atx::usize r = static_cast<atx::usize>((first + i) % st.d);
          lit_win_[c * len + i] = st.ring[r * stride + c * inst_ + j];
        }
      }
      o[j] = detail::lit_ts_cell(in.op,
                                 std::span<const atx::f64>{lit_win_.data(), st.n_in * len},
                                 st.n_in, len, st.d, in.imm[0], lit_scratch_);
    }
  }

  void exec_load(const Instr &in, const CrossSection &today) {
    const std::span<const atx::f64> f = today.fields[in.param];
    const std::span<atx::f64> o = row(in.dst);
    for (atx::usize j = 0; j < inst_; ++j) {
      const bool in_uni = today.universe.empty() || today.universe[j] != 0;
      o[j] = in_uni ? f[j] : detail::kVmNaN;
    }
  }

  // Element-wise / logical / select / split2 — the VM's scalar kernels verbatim.
  void exec_map(const Instr &in) {
    const std::span<atx::f64> o = row(in.dst);
    const auto un = [&](auto f) {
      const std::span<const atx::f64> a = src(in, 0);
      for (atx::usize j = 0; j < inst_; ++j) {
        o[j] = f(a[j]);
      }
    };
    const auto bin = [&](auto f) {
      const std::span<const atx::f64> a = src(in, 0);
      const std::span<const atx::f64> b = src(in, 1);
      for (atx::usize j = 0; j < inst_; ++j) {
        o[j] = f(a[j], b[j]);
      }
    };
    const auto cmp = [&](auto f) {
      bin([&f](atx::f64 x, atx::f64 y) {
        return (detail::vm_is_nan(x) || detail::vm_is_nan(y)) ? detail::kVmNaN
                                                              : (f(x, y) ? 1.0 : 0.0);
      });
    };
    switch (in.op) {
    case OpCode::Add:
      bin([](atx::f64 x, atx::f64 y) { return x + y; });
      break;
    case OpCode::Sub:
      bin([](atx::f64 x, atx::f64 y) { return x - y; });
      break;
    case OpCode::Mul:
      bin([](atx::f64 x, atx::f64 y) { return x * y; });
      break;
    case OpCode::Div:
      bin([](atx::f64 x, atx::f64 y) { return x / y; });
      break;
    case OpCode::Pow:
      bin([](atx::f64 x, atx::f64 y) { return std::pow(x, y); });
      break;
    case OpCode::Spow:
      bin(detail::vm_spow);
      break;
    case OpCode::MinP:
      bin(detail::vm_min);
      break;
    case OpCode::MaxP:
      bin(detail::vm_max);
      break;
    case OpCode::Neg:
      un([](atx::f64 x) { return -x; });
      break;
    case OpCode::Abs:
      un([](atx::f64 x) { return std::fabs(x); });
      break;
    case OpCode::Sign:
      un(detail::vm_sign);
      break;
    case OpCode::Log:
      un([](atx::f64 x) { return std::log(x); });
      break;
    case OpCode::Sigmoid:
      un([](atx::f64 x) { return 1.0 / (1.0 + std::exp(-x)); });
      break;
    case OpCode::Tanh:
      un([](atx::f64 x) { return std::tanh(x); });
      break;
    case OpCode::CmpLt:
      cmp([](atx::f64 x, atx::f64 y) { return x < y; });
      break;
    case OpCode::CmpGt:
      cmp([](atx::f64 x, atx::f64 y) { return x > y; });
      break;
    case OpCode::CmpLe:
      cmp([](atx::f64 x, atx::f64 y) { return x <= y; });
      break;
    case OpCode::CmpGe:
      cmp([](atx::f64 x, atx::f64 y) { return x >= y; });
      break;
    case OpCode::CmpEq:
      cmp([](atx::f64 x, atx::f64 y) { return x == y; });
      break;
    case OpCode::CmpNe:
      cmp([](atx::f64 x, atx::f64 y) { return x != y; });
      break;
    case OpCode::And:
      bin(detail::vm_and);
      break;
    case OpCode::Or:
      bin(detail::vm_or);
      break;
    case OpCode::Not:
      un(detail::vm_not);
      break;
    case OpCode::Select: {
      const std::span<const atx::f64> c = src(in, 0);
      const std::span<const atx::f64> a = src(in, 1);
      const std::span<const atx::f64> b = src(in, 2);
      for (atx::usize j = 0; j < inst_; ++j) {
        o[j] = detail::vm_select(c[j], a[j], b[j]);
      }
      break;
    }
    case OpCode::Split2: {
      const std::span<const atx::f64> x = src(in, 0);
      const std::span<atx::f64> lo = row(in.dst + 1);
      for (atx::usize j = 0; j < inst_; ++j) {
        o[j] = x[j];
        lo[j] = -x[j];
      }
      break;
    }
    default:
      ATX_UNREACHABLE(); // every other op is routed by exec()
    }
  }

  // Cross-sectional — vm.hpp cs_one_date restated (same valid scan, same kernels).
  void exec_cs(atx::usize k, const Instr &in) {
    const std::span<const atx::f64> x = src(in, 0);
    const std::span<atx::f64> out = row(in.dst);
    const bool grouped = streaming_detail::is_grouped_cs(in.op);
    const std::span<const atx::f64> g = grouped ? src(in, 1) : std::span<const atx::f64>{};
    const std::span<const atx::f64> z = (in.op == OpCode::CsResidualize && in.src[2] != kNoSlot)
                                            ? src(in, 2)
                                            : std::span<const atx::f64>{};
    const atx::f64 scale_a = scalar_[k][0];
    cs_valid_.clear();
    for (atx::usize i = 0; i < inst_; ++i) {
      out[i] = detail::kVmNaN;
      if (!detail::cs_is_nan(x[i])) {
        cs_valid_.push_back(i);
      }
    }
    switch (in.op) {
    case OpCode::CsRank:
      detail::cs_rank_row(x, cs_valid_, out, cs_scratch_);
      break;
    case OpCode::CsZscore:
      detail::cs_zscore_row(x, cs_valid_, out);
      break;
    case OpCode::CsScale:
      detail::cs_scale_row(x, cs_valid_, scale_a, out);
      break;
    case OpCode::CsNormalize:
      detail::cs_normalize_row(x, cs_valid_, out);
      break;
    case OpCode::CsWinsorize:
      detail::cs_winsorize_row(x, cs_valid_, scale_a, out);
      break;
    case OpCode::CsDemeanG:
    case OpCode::CsNeutG:
      detail::cs_group_demean_row(x, g, cs_valid_, out, cs_scratch_);
      break;
    case OpCode::CsResidualize:
      detail::cs_residualize_row(x, g, z, cs_valid_, out, cs_scratch_);
      break;
    case OpCode::CsQuantile:
      detail::cs_quantile_row(x, cs_valid_, scale_a, out, cs_scratch_);
      break;
    case OpCode::CsVecSum:
      detail::cs_vec_reduce_row(x, cs_valid_, out, /*want_avg=*/false);
      break;
    case OpCode::CsVecAvg:
      detail::cs_vec_reduce_row(x, cs_valid_, out, /*want_avg=*/true);
      break;
    case OpCode::CsRankG:
      detail::cs_group_row(x, g, cs_valid_, out, /*zscore=*/false, cs_scratch_);
      break;
    case OpCode::CsZscoreG:
      detail::cs_group_row(x, g, cs_valid_, out, /*zscore=*/true, cs_scratch_);
      break;
    case OpCode::CsCountG:
      detail::cs_group_count_mean_row(x, g, cs_valid_, out, /*want_mean=*/false, cs_scratch_);
      break;
    case OpCode::CsMeanG:
      detail::cs_group_count_mean_row(x, g, cs_valid_, out, /*want_mean=*/true, cs_scratch_);
      break;
    case OpCode::CsScaleG:
      detail::cs_group_scale_row(x, g, cs_valid_, out, cs_scratch_);
      break;
    default:
      ATX_UNREACHABLE();
    }
  }

  // Recurrences — the VM forward scan's per-instrument state, one date at a time.
  void exec_rec(atx::usize k, const Instr &in) {
    RecState &rs = rec_[k];
    const bool first = t_ == 0;
    const std::span<atx::f64> o = row(in.dst);
    if (rs.seeded.size() != inst_) {
      rs.prior.assign(inst_, 0.0);
      rs.hump.assign(inst_, detail::HumpState{});
      rs.kl.assign(inst_, detail::KalmanLevelState{});
      rs.xhat.assign(inst_, 0.0);
      rs.kr.assign(inst_, detail::KalmanRegState{});
      rs.seeded.assign(inst_, 0);
    }
    for (atx::usize j = 0; j < inst_; ++j) {
      bool seeded = rs.seeded[j] != 0;
      switch (in.op) {
      case OpCode::TradeWhen:
        o[j] = detail::trade_when_step(rs.prior[j], src(in, 0)[j], src(in, 2)[j], src(in, 1)[j],
                                       first);
        rs.prior[j] = o[j];
        break;
      case OpCode::Hump:
        o[j] = detail::hump_step(rs.hump[j], src(in, 0)[j], scalar_[k][0]);
        break;
      case OpCode::KalmanLevel:
        o[j] = detail::kalman_level_step(rs.kl[j], seeded, src(in, 0)[j], in.imm[0], in.imm[1]);
        break;
      case OpCode::OuFilter:
        o[j] = detail::ou_filter_step(rs.xhat[j], seeded, src(in, 0)[j], in.imm[0], in.imm[1]);
        break;
      case OpCode::KalmanReg: {
        const detail::KalmanRegOut r = detail::kalman_reg_step(rs.kr[j], seeded, src(in, 0)[j],
                                                               src(in, 1)[j], in.imm[0], in.imm[1]);
        o[j] = r.alpha;
        row(in.dst + 1)[j] = r.beta;
        row(in.dst + 2)[j] = r.resid;
        break;
      }
      default:
        ATX_UNREACHABLE();
      }
      rs.seeded[j] = seeded ? 1 : 0;
    }
  }

  // ---- time series -----------------------------------------------------------
  void exec_ts(atx::usize k, const Instr &in) {
    TsState &st = ts_[k];
    const atx::usize slot = (t_ % st.cap) * inst_;
    const std::span<const atx::f64> x = src(in, 0);
    std::copy(x.begin(), x.end(), st.ring_x.begin() + static_cast<std::ptrdiff_t>(slot));
    if (st.pair) {
      const std::span<const atx::f64> y = src(in, 1);
      std::copy(y.begin(), y.end(), st.ring_y.begin() + static_cast<std::ptrdiff_t>(slot));
    }
    const std::span<atx::f64> o = row(in.dst);
    switch (st.kind) {
    case TsKind::Lookback:
      ts_lookback(in, st, o);
      break;
    case TsKind::RunSum:
      ts_runsum(in, st, o);
      break;
    case TsKind::Extreme:
      ts_extreme(in, st, o);
      break;
    case TsKind::Welford:
      ts_welford(in, st, o);
      break;
    case TsKind::Decay:
    case TsKind::TimeReg:
      ts_sliding(in, st, o);
      break;
    case TsKind::CoMoment:
      ts_comoment(in, st, o);
      break;
    case TsKind::ExpDecay:
      ts_exp_decay(st, o);
      break;
    case TsKind::Generic:
      ts_generic(in, st, o);
      break;
    }
  }

  // Ring value of instrument j at absolute date `date` (must be within t-d..t).
  [[nodiscard]] static atx::f64 at(const std::vector<atx::f64> &ring, const TsState &st,
                                   atx::u64 date, atx::usize j, atx::usize inst) noexcept {
    return ring[static_cast<atx::usize>(date % st.cap) * inst + j];
  }

  void ts_lookback(const Instr &in, const TsState &st, std::span<atx::f64> o) const {
    for (atx::usize j = 0; j < inst_; ++j) {
      if (t_ < st.d) {
        o[j] = detail::kTsNaN;
        continue;
      }
      const atx::f64 shifted = at(st.ring_x, st, t_ - st.d, j, inst_);
      o[j] = in.op == OpCode::TsDelay ? shifted : at(st.ring_x, st, t_, j, inst_) - shifted;
    }
  }

  // ts_online_sum_family (compensated, ResearchFast), one date: the SAME
  // TsvRunSum enter/leave the VM sweep performs, gated on warm-up / missing —
  // identical operation order (W0-A0 A-13).
  void ts_runsum(const Instr &in, TsState &st, std::span<atx::f64> o) const {
    const atx::f64 nf = static_cast<atx::f64>(st.d);
    for (atx::usize j = 0; j < inst_; ++j) {
      detail::TsvRunSum &rs = st.runsum[j];
      rs.enter(at(st.ring_x, st, t_, j, inst_));
      if (t_ >= st.d) {
        rs.leave(at(st.ring_x, st, t_ - st.d, j, inst_));
      }
      if (t_ + 1 < st.d || rs.nan_cnt != 0) {
        o[j] = detail::kTsNaN;
      } else {
        o[j] = in.op == OpCode::TsSum ? rs.sum() : rs.sum() / nf;
      }
    }
  }

  // ts_online_extreme, one date: the same monotonic-deque pops/pushes (deque
  // entries are absolute dates kept in a per-instrument ring of `cap`).
  void ts_extreme(const Instr &in, TsState &st, std::span<atx::f64> o) const {
    const bool need_lo = in.op == OpCode::TsMin || in.op == OpCode::TsScale;
    const bool need_hi = in.op == OpCode::TsMax || in.op == OpCode::TsScale;
    const atx::u64 lo_bound = (t_ + 1 >= st.d) ? (t_ + 1 - st.d) : 0;
    for (atx::usize j = 0; j < inst_; ++j) {
      const atx::f64 enter = at(st.ring_x, st, t_, j, inst_);
      if (detail::ts_is_nan(enter)) {
        ++st.miss[j];
      }
      if (t_ >= st.d && detail::ts_is_nan(at(st.ring_x, st, t_ - st.d, j, inst_))) {
        --st.miss[j];
      }
      atx::u64 *lo = st.dq_lo.data() + j * st.cap;
      atx::u64 *hi = st.dq_hi.data() + j * st.cap;
      if (need_lo) {
        deque_step(lo, st.lo_head[j], st.lo_size[j], st, j, enter, lo_bound, /*keep_min=*/true);
      }
      if (need_hi) {
        deque_step(hi, st.hi_head[j], st.hi_size[j], st, j, enter, lo_bound, /*keep_min=*/false);
      }
      if (t_ + 1 < st.d || st.miss[j] != 0) {
        o[j] = detail::kTsNaN;
        continue;
      }
      const atx::f64 lv = need_lo ? at(st.ring_x, st, lo[st.lo_head[j]], j, inst_) : detail::kTsNaN;
      const atx::f64 hv = need_hi ? at(st.ring_x, st, hi[st.hi_head[j]], j, inst_) : detail::kTsNaN;
      if (in.op == OpCode::TsMin) {
        o[j] = lv;
      } else if (in.op == OpCode::TsMax) {
        o[j] = hv;
      } else {
        const atx::f64 range = hv - lv;
        o[j] = range == 0.0 ? 0.0 : (enter - lv) / range;
      }
    }
  }

  void deque_step(atx::u64 *dq, atx::usize &head, atx::usize &size, const TsState &st,
                  atx::usize j, atx::f64 enter, atx::u64 lo_bound, bool keep_min) const {
    while (size > 0 && dq[head] < lo_bound) {
      head = (head + 1) % st.cap;
      --size;
    }
    if (detail::ts_is_nan(enter)) {
      return;
    }
    while (size > 0) {
      const atx::f64 back = at(st.ring_x, st, dq[(head + size - 1) % st.cap], j, inst_);
      if (keep_min ? !(back >= enter) : !(back <= enter)) {
        break;
      }
      --size;
    }
    dq[(head + size) % st.cap] = t_;
    ++size;
  }

  void ts_welford(const Instr &in, TsState &st, std::span<atx::f64> o) const {
    const detail::TsvVarOut which = streaming_detail::welford_out(in.op);
    for (atx::usize j = 0; j < inst_; ++j) {
      detail::TsvWelfordState &w = st.welford[j];
      const atx::f64 enter = at(st.ring_x, st, t_, j, inst_);
      w.enter(enter);
      if (t_ >= st.d) {
        w.leave(at(st.ring_x, st, t_ - st.d, j, inst_));
      }
      o[j] = w.emit(which, enter, t_ + 1 >= st.d);
    }
  }

  void ts_sliding(const Instr &in, TsState &st, std::span<atx::f64> o) const {
    const bool has_leave = t_ >= st.d;
    const bool full = t_ + 1 >= st.d;
    for (atx::usize j = 0; j < inst_; ++j) {
      const auto win = [this, &st, j](atx::usize i) noexcept {
        return at(st.ring_x, st, t_ + 1 - st.d + i, j, inst_);
      };
      const atx::f64 enter = at(st.ring_x, st, t_, j, inst_);
      const atx::f64 leave = has_leave ? at(st.ring_x, st, t_ - st.d, j, inst_) : 0.0;
      o[j] = st.kind == TsKind::Decay
                 ? st.decay[j].step(enter, has_leave, leave, full, st.d, win)
                 : st.timereg[j].step(in.op, enter, has_leave, leave, full, st.d, win);
    }
  }

  void ts_exp_decay(TsState &st, std::span<atx::f64> o) const {
    const bool has_leave = t_ >= st.d;
    const bool full = t_ + 1 >= st.d;
    for (atx::usize j = 0; j < inst_; ++j) {
      const auto win = [this, &st, j](atx::usize i) noexcept {
        return at(st.ring_x, st, t_ + 1 - st.d + i, j, inst_);
      };
      const atx::f64 enter = at(st.ring_x, st, t_, j, inst_);
      const atx::f64 leave = has_leave ? at(st.ring_x, st, t_ - st.d, j, inst_) : 0.0;
      o[j] = st.exp_decay[j].step(enter, has_leave, leave, full, st.exp_coeff, win);
    }
  }

  void ts_comoment(const Instr &in, TsState &st, std::span<atx::f64> o) const {
    const bool has_leave = t_ >= st.d;
    const bool full = t_ + 1 >= st.d;
    for (atx::usize j = 0; j < inst_; ++j) {
      const auto win = [this, &st, j](atx::usize i) noexcept {
        const atx::u64 date = t_ + 1 - st.d + i;
        return std::pair<atx::f64, atx::f64>{at(st.ring_x, st, date, j, inst_),
                                            at(st.ring_y, st, date, j, inst_)};
      };
      const atx::f64 xe = at(st.ring_x, st, t_, j, inst_);
      const atx::f64 ye = at(st.ring_y, st, t_, j, inst_);
      const atx::f64 xl = has_leave ? at(st.ring_x, st, t_ - st.d, j, inst_) : 0.0;
      const atx::f64 yl = has_leave ? at(st.ring_y, st, t_ - st.d, j, inst_) : 0.0;
      o[j] = st.comoment[j].step(in.op, xe, ye, has_leave, xl, yl, full, st.d, win, true);
    }
  }

  // Batch per-cell kernel over the ring window (the last L = min(t+1, d) dates,
  // oldest first) evaluated at its newest cell — the same cells, the same order
  // the batch column kernel reads.
  void ts_generic(const Instr &in, const TsState &st, std::span<atx::f64> o) {
    const atx::usize len = static_cast<atx::usize>(std::min<atx::u64>(t_ + 1, st.d));
    const atx::u64 first = t_ + 1 - len;
    for (atx::usize j = 0; j < inst_; ++j) {
      for (atx::usize i = 0; i < len; ++i) {
        gather_[i] = at(st.ring_x, st, first + i, j, inst_);
      }
      const std::span<const atx::f64> wx{gather_.data(), len};
      if (st.pair) {
        for (atx::usize i = 0; i < len; ++i) {
          gather_b_[i] = at(st.ring_y, st, first + i, j, inst_);
        }
        o[j] = detail::ts_pair_at(in.op, wx, std::span<const atx::f64>{gather_b_.data(), len},
                                  len - 1, 0, st.d, 1, scratch_a_, scratch_b_);
      } else if (st.ou) {
        o[j] = detail::ou_value_at(in.op, wx, len - 1, 0, st.d, 1, scratch_a_);
      } else {
        o[j] = detail::ts_value_at(in.op, wx, len - 1, 0, st.d, 1, scratch_a_, in.imm[0]);
      }
    }
  }

  Program prog_;
  atx::usize inst_{0};
  EvalMode mode_{EvalMode::AuditExact};
  atx::u64 t_{0}; // dates stepped so far (== batch date index of the next step)
  std::vector<atx::f64> slots_;
  std::vector<atx::f64> out_;
  std::vector<std::array<atx::f64, 3>> scalar_; // Const-resolved scalar operands
  std::vector<TsState> ts_;
  std::vector<RecState> rec_;
  std::vector<atx::f64> gather_;
  std::vector<atx::f64> gather_b_;
  std::vector<atx::f64> scratch_a_;
  std::vector<atx::f64> scratch_b_;
  std::vector<atx::usize> cs_valid_;
  detail::CsScratch cs_scratch_;
  std::vector<streaming_detail::LitTsState> lit_; // W2 Ts rings (per instruction)
  std::vector<atx::f64> lit_win_;                 // W2 gathered windows (n_in * d)
  detail::LitScratch lit_scratch_;                // W2 sort / OLS scratch
};

} // namespace atx::engine::alpha

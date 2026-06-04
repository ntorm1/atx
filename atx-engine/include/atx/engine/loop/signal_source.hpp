#pragma once

// atx::engine::loop — the ISignalSource seam (P2-3).
//
// ===========================================================================
//  Why this is a SEAM, not a hardwired strategy
// ===========================================================================
//  This header defines THE one interface between the backtest loop and "the
//  strategy" (plan §3.1). The loop never knows what a signal is or how it is
//  computed; it only knows it can hand a point-in-time PanelView to an
//  ISignalSource and get back one score per live-universe instrument. That
//  inversion is what lets the SAME loop run a test double today and the Phase-3
//  alpha VM tomorrow with no loop change: ScriptedSignalSource (a baked replay,
//  for deterministic loop tests) and VmSignalSource (the production adapter that
//  runs a compiled alpha::Program over the panel) are interchangeable behind
//  this base. The design is VM-centric: the "real" strategy is a compiled alpha
//  program, and the loop treats it as just another ISignalSource (Phase-4's
//  mega-alpha combiner will plug in the same way).
//
// ===========================================================================
//  The evaluate() contract — PURE / deterministic
// ===========================================================================
//  evaluate(PanelView) is PURE in the panel: the same panel contents must
//  produce the same SignalView. It has no hidden time-dependence beyond the
//  panel it is given (a VmSignalSource reads ONLY the panel; a ScriptedSignal-
//  Source reads ONLY its baked schedule — see its note). Determinism is what
//  makes a backtest byte-reproducible (Phase-2 exit criterion) and is why the
//  signature takes the panel by value and returns a value, with no I/O or
//  global state in the contract.
//
//  The result is one f64 per instrument, INDEX-ALIGNED to the universe order
//  shared with the panel and the weight policy (values[i] is the score for
//  universe()[i]). NaN means "no opinion" — the weight policy treats a NaN as
//  "not ranked / no target", distinct from a 0.0 score. The signal length MUST
//  equal the universe size.
//
// ===========================================================================
//  SignalView ownership / borrow lifetime  (downstream contract — P2-4/P2-7)
// ===========================================================================
//  SignalView is NON-OWNING, exactly like PanelView. Its `values` span borrows
//  the source's INTERNAL buffer and is valid only until the next evaluate() on
//  the same source (the next call overwrites that buffer). Treat it like an
//  iterator: consume it within the decision step (P2-4's
//  WeightPolicy::to_target_weights reads it immediately), never store it across
//  another evaluate() or past the source's destruction. Returning a view (not a
//  vector) keeps evaluate() allocation-free on the hot path: the VM writes its
//  alpha column into its own pooled slot and hands back a span over it.

#include <span>   // std::span — the non-owning signal values view
#include <vector> // std::vector — ScriptedSignalSource's owned schedule storage

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp" // ATX_ASSERT (construction precondition)
#include "atx/core/types.hpp" // usize, f64

#include "atx/engine/loop/panel_types.hpp" // PanelView (the evaluate() input)

namespace atx::engine {

// ===========================================================================
//  SignalView — one f64 score per live-universe instrument.
//
//  values[i] is the score for the instrument at universe index i (the SAME
//  fixed order PanelView::universe() exposes). NaN == "no opinion". NON-OWNING:
//  it borrows the source's internal buffer and is invalidated by the next
//  evaluate() on that source (see the header borrow-lifetime note). Trivially
//  copyable and passed BY VALUE, mirroring PanelView.
// ===========================================================================
struct SignalView {
  std::span<const atx::f64> values; // values[i] <-> universe()[i]; NaN = no opinion
};

// ===========================================================================
//  ISignalSource — the strategy seam the loop calls each rebalance.
//
//  Abstract polymorphic base (mirrors data::IDataHandler discipline): virtual
//  dtor, copy/move deleted to forbid slicing. Implementations own their signal
//  buffer; evaluate() returns a borrowed view into it.
// ===========================================================================
class ISignalSource {
public:
  ISignalSource() = default;
  virtual ~ISignalSource() = default;

  // Polymorphic base: suppress slicing copies/moves (agent §1, Rule of Five).
  ISignalSource(const ISignalSource &) = delete;
  ISignalSource &operator=(const ISignalSource &) = delete;
  ISignalSource(ISignalSource &&) = delete;
  ISignalSource &operator=(ISignalSource &&) = delete;

  /// Produce one score per live-universe instrument from the point-in-time
  /// panel. PURE in `panel` (same panel → same signal). The returned SignalView
  /// borrows internal storage valid only until the next evaluate() on this
  /// source. An expected failure (e.g. an exhausted replay schedule, or a
  /// universe/signal size mismatch) travels back as Err — never an abort/throw.
  [[nodiscard]] virtual atx::core::Result<SignalView> evaluate(PanelView panel) = 0;

  /// The number of trailing rows this source needs in the panel. Sizes the
  /// loop's RollingPanel max_lookback. VmSignalSource forwards the alpha
  /// program's compile-time lookback; ScriptedSignalSource returns its
  /// configured N.
  [[nodiscard]] virtual atx::usize max_lookback() const noexcept = 0;
};

// ===========================================================================
//  ScriptedSignalSource — a deterministic baked-schedule test double.
//
//  Holds a pre-baked schedule of per-rebalance signal vectors and replays them
//  in order, one per evaluate() call, advancing an internal cursor. It is a
//  TEST DOUBLE: evaluate() IGNORES the panel BY DESIGN — the schedule is canned,
//  not computed from market data — so a loop test can assert deterministic
//  weights/fills without standing up an alpha program. (It still satisfies the
//  pure contract trivially: output depends only on construction + call count.)
//
//  Storage: the schedule is COPIED into an owned flat buffer at construction
//  (n_rebalances * universe_size f64), so each returned SignalView borrows this
//  source's own memory and no per-call allocation occurs. Every scheduled vector
//  MUST have exactly `universe_size` entries (asserted at construction). NaN
//  entries pass through verbatim (the "no opinion" sentinel is preserved).
// ===========================================================================
class ScriptedSignalSource final : public ISignalSource {
public:
  /// Build from a schedule of per-rebalance signal vectors. `universe_size` is
  /// the fixed signal length each evaluate() returns; `max_lookback` is the N
  /// reported to the loop (the panel depth a real strategy would need — the test
  /// double carries it so the loop sizes its panel identically to production).
  /// PRECONDITION: every schedule row has exactly `universe_size` entries
  /// (ABORTS in debug — a malformed fixture is a test bug, not a runtime error).
  ///
  // bugprone-exception-escape suppressed: the ctor is noexcept by design (it is
  // a test fixture builder). Its only throwing op is the construction-time
  // flat-buffer reserve/insert (bounded by the schedule size). A std::bad_alloc
  // here is unrecoverable fixture-setup failure, so terminating (the noexcept
  // effect) is the intended fail-closed posture — no half-built source leaks.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ScriptedSignalSource(const std::vector<std::vector<atx::f64>> &schedule, atx::usize universe_size,
                       atx::usize max_lookback) noexcept
      : universe_size_{universe_size}, max_lookback_{max_lookback}, n_rebalances_{schedule.size()} {
    flat_.reserve(universe_size * schedule.size());
    for (const std::vector<atx::f64> &vec : schedule) {
      // A fixture whose row length disagrees with the declared universe size is
      // a programmer error (it would yield a signal of the wrong length), so it
      // fails closed here rather than producing a malformed SignalView later.
      ATX_ASSERT(vec.size() == universe_size);
      flat_.insert(flat_.end(), vec.begin(), vec.end());
    }
  }

  /// Replay the next baked vector as a borrowed SignalView, advancing the
  /// cursor. The `panel` is intentionally unused (canned replay — see class
  /// note). Returns Err(OutOfRange) once the schedule is exhausted.
  [[nodiscard]] atx::core::Result<SignalView> evaluate(PanelView panel) override {
    ATX_UNUSED(panel); // panel-independent by design (test double).
    if (cursor_ >= n_rebalances_) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                            "ScriptedSignalSource: schedule exhausted");
    }
    // SAFETY: the flat buffer holds n_rebalances_ * universe_size_ f64; cursor_ <
    //         n_rebalances_ (checked above), so this sub-span lies wholly inside
    //         the allocation. Slicing an owned contiguous buffer is the intended
    //         access pattern (mirrors PanelView's ring indexing); a per-row
    //         std::vector would force a per-call allocation the seam forbids.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::span<const atx::f64> values{flat_.data() + cursor_ * universe_size_, universe_size_};
    ++cursor_;
    return atx::core::Ok(SignalView{values});
  }

  /// The configured N (the panel depth the loop should provision).
  [[nodiscard]] atx::usize max_lookback() const noexcept override { return max_lookback_; }

private:
  std::vector<atx::f64> flat_; // owned [n_rebalances * universe_size] schedule
  atx::usize universe_size_;   // fixed signal length per rebalance
  atx::usize max_lookback_;    // panel depth reported to the loop
  atx::usize n_rebalances_;    // number of scheduled vectors (== schedule.size())
  atx::usize cursor_{0};       // index of the next vector to replay
};

// ===========================================================================
//  VmSignalSource — production adapter over the Phase-3 alpha VM.
//
//  CONTRACT (staged red until Phase 3 — see below for why it is compile-guarded)
//  --------------------------------------------------------------------------
//  VmSignalSource is the REAL strategy seam: it wraps a compiled alpha::Program
//  and the alpha::Engine that executes it. Its responsibilities, frozen here so
//  the seam contract is recorded before its dependency exists:
//
//    * Holds an alpha::Program (the compiled alpha expression DSL — Phase-3) and
//      an alpha::Engine (the VM that runs it). Both are owned for the source's
//      lifetime; construction compiles/binds the program once.
//    * evaluate(PanelView panel): runs the VM over the panel and returns the
//      alpha column (one score per universe instrument) as a SignalView that
//      borrows the Engine's pooled output slot. NO per-call allocation beyond
//      the VM's own pre-sized pooled slots — the whole reason evaluate() returns
//      a view, not a vector. Pure in `panel`: the program reads only the panel.
//    * max_lookback(): forwards alpha::Program's COMPILE-TIME lookback (the
//      deepest trailing window any operator in the program references), so the
//      loop sizes its RollingPanel exactly to the program's needs.
//
//  WHY COMPILE-GUARDED, NOT STUBBED: alpha::Program / alpha::Engine do not exist
//  yet (Phase 3). A fake/stub implementation that "pretends to work" would be
//  dead code that could silently ship wrong signals. Instead the declaration AND
//  intended implementation live behind ATX_ENGINE_HAS_ALPHA_VM, a macro defined
//  NOWHERE today — so this block never compiles, the green build is unaffected,
//  and the contract above is still recorded as code (not prose). When Phase 3
//  lands, defining the macro (and adding the #include for the alpha headers)
//  turns this into the real adapter; its red→green test is a tracked Deferred
//  residual in the Phase-2 ledger.
// ===========================================================================
#if defined(ATX_ENGINE_HAS_ALPHA_VM)

class VmSignalSource final : public ISignalSource {
public:
  /// Wrap a compiled alpha program and its execution engine. `program` is the
  /// compiled alpha::Program; `engine` runs it over a panel. Both are owned for
  /// this source's lifetime; the program is bound to the engine once here.
  VmSignalSource(alpha::Program program, alpha::Engine engine) noexcept
      : program_{std::move(program)}, engine_{std::move(engine)} {}

  /// Run the VM over `panel` and return the alpha column as a SignalView that
  /// borrows the engine's pooled output slot. No per-call allocation beyond the
  /// VM's pooled slots. Err on a VM execution fault (an expected failure — e.g.
  /// a program/panel shape mismatch — not an abort).
  [[nodiscard]] atx::core::Result<SignalView> evaluate(PanelView panel) override {
    ATX_TRY(const std::span<const atx::f64> col, engine_.run(program_, panel));
    return atx::core::Ok(SignalView{col});
  }

  /// Forward the program's compile-time lookback (deepest trailing window any
  /// operator references), so the loop sizes its RollingPanel to the program.
  [[nodiscard]] atx::usize max_lookback() const noexcept override {
    return program_.max_lookback();
  }

private:
  alpha::Program program_; // compiled alpha expression (Phase-3 DSL)
  alpha::Engine engine_;   // the VM that executes program_ over a panel
};

#endif // ATX_ENGINE_HAS_ALPHA_VM

} // namespace atx::engine

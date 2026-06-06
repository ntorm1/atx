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

#include <cstdint> // std::uint8_t — alpha::Panel universe mask element
#include <limits>  // std::numeric_limits (the "no opinion" quiet-NaN sentinel)
#include <span>    // std::span — the non-owning signal values view
#include <string>  // std::string — alpha::Panel field-name dictionary
#include <utility> // std::move (program hand-off)
#include <vector>  // std::vector — ScriptedSignalSource's owned schedule storage

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp" // ATX_ASSERT (construction precondition)
#include "atx/core/types.hpp" // usize, f64

#include "atx/engine/alpha/bytecode.hpp"   // alpha::Program (compiled alpha + required_lookback)
#include "atx/engine/alpha/panel.hpp"      // alpha::Panel, SignalSet (the VM's data plane)
#include "atx/engine/alpha/vm.hpp"         // alpha::Engine (the vectorized executor)
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
//  ATX_ENGINE_HAS_ALPHA_VM — the green-gate, resolved IN-HEADER (P3c-3).
//
//  The Phase-2 freeze guarded VmSignalSource behind this macro, defined NOWHERE,
//  against an ASSUMED alpha API. As of Phase 3 the alpha VM headers are
//  header-only and ALWAYS present (included above), so we DEFINE the macro right
//  here — no CMakeLists change, no build-system flag — and the adapter below
//  compiles unconditionally. The macro is kept (rather than just dropping the
//  #if) so any downstream `#if defined(ATX_ENGINE_HAS_ALPHA_VM)` written against
//  the frozen contract still sees the seam as "present".
// ===========================================================================
#define ATX_ENGINE_HAS_ALPHA_VM 1

// ===========================================================================
//  VmSignalSource — production adapter over the Phase-3 alpha VM (as-built).
//
//  THE API RECONCILIATION (the frozen contract above was WRONG)
//  --------------------------------------------------------------------------
//  The Phase-2 freeze assumed `Engine::run(program, panel) -> span<f64>` and
//  `Program::max_lookback()`. The AS-BUILT alpha API differs and this adapter is
//  rewritten to it:
//    * alpha::Engine BINDS its alpha::Panel at CONSTRUCTION (const ref) and
//      `evaluate(const Program&) -> Result<SignalSet>` returns the WHOLE
//      date×instrument matrix for EVERY alpha root — not a single column.
//    * the lookback is a FIELD, `Program::required_lookback` (atx::u16), not a
//      method.
//    * the loop hands a `loop::PanelView` (newest-first, column-major per field,
//      ring storage) but the VM consumes an `alpha::Panel` (date-major,
//      CHRONOLOGICAL, flat per-field f64 + a {0,1} universe mask). The adapter
//      must TRANSPOSE: reverse the row order (newest-first -> oldest-first) and
//      reshape into the date-major layout. Chronological order is LOAD-BEARING —
//      every Ts* op reads a causal trailing window [t-d+1, t], so a reversed
//      window silently corrupts every time-series alpha.
//
//  WHAT evaluate(PanelView) DOES
//    1. Build an alpha::Panel from the trailing PanelView: dates = panel.rows(),
//       instruments = panel.instruments(), the five OHLCV fields named so a
//       program's `close`/`open`/... references resolve, with row reversal +
//       reshape and a PIT universe mask (present()==false -> 0, reads back NaN).
//    2. Construct an alpha::Engine over that Panel and evaluate(program_).
//    3. Extract the CURRENT-date cross-section: the NEWEST date is the LAST
//       alpha date (dates-1) of the program's first root; copy it into a
//       source-owned buffer and hand back a SignalView over THAT buffer (never a
//       temporary — the borrow lives until the next evaluate()).
//    * max_lookback() forwards program_.required_lookback.
//
//  ALLOCATION (scoped honestly, NOT claimed zero). The as-built Engine binds its
//  Panel at construction and there is no rebind API, so a fresh alpha::Panel +
//  Engine are built per evaluate() — that allocates (the Panel's owned columns,
//  the Engine's slot pool). We REUSE source-owned scratch (field_data_, the
//  signal_ output buffer) across calls to keep the per-call allocation to the
//  Panel/Engine themselves. This is acceptable per plan §3.5: evaluate() runs at
//  the REBALANCE cadence (per-schedule, not per-bar), so the cold-ish build is a
//  documented residual, not a hot-path regression. The SignalView borrow is
//  zero-alloc steady-state (signal_ is reserved once, refreshed in place).
//
//  PURE in `panel`: the program reads only the panel the source is handed; there
//  is no hidden time state (the VM's only state is recurrence scratch reset per
//  evaluate). Same panel contents -> same SignalView (the ISignalSource contract).
// ===========================================================================

class VmSignalSource final : public ISignalSource {
public:
  /// Wrap a compiled alpha program. `program` is owned for this source's lifetime
  /// (typically ONE alpha = the strategy; root 0 is the traded alpha). The VM
  /// Engine is built per evaluate() over the freshly-transposed panel (the
  /// as-built Engine binds its Panel at construction — see the header note).
  explicit VmSignalSource(alpha::Program program) noexcept : program_{std::move(program)} {}

  /// Transpose the trailing PanelView into a chronological alpha::Panel, run the
  /// VM, and return the current-date cross-section as a SignalView borrowing this
  /// source's own buffer (valid until the next evaluate()). Err on any VM fault /
  /// shape mismatch (an expected failure — never an abort/throw).
  [[nodiscard]] atx::core::Result<SignalView> evaluate(PanelView panel) override {
    const atx::usize dates = panel.rows();
    const atx::usize inst = panel.instruments();

    ATX_TRY(const alpha::Panel ap, build_alpha_panel(panel, dates, inst));
    alpha::Engine engine{ap};
    ATX_TRY(const alpha::SignalSet signals, engine.evaluate(program_));

    // The strategy is root 0 (the traded alpha). A zero-root or zero-date panel
    // yields no opinion (an expected boundary, not a fault).
    if (signals.alphas.empty() || dates == 0) {
      signal_.assign(inst, kNoOpinion);
      return atx::core::Ok(SignalView{std::span<const atx::f64>{signal_}});
    }
    // The CURRENT date is the NEWEST row == the LAST alpha date (chronological).
    const std::span<const atx::f64> cross = signals.alpha_cross_section(0, dates - 1);
    signal_.assign(cross.begin(), cross.end());
    return atx::core::Ok(SignalView{std::span<const atx::f64>{signal_}});
  }

  /// Forward the program's compile-time lookback (deepest trailing window any
  /// operator references), so the loop sizes its RollingPanel to the program.
  [[nodiscard]] atx::usize max_lookback() const noexcept override {
    return static_cast<atx::usize>(program_.required_lookback);
  }

private:
  /// Quiet NaN "no opinion" sentinel (matches the SignalView NaN contract).
  static constexpr atx::f64 kNoOpinion = std::numeric_limits<atx::f64>::quiet_NaN();

  /// The five OHLCV field names, in PanelField storage order. A program that
  /// references any of these resolves; the VM loads only the ones it uses.
  [[nodiscard]] static std::vector<std::string> ohlcv_field_names() {
    return {"open", "high", "low", "close", "volume"};
  }

  /// Build a date-major, CHRONOLOGICAL alpha::Panel from the newest-first
  /// PanelView. Reuses the source-owned field_data_ scratch (refreshed in place),
  /// then hands it to Panel::create (which copies it). Returns the program's
  /// shape error verbatim if the (rare) ragged-input guard ever trips.
  [[nodiscard]] atx::core::Result<alpha::Panel> build_alpha_panel(PanelView panel, atx::usize dates,
                                                                  atx::usize inst) {
    const atx::usize cells = dates * inst;
    field_data_.assign(kPanelFieldCount, std::vector<atx::f64>(cells, kNoOpinion));
    universe_.assign(cells, std::uint8_t{0});

    // SAFETY: transpose + reshape. PanelView row 0 is the NEWEST sealed row; the
    //   alpha::Panel wants date 0 == EARLIEST. So alpha date `d` reads PanelView
    //   row `(dates-1) - d` (the reversal that makes the window chronological —
    //   load-bearing for every Ts* op). The flat alpha index is `d*inst + j`,
    //   bounded by `cells` (== field_data_ column size); `row < dates == rows()`
    //   and `j < inst == instruments()` satisfy the PanelView accessor
    //   preconditions. Absent cells read as NaN and present()==false -> mask 0.
    for (atx::usize d = 0; d < dates; ++d) {
      const atx::usize row = (dates - 1U) - d; // newest-first -> chronological
      for (atx::usize j = 0; j < inst; ++j) {
        const atx::usize idx = d * inst + j;
        field_data_[static_cast<atx::usize>(PanelField::Open)][idx] = panel.open(row, j);
        field_data_[static_cast<atx::usize>(PanelField::High)][idx] = panel.high(row, j);
        field_data_[static_cast<atx::usize>(PanelField::Low)][idx] = panel.low(row, j);
        field_data_[static_cast<atx::usize>(PanelField::Close)][idx] = panel.close(row, j);
        field_data_[static_cast<atx::usize>(PanelField::Volume)][idx] = panel.volume(row, j);
        universe_[idx] = panel.present(row, j) ? std::uint8_t{1} : std::uint8_t{0};
      }
    }
    return alpha::Panel::create(dates, inst, ohlcv_field_names(), field_data_, universe_);
  }

  alpha::Program program_;                        // compiled alpha (root 0 == traded strategy)
  std::vector<std::vector<atx::f64>> field_data_; // [field][dates*inst] transpose scratch (reused)
  std::vector<std::uint8_t> universe_;            // [dates*inst] PIT mask scratch (reused)
  std::vector<atx::f64> signal_; // current-date cross-section the SignalView borrows
};

} // namespace atx::engine

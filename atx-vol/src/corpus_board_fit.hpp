#pragma once

// corpus_board_fit — the ONE per-board fit pipeline shared by `build_corpus`
// (corpus.cpp) and `populate_surface_db` (surface_db_populate.cpp): fit ONE
// board through the blessed atx-vol path (OptionChain::from_frame ->
// PricerFitter::fit -> VolaSession::to_priced_surface). See corpus.cpp's
// original design note (corpus.hpp / corpus.cpp) for why the corpus fits one
// board at a time rather than fanning out across a whole universe.
//
// Private, src/-only header: NOT installed, NOT part of the public
// atx/vol/ API surface. Both TUs that need this logic live inside the
// atx-vol library target, so ordinary internal linkage across translation
// units is sufficient.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "atx/vol/correction.hpp"      // CorrectionCache (C2 cross-date cache export)
#include "atx/vol/corpus.hpp"          // CorpusBoard, CorpusFitStatus, CorpusAdmissionPolicy, ...
#include "atx/vol/priced_surface.hpp"  // PricedSurface
#include "atx/vol/pricer_fitter.hpp"   // PricerConfig
#include "atx/vol/session.hpp"         // SessionInputs
#include "atx/vol/surface_archive.hpp" // SurfaceProvenance
#include "atx/vol/types.hpp"           // ErrorCode
#include "atx/vol/vol_curve.hpp"       // VolCurveKind

namespace atx::vol {

// C2 (perf): the per-side correction caches the fit ACTUALLY built for a board,
// exported so the cross-date warm-start chain can carry them to the next date.
// Populated only when the fit built fresh caches (cold path); left empty when the
// fit reused supplied caches (nothing new to carry) or built none. The two
// optionals OWN the caches (copied out before the fitter leaves scope).
struct WarmCacheExport {
  std::optional<CorrectionCache> call{};
  std::optional<CorrectionCache> put{};
  [[nodiscard]] bool any() const noexcept { return call.has_value() || put.has_value(); }
};

// Per-board fit outcome (worker output slot). Move-only: owns the fitted
// (move-only) `PricedSurface`. Workers write disjoint slots (one per board),
// so aggregation is race-free.
struct FitSlot {
  CorpusFitStatus status{CorpusFitStatus::Skipped};
  VolCurveKind chosen_kind{VolCurveKind::ConvexDense};
  std::uint32_t n_slices{0};
  double oos_in_band{0.0};       // meaningful iff oos_in_band_available
  bool oos_in_band_available{false}; // true iff an OOS candidate score was
                                     // computed (false for a pinned curve —
                                     // it has no held-out selector score)
  ErrorCode error_code{ErrorCode::Unknown};
  // The failing fit Error's MESSAGE, kept beside its code so the two together
  // reconstruct the `Error` the fit actually returned instead of collapsing it to
  // a category. The code alone answers "what class of failure"; the MESSAGE is
  // where the diagnostic lives -- PricerFitter's risk pipeline formats the failing
  // gate, the offending slice, the log-moneyness and the slack into it
  // ("risk surface rejected: model=... mask=... butterfly_slice=... carry=...",
  // pricer_fitter.cpp), and dropping it here is what left an operator with a bare
  // `cells_failed` count and no next step. Empty on success, and on a failure
  // whose Error carried no context.
  //
  // Additive: `error_code` keeps its exact meaning and its existing readers
  // (corpus.cpp's manifest/quality rows) are untouched.
  std::string error_message{};
  CorpusQualityMetrics quality{};
  CorpusAdmissionDecision admission{CorpusDisposition::Admitted, CorpusAdmissionReason::None, 0u};
  std::optional<PricedSurface> surface{};        // present iff status == Ok
  std::optional<SurfaceProvenance> provenance{}; // the fitter's own health for `surface`
};

[[nodiscard]] std::uint32_t saturated_u32(std::size_t value) noexcept;

// Fit ONE board through the blessed path. `admission`, when non-null,
// evaluates + records quality/admission (build_corpus's qualified path);
// pass nullptr to skip quality collection entirely (populate_surface_db's
// use — it never quarantines, only records Ok/Failed/Skipped).
//
// `session_overlay`, when set, is invoked on the fitter's fully-resolved
// `SessionInputs` immediately before the (first) `VolaSession::build` call —
// e.g. `apply_symbol_config` layering a per-symbol `SymbolFitConfig` onto the
// EXACT inputs the blessed path fits with (populate_surface_db's per-symbol
// manifest overlay). A caller that also wants the curve-pin to be immune to
// PricerFitter's auto-routed fallback ladder must ALSO set `tmpl.curve`
// (mirroring `CorpusBoard::curve`'s existing pin semantics) — the overlay
// alone does not suppress the ladder. Applied once; a fallback-ladder retry
// does not re-invoke it.
//
// Pure w.r.t. shared state: reads only its own `board` + `tmpl` (const),
// constructs its own chain / fitter. Safe to run concurrently on distinct
// boards.
// `out_caches` (C2), when non-null, receives a COPY of the per-side correction
// caches this fit built — for the cross-date warm-start chain to carry forward.
// Populated only on a successful fit that BUILT caches (cold path); left empty
// when the fit reused supplied caches or built none. Never populated on failure.
[[nodiscard]] FitSlot fit_board(const CorpusBoard &board, const PricerConfig &tmpl,
                                const CorpusAdmissionPolicy *admission,
                                const std::function<void(SessionInputs &)> &session_overlay = {},
                                WarmCacheExport *out_caches = nullptr);

} // namespace atx::vol

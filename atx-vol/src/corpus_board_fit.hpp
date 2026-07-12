#pragma once

// corpus_board_fit — the ONE per-board fit pipeline shared by `build_corpus`
// (corpus.cpp) and `populate_surface_db` (surface_db_populate.cpp): fit ONE
// board through the blessed atx-vol path (OptionChain::from_frame ->
// PricerFitter::fit -> VolaSession::to_priced_surface). See corpus.cpp's
// original design note (corpus.hpp / corpus.cpp) for why this does not go
// through calib_pool.hpp's calibrate_pool.
//
// Private, src/-only header: NOT installed, NOT part of the public
// atx/vol/ API surface. Both TUs that need this logic live inside the
// atx-vol library target, so ordinary internal linkage across translation
// units is sufficient.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "atx/vol/corpus.hpp"         // CorpusBoard, CorpusFitStatus, CorpusAdmissionPolicy, ...
#include "atx/vol/priced_surface.hpp" // PricedSurface
#include "atx/vol/pricer_fitter.hpp"  // PricerConfig
#include "atx/vol/session.hpp"        // SessionInputs
#include "atx/vol/types.hpp"          // ErrorCode
#include "atx/vol/vol_curve.hpp"      // VolCurveKind

namespace atx::vol {

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
  CorpusQualityMetrics quality{};
  CorpusAdmissionDecision admission{CorpusDisposition::Admitted, CorpusAdmissionReason::None, 0u};
  std::optional<PricedSurface> surface{}; // present iff status == Ok
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
[[nodiscard]] FitSlot fit_board(const CorpusBoard &board, const PricerConfig &tmpl,
                                const CorpusAdmissionPolicy *admission,
                                const std::function<void(SessionInputs &)> &session_overlay = {});

} // namespace atx::vol

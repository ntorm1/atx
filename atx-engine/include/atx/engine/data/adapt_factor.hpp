#pragma once

// atx::engine::data — adapt_factor (S6.6): BYO FactorModelArtifact lowering +
// reference-dataset cross-section ingestion.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  Two seams:
//
//  (1) artifact_to_factor_model — lowers a FactorModelArtifact (user-supplied
//      X/F/D matrices) into a risk::FactorModel by forwarding to
//      risk::FactorModel::create. NO independent validation here — create is the
//      single source of truth (SPD of F, positive D, shape checks, fit-window).
//      Byte-identity guarantee: calling create directly with the same matrices
//      produces an identical FactorModel (same internals -> same apply() output).
//
//  (2) reference_spans — materializes a market_cap / group_id cross-section from
//      a reference Dataset (Role::Reference) as-of a given date, aligned onto
//      the price Dataset's instrument axis. These are exactly the spans
//      FactorModelBuilder::build_components(panel, window, market_cap, group_id)
//      consumes. Missing instrument in the reference -> NaN market_cap /
//      default_group group_id (never a hard error).
//
// Cold-path (once per backtest window). Deterministic; no RNG.

#include <cstdint> // std::uint32_t (u32)
#include <vector>

#include "atx/core/error.hpp" // Result, Err
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/engine/data/dataset.hpp"               // Dataset, DateKey, as_of_index
#include "atx/engine/data/factor_model_artifact.hpp" // FactorModelArtifact
#include "atx/engine/risk/fwd.hpp"                   // risk::FactorModel fwd decl

namespace atx::engine::data {

// ---------------------------------------------------------------------------
//  artifact_to_factor_model
// ---------------------------------------------------------------------------
// Lowers `a` into a risk::FactorModel by copying its matrices into
// risk::FactorModel::create. Err is forwarded from create on a shape
// violation, non-SPD F, non-positive D, or invalid fit window.
//
// The copy (not move) from the const artifact is intentional: the artifact
// remains valid for re-use after this call. If the caller wants to avoid the
// copy, they can move out of the artifact fields before calling.
[[nodiscard]] atx::core::Result<atx::engine::risk::FactorModel>
artifact_to_factor_model(const FactorModelArtifact &a);

// ---------------------------------------------------------------------------
//  RefSpans — cross-section vectors for FactorModelBuilder
// ---------------------------------------------------------------------------
// Produced by reference_spans; consumed by FactorModelBuilder::build_components
// as std::span<const f64> market_cap and std::span<const u32> group_id.
//
// Both vectors are sized == price.num_instruments(), in price.instruments() order.
// Missing instruments get NaN market_cap / default_group group_id.
struct RefSpans {
  std::vector<atx::f64> market_cap; // size == price.num_instruments()
  std::vector<atx::u32> group_id;   // size == price.num_instruments()
};

// ---------------------------------------------------------------------------
//  reference_spans
// ---------------------------------------------------------------------------
// Materialize the market_cap / group_id cross-section from `reference` as-of
// `as_of_date`, aligned onto `price`'s instrument axis.
//
// Requirements on `reference`:
//   * Must carry columns named "market_cap" (ColumnDType::F64) and
//     "group_id" (ColumnDType::Category stored as f64).
//     -> Err(NotFound) if either column is absent.
//   * reference.dates() and price.dates() must be strictly ascending
//     (validated by align_onto / as_of_index; we rely on as_of_index here).
//
// Per-instrument resolution (in price.instruments() order):
//   1. Find the instrument in reference.instruments() (linear scan; O(M) cold).
//      If absent: market_cap[i] = NaN, group_id[i] = default_group. Done.
//   2. Resolve as-of row: as_of_index(reference.dates(), as_of_date).
//      If nullopt (as_of_date precedes all reference dates):
//              market_cap[i] = NaN, group_id[i] = default_group. Done.
//   3. Read reference.column_by_name("market_cap")[row * ref_ni + ref_inst_idx].
//      Read reference.column_by_name("group_id")  [row * ref_ni + ref_inst_idx].
//      group_id value is stored as f64/Category -> cast to u32
//      (NaN value -> default_group).
[[nodiscard]] atx::core::Result<RefSpans>
reference_spans(const Dataset &reference, const Dataset &price, DateKey as_of_date,
                atx::u32 default_group = 0U);

} // namespace atx::engine::data

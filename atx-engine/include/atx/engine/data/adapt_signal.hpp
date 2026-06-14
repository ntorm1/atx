#pragma once

// atx::engine::data — adapt_signal (S6.5): external Signal Dataset → gated
// library admission (and, via merge_features_into_panel, the feature path).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  S6.5 lowers an EXTERNAL precomputed-signal Dataset (Role::Signal) into the
//  library through the EXISTING admission gate — it adds NO new admission logic.
//  Two paths fall out:
//
//   (1) signal-as-feature — a Signal Dataset is just feature columns: it merges
//       onto a Panel as new named fields via the existing S6.4b
//       merge_features_into_panel (align_onto + append). NO code here; the
//       SignalAsFeatureReferenceable test pins it. (Field-name collision rule
//       applies: a signal column must not clash with a price-derived field or a
//       prior column — Panel::create does NOT dedupe.)
//
//   (2) signal-as-library-admission — signal_to_candidates (below). It REUSES
//       the existing seams end-to-end:
//         align_onto(price, signal)  -> aligned signal columns over the panel axis
//         (build a SignalSet)        -> one alpha per aligned column, date-major
//         alpha::extract_streams     -> per-column realized pnl + position streams
//         combine::compute_metrics   -> per-column AlphaMetrics (book_size 1.0,
//                                       the gross-normalized-fraction convention
//                                       the mined path uses)
//       and synthesizes one library::AlphaCandidate per column. The CALLER then
//       does lib.admit(candidate, gate) per candidate — gated by the EXISTING
//       deflated-Sharpe + S4 robustness battery.
//
// ===========================================================================
//  Ownership / lifetime (the load-bearing design — mirrors learn/ensemble.hpp)
// ===========================================================================
//  library::AlphaCandidate::pnl / pos_flat are NON-OWNING spans; they must outlive
//  the admit() call. The realized streams live in an alpha::AlphaStreams. So
//  signal_to_candidates returns a SignalAdmission that OWNS the streams and holds
//  the candidates whose spans point INTO that owned AlphaStreams. (A bare
//  vector<AlphaCandidate> return would dangle.) std::vector move preserves its
//  buffer, and AlphaStreams stores its streams in std::vector members whose data()
//  is move-stable, so the candidate spans stay valid across the by-value return.
//
// Cold-path (once per research window); allocation is intentional. Deterministic:
//  canon_hash is a content hash with no RNG / clock input.

#include <vector>

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // usize

#include "atx/engine/alpha/panel.hpp"        // alpha::Panel
#include "atx/engine/alpha/streams.hpp"      // alpha::AlphaStreams
#include "atx/engine/data/dataset.hpp"       // data::Dataset
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/library/library.hpp"    // library::AlphaCandidate
#include "atx/engine/loop/weight_policy.hpp" // WeightPolicy

namespace atx::engine::data {

// ===========================================================================
//  SignalAdmission — the OWNING result of signal_to_candidates.
//
//  `streams` owns the realized pnl_flat / pos_flat buffers; each candidate in
//  `candidates` (one per signal column) carries pnl / pos_flat spans that point
//  INTO `streams`. The candidate spans are valid for as long as this
//  SignalAdmission is alive (and across its move). The caller admits each
//  candidate via library::Library::admit(candidate, gate) BEFORE this value is
//  destroyed.
// ===========================================================================
struct SignalAdmission {
  alpha::AlphaStreams streams;                     // owns pnl_flat / pos_flat
  std::vector<library::AlphaCandidate> candidates; // one per signal column; spans into `streams`
};

// ===========================================================================
//  signal_to_candidates — external Signal Dataset → library candidates (path 2).
//
//  Aligns `signal` onto the canonical `price` axis (align_onto), builds a
//  SignalSet over `panel` (one alpha per aligned column, date-major), realizes the
//  per-column pnl/position streams via alpha::extract_streams(.., policy, panel,
//  sim), and synthesizes one library::AlphaCandidate per column with metrics from
//  combine::compute_metrics (book_size 1.0). Returns the OWNING SignalAdmission.
//
//  Each candidate is flagged externally-sourced:
//    * prov.expr_source = "<external:" + signal.provenance().source + ":" + column ">"
//    * prov.parent_hashes = {}, prov.mutation_op = 0, prov.seed = 0
//    * canon_hash = a deterministic content hash over an EXTERNAL tag (the literal
//      "external" marker + dataset source + column name). Because the tag is hashed
//      verbatim and a mined genome's factory::canonical_hash never hashes that tag,
//      the external hash space is DISJOINT from the genome-hash space by
//      construction (the cross-run dedup-distinctness guarantee).
//    * as_of = as_of; source = nullptr (no live re-eval handle).
//
//  Errors: propagates align_onto's Err (e.g. non-ascending dates) and
//  extract_streams's Err (panel/SignalSet shape mismatch, or no "close" field).
// ===========================================================================
[[nodiscard]] atx::core::Result<SignalAdmission>
signal_to_candidates(const Dataset &signal, const Dataset &price, const alpha::Panel &panel,
                     const exec::ExecutionSimulator &sim, const WeightPolicy &policy,
                     atx::usize as_of);

} // namespace atx::engine::data

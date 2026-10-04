#pragma once

// The `factors` verb of atx-equity-strategy-targets (platform P9 B1, contract K-P9-4): every
// candidate's unsigned factor series as fit_composition_weights.py factor_record defines it
// (FACTOR_SEMANTICS, fit:1130-1170), computed on the C++ price-risk basis of the `exposures` verb
// (contract K2, export_decision) instead of the fitter's own Context.
//
//   factors --role ROLE/manifest.json --role-sha256 SHA --signals DIR --output NEWDIR
//           [--max-bytes N]
//
// DIR/signals.json (atx.factor-signals/v1) names the candidates in roster order:
//   {"schema": "atx.factor-signals/v1", "role_manifest_sha256": SHA,
//    "candidates": [{"id", "payload", "payload_sha256"}, ...]}
// Each payload is a date-major dates x instruments little-endian f64 signal of the pinned role
// (the IC runner's candidate cache payload, read in place; a relative path resolves against DIR),
// verified by extent and SHA-256. {id, payload, payload_sha256} is the shape of the runner
// summary's candidate_cache.entries[] and of the fitter's resolved cache entry, so a caller lists
// the entries it already resolved (other keys of an entry are ignored).
// The role goes through the research verbs' shared loader (ResearchRole: pin, seal, the E-10
// delisting-returns refusal) before any payload is read.
//
// Decisions d in [score_begin, score_end - 2) (each has its d + 2 label). Per decision and
// candidate k:
//   q_k(d)  the centred tied rank of signal_k[d] over the used names (member && present && ok:
//           the exposures verb's rows) with a finite signal, 0 on the other used names;
//           residualised twice on the basis Q (y - Q Q'y, one refinement step, as the fitter);
//           divided by its gross when live (entry gross > 0, finite residual gross > 1e-9 x
//           entry), else 0 (flat).
//   f_k(d)  sum_i q_k(d)_i r_i(d+2), r the valid guarded return of interval d + 2 (invalid: 0);
//           NaN on a flat decision.
//   tau_k   mean over consecutive decisions of sum_i |q_k(d)_i - q_k(d-1)_i| (no drift; a flat
//           book is 0).
//   h_k(d)  report only (F-3): sum_i q_k(d)_i sum_{j<21} r_i(d+2+j), the decision book held over
//           the 21 sessions after its fill session; NaN when flat or when interval d + 22 is past
//           the role's last session.
// The basis is the K2 export's, which equals numpy's QR to rounding, and the sums run in
// ascending order, so f and tau equal the fitter's records to 1e-12 (not bit for bit).
//
// NEWDIR (exclusive) receives factor.f64 (decisions x candidates, decision-major), tau.f64
// (candidates), factor_h21.f64 (decisions x candidates) and manifest.json LAST
// (atx.factor-series/v1: role pin, window, decision sessions, candidates, used rows, refusals,
// semantics, file SHAs).

#include <iosfwd>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_exposures_verb.hpp"
#include "strategy_price_exposures.hpp"

namespace atx::impl::strategy {
// The traded horizon of the report-only series (sessions held after the fill session).
inline constexpr atx::usize kFactorHorizonSessions = 21;

// The fitter's price-risk context over decisions [begin, begin + decisions) of one panel, in the
// compact layout the factor loop reads: per decision the used names (ascending) and their Q rows.
struct FactorContext {
  atx::usize instruments{}, begin{}, decisions{};
  std::vector<atx::usize> row_begin; // decisions + 1 offsets into rows
  std::vector<atx::usize> rows;      // used names of each decision, ascending
  std::vector<atx::f64> basis;       // rows.size() x kBasisColumns: their Q rows
  std::vector<atx::f64> forward;     // decisions x instruments: interval d + 2 return, invalid 0
  std::vector<atx::f64> forward_h21; // decisions x instruments: sum of 21 forward rows from d
                                     // (NaN where fewer than 21 remain)
  std::vector<ExposureDecision> outcome; // decisions: refusal and used rows of export_decision
};

// export_decision over [begin, end) (each decision reads sessions <= d + 2; end + 2 <= dates).
// Errors: InvalidArgument for geometry, those of export_decision; OutOfRange on allocation.
[[nodiscard]] atx::core::Result<FactorContext> build_factor_context(
    const PriceExposureInput& panel, std::span<const atx::u8> member,
    const PriceExposureConfig& cfg, atx::usize begin, atx::usize end);

// One candidate's unsigned factor record.
struct FactorSeries {
  std::vector<atx::f64> f;   // decisions; NaN on a flat decision
  std::vector<atx::f64> h21; // decisions; report only (NaN: flat or past the window)
  atx::f64 tau{};
  atx::usize live_decisions{};
};
// Working storage reused across candidates (contents private to the implementation).
struct FactorScratch {
  std::vector<std::pair<atx::f64, atx::usize>> ranked;
  std::vector<atx::f64> x, q_prev, q_cur;
};
// The record of `signal` (the candidate's whole payload: ctx's panel dates x instruments,
// date-major). Errors: InvalidArgument when the signal is not a whole number of rows covering the
// context's decisions; OutOfRange on allocation.
[[nodiscard]] atx::core::Result<FactorSeries> factor_series(const FactorContext& ctx,
                                                            std::span<const atx::f64> signal,
                                                            FactorScratch& scratch);

struct FactorExportConfig {
  std::string role_path, role_sha256, signals_directory, output_directory;
  atx::u64 max_working_bytes{1'400'000'000ULL};
};
// The verb above (see the header comment). Every refusal comes before any output.
[[nodiscard]] atx::core::Status run_factor_export(const FactorExportConfig& cfg,
                                                  std::ostream& progress);
// argv[0] is the "factors" verb. Exit 0 ok, 1 refused, 2 usage.
[[nodiscard]] int dispatch_factors(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy

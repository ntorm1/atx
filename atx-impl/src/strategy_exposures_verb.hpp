#pragma once

// The `exposures` verb of atx-equity-strategy-targets (platform v8 D-2, contract K2): the
// price-risk context that fit_composition_weights.py builds in Python (PricePanel,
// neutralization_basis, Context.build), exported from the C++ exposures that the NAV
// replay neutralizes with, so the fitter can read one implementation instead of its own.
//
//   exposures --role ROLE/manifest.json --role-sha256 SHA --output NEWDIR [--max-bytes N]
//
// Decisions d in [score_begin, score_end - 2) of the pinned role (the fitter's window: each
// has its d + 2 label). NEWDIR (exclusive) receives, streamed:
//   basis.f64            decisions x names x 4 little-endian f64, [d][i][k]: row i of the
//                        orthonormal Q of [1, z_beta, z_vol, z_log_adv] over the used names
//                        (member && present && ok, ascending) on a used name, else 0; all 0 on
//                        a refused decision. basis[d][i][0] != 0 iff name i is used at d.
//   forward_returns.f64  decisions x names f64: the valid return of interval d + 2 = (d+1, d+2],
//                        NaN when invalid (the fitter maps NaN to 0).
//   manifest.json        LAST: role pin, window, geometry, file SHAs, per-decision used rows,
//                        the refused decisions {decision_index, reason, used_rows}, semantics.

#include <iosfwd>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_price_exposures.hpp"

namespace atx::impl::strategy {
inline constexpr atx::usize kBasisColumns = kPriceExposureCount + 1; // intercept + exposures

// What export_decision reports for one decision.
struct ExposureDecision {
  BasisRefusal refusal{BasisRefusal::None};
  atx::usize used_rows{}; // member && present && ok names at d (also on a refusal)
};
// Working storage of export_decision, reused across decisions. Binding exposure's session
// ring to the panel (enable_session_ring) logs each session once over the whole export.
struct ExposureExportScratch {
  PriceExposureScratch exposure;
  NeutralizeScratch neutralize;
  std::vector<atx::f64> exposures, q;
  std::vector<atx::u8> ok;
  std::vector<atx::usize> rows;
};
// Decision d over `panel` with the decision membership `member` (date-major, the panel's
// geometry, nonzero = member): the used names are member && present && ok (the exposures of
// d); basis (instruments x kBasisColumns, row-major) receives neutralization_basis's Q on the
// used names and 0 elsewhere (all 0 on a refusal); forward (instruments) the interval d + 2
// return by session_interval_returns. Reads sessions <= d + 2; requires d + 2 < dates.
// Errors: InvalidArgument for geometry, those of compute_price_exposures, neutralization_basis
// and session_interval_returns; OutOfRange on allocation failure.
[[nodiscard]] atx::core::Result<ExposureDecision> export_decision(
    const PriceExposureInput& panel, std::span<const atx::u8> member,
    const PriceExposureConfig& cfg, atx::usize d, ExposureExportScratch& scratch,
    std::span<atx::f64> basis, std::span<atx::f64> forward);

struct ExposureExportConfig {
  std::string role_path, role_sha256, output_directory;
  atx::u64 max_working_bytes{1'400'000'000ULL};
};
// The verb above on the engine role reader (read_strategy_role, then the external pin). The
// research-window seal is checked from the pinned manifest before any payload is read.
// price-risk-v1 at the PriceExposureConfig defaults (the fitter's constants).
[[nodiscard]] atx::core::Status run_exposure_export(const ExposureExportConfig& cfg,
                                                    std::ostream& progress);
// argv[0] is the "exposures" verb. Exit 0 ok, 1 refused, 2 usage.
[[nodiscard]] int dispatch_exposures(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy

#pragma once

// atx::engine::research::composition -- the `atx-research-composition` executable (P9 lane D2;
// migration plan section 2: one executable per library verb, JSON spec in, JSON out).
//
//   atx-research-composition fit --factors DIR --spec FILE --output NEWDIR
//
// DIR: a K-P9-4 factor series directory (`atx-equity-strategy-targets factors`, contract K-P9-4a:
// manifest.json atx.factor-series/v1, factor.f64, tau.f64, factor_h21.f64, each verified against
// the manifest's extent and SHA-256 by admission::read_factor_series). TRAIN: the decisions whose
// session lies in [kTrainBeginNs, kTrainEndExclusiveNs) (research_window.hpp); a decision at or
// after the seal is refused.
// FILE (atx.composition-fit-spec/v1, at most 16 MiB):
//   {"schema": "atx.composition-fit-spec/v1",
//    "composition": "ew-theme-std-v1" | "ew-theme-std-aim-v1" | "ic-shrink-v1"
//                   | "ic-shrink-aim-v1",
//    "members": [{"id": ..., "theme": ..., "sign": 1, "tier": <source tier, ew-theme-std rules>,
//                 "gain": <aim gain, the aim rules>}, ...],   (the fitter's admitted members, in
//                                                             admission order)
//    "tier_scores": {tier: score, ...},                       (ew-theme-std rules)
//    "theme_erc": bool, "theme_tsmom": bool, "theme_resid": bool,
//    "registered_themes": [theme, ...]}                       (theme-resid-v1: registered order)
// NEWDIR (created exclusively) receives composition_fit.json (atx.composition-fit/v1): the inputs'
// SHA-256, the members and themes, and per stage every fitted value the fitter's weights document
// records (fit.hpp CompositionFit), members and themes by name; floats in shortest round-trip form.
//
// P9 lane D3W (contract K-P9-8, ruling D3-1): `fit ... --walk-forward expanding --lag N --step N
// [--min-decisions N]` (lag and step required with it, min-decisions default 21; none of the three
// without it) also refits the spec walk-forward (fit.hpp fit_walk_forward) and adds the block
//   "walk_forward": {"rule": "walk-forward-v1", "window", "lag", "step", "min_decisions",
//                    "rows": [{"decision_index", "from_session", "fit_decisions", "last_fit_index",
//                              "last_fit_session", "weights": [per member]}, ...]}
// Without the flag the document's bytes are unchanged.
//
// Exit codes: 0 fitted; 1 refused (message on `err`, nothing published); 2 usage.

#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/engine/research/composition/fit.hpp"

namespace atx::engine::research::composition {

inline constexpr std::string_view kFitSpecSchema = "atx.composition-fit-spec/v1";
inline constexpr std::string_view kFitSchema = "atx.composition-fit/v1";
inline constexpr std::string_view kFitFile = "composition_fit.json";

// The spec of FILE's text. Err(InvalidArgument) on a malformed document.
[[nodiscard]] core::Result<FitSpec> parse_fit_spec(std::string_view text);

// The fit of `spec_file` on `factors_dir`, published into `output_dir` (created exclusively), with
// the walk-forward rows when `walk_forward` is set. Returns the published document's text.
[[nodiscard]] core::Result<std::string> run_fit(
    const std::filesystem::path& factors_dir, const std::filesystem::path& spec_file,
    const std::filesystem::path& output_dir,
    const std::optional<WalkForwardSpec>& walk_forward = std::nullopt);

// The verb: argv as main() receives it.
[[nodiscard]] int research_composition_main(std::span<const std::string_view> args,
                                            std::ostream& out, std::ostream& err);

} // namespace atx::engine::research::composition

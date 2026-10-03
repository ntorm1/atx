#pragma once

// atx::engine::research::admission -- the `atx-research-admission` executable (platform P9 B1):
// the C++ screen on a K-P9-4 factor series directory.
//
//   atx-research-admission screen --factors DIR --candidates FILE --output NEWDIR
//                                 [--screen v4-prior-v1|v4-prior-v2]
//                                 [--sign-rule gate-prior-v1|wave-zero-kept-v1]
//
// DIR: the `factors` verb's output (atx.factor-series/v1 manifest.json; factor.f64, tau.f64 and
// factor_h21.f64, each verified against the manifest's SHA-256 and extent).
// FILE: a JSON object whose "candidates" array lists, in roster order and with the factor
// manifest's ids, each candidate's {id, family, theme, tier, prior_sign, runner_sign, cache_entry,
// cache_payload_sha256}. Other keys are ignored, so the fitter's own admission.json (whose
// candidates carry these keys in library order) is accepted as is: only the metadata keys are
// read, never a decision.
// TRAIN: the decisions whose session lies in [kTrainBeginNs, kTrainEndExclusiveNs)
// (research_window.hpp); a decision at or after the seal is refused.
// NEWDIR (exclusive) receives admission.csv (the fitter's V4 columns), traded_horizon.csv (F-3,
// report only) and manifest.json LAST (rules, inputs, counts, admitted order, the sign rule's
// verdict per admitted candidate).
//
// Exit codes: 0 screened; 1 refused (message on `err`, nothing published); 2 usage.

#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/admission/admission_csv.hpp"
#include "atx/engine/research/admission/screen.hpp"
#include "atx/engine/research/admission/sign_rule.hpp"

namespace atx::engine::research::admission {

inline constexpr std::string_view kFactorSeriesSchema = "atx.factor-series/v1";
inline constexpr std::string_view kAdmissionManifestSchema = "atx.research-admission/v1";

struct ScreenSpec {
  std::filesystem::path factors_dir;
  std::filesystem::path candidates_file;
  std::filesystem::path output_dir;
  ScreenId screen{ScreenId::V4PriorV1};
  SignRule sign_rule{kPm735SignRule};
};

// A verified K-P9-4 directory.
struct FactorSeriesInput {
  std::vector<std::string> ids;           // candidates, roster order
  usize decisions{};
  std::vector<i64> decision_sessions_ns;  // decisions
  std::vector<f64> factors;               // decisions x candidates
  std::vector<f64> taus;                  // candidates
  std::vector<f64> factors_h21;           // decisions x candidates
  std::string manifest_sha256;
  std::string role_manifest_sha256;
};

// Err(InvalidArgument) on a malformed manifest or a payload whose extent or SHA-256 differs;
// Err(IoError) when a file cannot be read.
[[nodiscard]] core::Result<FactorSeriesInput> read_factor_series(const std::filesystem::path &dir);

// The candidates of FILE's text. Err(InvalidArgument) on a missing or mistyped metadata key, a
// prior sign outside {0, 1}, a runner sign outside {-1, 0, 1}, tiers mixing grades and integers,
// or a duplicate id.
[[nodiscard]] core::Result<std::vector<CandidateMeta>> parse_candidates(std::string_view text);

// The screen of `spec`, published into spec.output_dir (created exclusively, manifest last).
// Returns the admitted ids in admission order.
[[nodiscard]] core::Result<std::vector<std::string>> run_screen(const ScreenSpec &spec);

// The verb: argv as main() receives it.
[[nodiscard]] int research_admission_main(std::span<const std::string_view> args,
                                          std::ostream &out, std::ostream &err);

} // namespace atx::engine::research::admission

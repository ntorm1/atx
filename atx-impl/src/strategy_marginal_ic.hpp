#pragma once
#include <iosfwd>
#include <string>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
// `atx-equity-strategy-ic marginal` (platform v8 F-2, contract K6): does each candidate add
// anything the book does not already have? Per decision date of the pool's role, every library
// candidate's centred rank (over the pool's member names) is residualised on the book composite
// (the pool's saved combined signal) and, with a themes file, on the theme composites of the
// pool's weighted members (at most 11 regressors, plus the intercept), grouped by the weights
// file's own theme block when it has one and re-ranked as the blend re-ranks them under
// ew-theme-std-v1 (review B-2). Against the runner's h 21
// label: marginal_ic21 = correlation of that residual with the label's ranks, ic21 = the
// candidate's Spearman rank IC on the same names, each with a Bartlett HAC t at lag 21 (engine
// combine/marginal_rank_ic.hpp). max_abs_rho is the largest |mean daily correlation of centred
// ranks| to another library candidate.
//
// Streams by date: one row of each cached payload, the composite and the label is resident at a
// time; the role is read once to build the h 21 labels and released before streaming.
// Inputs are bound to one another, not trusted: the role by the pool's role_manifest_sha256, its
// sessions and ids by the pool's axis payloads, the themes file by the pool's
// composition_weights_sha256, and every cached payload by its sidecar's SHA256.
struct MarginalIcConfig {
  // The u pass's --candidate-cache DIR; entries are read (v2 layout) under DIR/<vm identity of
  // this build> or DIR, in <role sha>/ or <role sha>/fp_*/.
  std::string candidate_cache_directory;
  std::string library_path, library_sha256; // SHA optional: verified when given, always recorded
  std::string pool_path, pool_sha256;       // <role>_combined.json of a --save-combined run
  std::string role_manifest;                // the pool's role manifest (labels)
  std::string themes_path;                  // optional: the pool's composition weights file
  std::string fields_directory;             // optional: picks the entry whose field payloads match
  std::string output_directory;             // must not exist; receives marginal_ic.json
  atx::usize min_names{50};
  atx::u64 max_working_bytes{600ULL << 20};
};
// Conservative peak bytes: the role while labels are built, labels, member mask, one row per
// stream, the daily series and the pairwise sums. No term is dates x names x candidates.
[[nodiscard]] atx::core::Result<atx::u64> marginal_ic_working_bytes(
    atx::usize dates, atx::usize names, atx::usize score_rows, atx::usize candidates,
    atx::usize regressors);
[[nodiscard]] atx::core::Status run_marginal_ic(const MarginalIcConfig& cfg, std::ostream& progress);
// argv[0] is the verb itself ("marginal").
[[nodiscard]] int dispatch_marginal_ic(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy

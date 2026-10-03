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
// pool's weighted members (at most 34 regressors, plus the intercept), grouped by the weights
// file's own theme block when it has one and re-ranked as the blend re-ranks them under
// ew-theme-std-v1 (review B-2). Against the runner's h 21
// label: marginal_ic21 = correlation of that residual with the label's ranks, ic21 = the
// candidate's Spearman rank IC on the same names, each with a Bartlett HAC t at lag 21 (engine
// combine/marginal_rank_ic.hpp). max_abs_rho is the largest |mean daily correlation of centred
// ranks| to another library candidate.
//
// Streams by date: one row of each cached payload, the composite and the label is resident at a
// time; the role is read once to build the h 21 labels and released before streaming. Each row
// is compacted to the date's member names before any kernel runs (P9 S1: every kernel reads
// finite cells only, and a nonmember is NaN in every input, so the bits are those of the full
// role row).
// Inputs are bound to one another, not trusted: the role by the pool's role_manifest_sha256, its
// sessions and ids by the pool's axis payloads, the themes file by the pool's
// composition_weights_sha256, and every cached payload by its sidecar's SHA256.
//
// P9 S1 options (each absent: the v8 verb, every output byte but the stage timings unchanged):
//   candidates_path     --candidates FILE: UTF-8, one library candidate id per line. Only the
//                       listed candidates are residualised and get a row; rho is formed only for
//                       pairs with a listed id; every candidate stays in the pool's regressors
//                       and in the max_abs_rho search. Rows equal the full run's rows for them.
//   pair_cache_directory  --pair-cache DIR: pair statistics reused across runs on the role
//                       (strategy_marginal_pair_cache.hpp); hits equal the computed bits.
//   verified_digests_path --verified-digests FILE: one payload SHA256 per line that the caller
//                       verified (the u pass of the same wave); a payload whose recorded SHA256
//                       is listed has its extent checked but is not re-hashed.
//   workers             --workers N (1..16): DetPool date bands; per-row writes only, pair
//                       values added in date order after each band join, so every N gives the
//                       same bits.
//   exclude_self        --exclude-self (needs --themes): a book member's row is residualised on
//                       regressors without its own term (the review CM s.4 bias: v8 member rows
//                       sit in a composite that contains them). Report-only rows.
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
  std::string candidates_path;       // optional (P9 S1)
  std::string pair_cache_directory;  // optional (P9 S1)
  std::string verified_digests_path; // optional (P9 S1)
  atx::usize workers{1};             // 1..16 (P9 S1)
  bool exclude_self{false};          // P9 S1
  // Reference switch, no CLI flag: false streams every role name per row (the v8 layout)
  // instead of the date's member names. Outputs are byte-identical either way (test
  // MarginalIc.CompactedRowsEqual).
  bool compact_rows{true};
};
// Conservative peak bytes: the role while labels are built, labels, member mask, one row per
// stream, the daily series and the pairwise sums. No term is dates x names x candidates.
[[nodiscard]] atx::core::Result<atx::u64> marginal_ic_working_bytes(
    atx::usize dates, atx::usize names, atx::usize score_rows, atx::usize candidates,
    atx::usize regressors);
// The same with the P9 S1 options: every band worker beyond the first holds its own row set, a
// band chunk holds one row of pair values per row, --exclude-self holds a second regressor set
// per worker, and --pair-cache holds the loaded pairs. Defaults: the five-argument peak.
struct MarginalWorkingExtras {
  atx::usize workers{1};
  bool exclude_self{false};
  atx::usize computed_pairs{0}; // pairs evaluated per row
  atx::usize cached_pairs{0};   // pair statistics loaded from --pair-cache
};
[[nodiscard]] atx::core::Result<atx::u64> marginal_ic_working_bytes(
    atx::usize dates, atx::usize names, atx::usize score_rows, atx::usize candidates,
    atx::usize regressors, const MarginalWorkingExtras& extras);
[[nodiscard]] atx::core::Status run_marginal_ic(const MarginalIcConfig& cfg, std::ostream& progress);
// argv[0] is the verb itself ("marginal").
[[nodiscard]] int dispatch_marginal_ic(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy

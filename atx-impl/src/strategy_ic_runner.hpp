#pragma once
#include <iosfwd>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
namespace atx::impl::strategy {
struct IcRunnerConfig {
  std::string library_path, library_sha256;
  std::string train_manifest, train_sha256;
  std::string validation_manifest, validation_sha256;
  std::string orientations_path, orientations_sha256; // optional pinned TRAIN artifact; validation-only
  std::string output_directory;
  atx::u64 max_working_bytes{512ULL<<20};
  atx::usize min_names{20},min_dates{128};
  atx::usize workers{1}; // 1 is the existing serial baseline; explicit2..4 share one VM pool
  bool save_combined{false}; // exact blend+support artifact, no reevaluation or portfolio simulation
  bool plan_only{false}; // pinned metadata/DSL compilation only; no role payloads
  // Optional reusable raw DSL signals under DIR[/<vm-identity>]/<sha>/, where
  // <sha> is the role manifest SHA256 for a candidate reading only close/raw_close/
  // volume and the role's fields manifest SHA256 for one reading extra fields; the
  // <vm-identity> level is omitted only for the legacy identity dslvm1_clang18.1.
  // Never part of the recipe because a verified hit reproduces the VM bytes exactly.
  std::string candidate_cache_directory;
  // Optional pinned per-candidate composition weights replacing equal weights.
  std::string composition_weights_path, composition_weights_sha256;
  // Optional pinned point-in-time field directories (atx.research-role-fields/v1,
  // SHA256 of DIR/manifest.json), each bound to its role's manifest and axes.
  // Only extra fields some candidate's DSL references are loaded. Absent: the
  // recipe, cache keys and every output are byte-identical to runs without them.
  std::string train_fields_directory, train_fields_sha256;
  std::string validation_fields_directory, validation_fields_sha256;
};
// IC-only research; no book, surfaces, fees, Sharpe, stock events or holdout.
// TRAIN21h sample rank-IC fits signs; validation uses frozen signs; screening remains diagnostic for the fixed blend.
// One role, one label cache and one evaluated DSL signal are retained at a time.
// A pinned orientations artifact plus its adjacent canonical-hash-bound recipe.json
// enables validation-only execution; TRAIN metadata is checked but its payload is not loaded.
// A library declares close/raw_close/volume plus any extra fields; every declared
// extra must be present in each scored role's pinned fields manifest.
[[nodiscard]] atx::core::Status run_ic(const IcRunnerConfig&,std::ostream& progress);
// The candidate cache's VM identity and the engine sources pinned against its
// semantics version (paths repo-relative, hashed in this order; see the runner).
struct IcCacheVmIdentity {
  int semantics_version{};
  std::string identity;
  std::vector<std::string> sources;
  std::string sources_sha256;
};
[[nodiscard]] IcCacheVmIdentity ic_cache_vm_identity();
[[nodiscard]] int dispatch_ic(int argc,char** argv,std::ostream& out,std::ostream& err);
} // namespace atx::impl::strategy

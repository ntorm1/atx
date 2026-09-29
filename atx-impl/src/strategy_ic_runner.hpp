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
  // Optional reusable raw DSL signals under ROOT = DIR[/<vm-identity>] (the
  // <vm-identity> level is omitted only for the legacy identity dslvm1_clang18.1).
  // Content-keyed layout (atx.dsl-candidate-signal/v2, written by this runner):
  // ROOT/<role-sha>/<id>.<dsl16>.{f64,json} for a candidate reading only close/
  // raw_close/volume, ROOT/<role-sha>/fp_<fk16>/<id>.<dsl16>.{f64,json} for one
  // reading extra fields, where dsl16 = the first 16 hex of SHA256(DSL text) and
  // fk16 those of SHA256 over the candidate's sorted "field=<name>:<payload sha>\n"
  // lines. The sidecar records field_payload_sha256 (field -> payload SHA256) and
  // signal_key_sha256; one field payload change misses only the candidates that
  // read it, and a changed DSL under an old id is a new entry, never a refusal.
  // The v1 layout (ROOT/<role-or-fields-manifest-sha>/<id>.{f64,json}) stays
  // readable in place; see candidate_cache_legacy_fields. Never part of the
  // recipe because a verified hit reproduces the VM bytes exactly. The option also
  // caches each candidate's IC result beside its signal entry
  // (<entry dir>/ic<v>_<key16>/<stem>.json), keyed on the signal payload SHA256.
  std::string candidate_cache_directory;
  // Extra atx.research-role-fields/v1 directories (repeatable) whose manifests
  // keyed v1 field entries (ROOT/<their manifest sha>/<id>.json). Such an entry is
  // a hit when, for every field its candidate reads, that manifest's payload
  // SHA256 equals the pinned manifest's. The pinned manifest itself needs no
  // listing. A manifest is authenticated by its own SHA256, which the v1 sidecar
  // names, so no separate pin is taken. Only read with --candidate-cache.
  std::vector<std::string> candidate_cache_legacy_fields;
  // Metadata-only: resolve every candidate against the cache (no role payloads,
  // no output directory) and print hits/misses plus the unreferenced entries
  // under ROOT with their bytes. Requires --candidate-cache; excludes --plan-only.
  bool cache_report{false};
  // Optional pinned per-candidate composition weights replacing equal weights. Bound
  // to TRAIN by train_manifest_sha256; against a frozen TRAIN artifact (which must be
  // an unweighted run) also by provenance.orientations_sha256 == --orientations-sha256
  // and provenance.fields_manifest_sha256 == TRAIN's fields pin.
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
// Same shape for the IC-result cache: its semantics version, identity (compiler,
// FP flavor, IC SIMD width) and the engine IC scoring sources pinned against it.
[[nodiscard]] IcCacheVmIdentity ic_result_cache_identity();
[[nodiscard]] int dispatch_ic(int argc,char** argv,std::ostream& out,std::ostream& err);
} // namespace atx::impl::strategy

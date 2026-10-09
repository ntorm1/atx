"""ABS: --absent-members / --absent-members-sha256 in the IC runner (config, run_ic, score_role,
dispatch_ic), the module's CMake tail append and the ic_detail TU list."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from abs_edit import File  # noqa: E402

ROOT = "C:/atx-wt/pool-28/"

# ---- strategy_ic_runner.hpp: the two config fields ----
h = File(ROOT + "atx-impl/src/strategy_ic_runner.hpp")
h.once("""  std::string theme_registry_path, theme_registry_sha256;
};
""", """  std::string theme_registry_path, theme_registry_sha256;
  // absent-members-v1 (P9 lane ABS, ruling OD3P-1; strategy_ic_absent.hpp): an
  // atx.od3-flagged-members/v1 list (the SHA-256 of its bytes pinned) of library members the pass
  // treats, on every date, as members with no data. Both or neither; refused with
  // --no-composition. Absent: every output byte unchanged; present, summary.json records
  // absent_members and nothing else names it (no recipe term, no cache key).
  std::string absent_members_path, absent_members_sha256;
};
""")
h.save()

# ---- strategy_ic_detail.hpp: the TU list names the new module ----
d = File(ROOT + "atx-impl/src/strategy_ic_detail.hpp")
d.once("""//   strategy_ic_runner.cpp        score_role, outputs, run_ic, dispatch_ic
""", """//   strategy_ic_runner.cpp        score_role, outputs, run_ic, dispatch_ic
//   strategy_ic_absent.cpp        absent-members-v1 list (strategy_ic_absent.hpp; P9 lane ABS)
""")
d.save()

# ---- strategy_ic_runner.cpp ----
r = File(ROOT + "atx-impl/src/strategy_ic_runner.cpp")
r.once("""#include "strategy_ic_runner.hpp"
#include "strategy_ic_composition.hpp"
""", """#include "strategy_ic_runner.hpp"
#include "strategy_ic_absent.hpp"
#include "strategy_ic_composition.hpp"
""")

r.once("""// rule table rows the file carries; the rows whose stage runs are the composition's ordered stage
// list (composition_stages, strategy_ic_rules.hpp) and the combined manifest's rule keys.
co::Result<Json> score_role(const IcRunnerConfig& cfg,const Library& lib,const Role& spec,
    const KnownManifests& known,const PinnedWeights& pinned,std::vector<int>& signs,Json& frozen,
    const std::string& recipe_sha,const std::string& orientation_pin,std::ostream& progress) {
""", """// rule table rows the file carries; the rows whose stage runs are the composition's ordered stage
// list (composition_stages, strategy_ic_rules.hpp) and the combined manifest's rule keys.
// `absent` (absent-members-v1, P9 lane ABS; empty: no list): per candidate 1 when listed.
co::Result<Json> score_role(const IcRunnerConfig& cfg,const Library& lib,const Role& spec,
    const KnownManifests& known,const PinnedWeights& pinned,std::span<const u8> absent,
    std::vector<int>& signs,Json& frozen,const std::string& recipe_sha,
    const std::string& orientation_pin,std::ostream& progress) {
""")

r.once("""  for (usize k=0;k<lib.candidates.size();++k) {
    const auto& candidate=lib.candidates[k];
    ledger<<Json{{"id",candidate.id},{"status","started"},{"number",k+1}}.dump()<<'\\n'<<std::flush;
""", """  for (usize k=0;k<lib.candidates.size();++k) {
    const auto& candidate=lib.candidates[k];
    const bool listed=!absent.empty() && absent[k]!=0; // absent-members-v1
    ledger<<Json{{"id",candidate.id},{"status","started"},{"number",k+1}}.dump()<<'\\n'<<std::flush;
""")

r.once("""    // Under --label-terminal both IC results must be cached (each reads the signal).
    if (cfg.no_composition && ic_scope && cache.hits[k]) {
""", """    // Under --label-terminal both IC results must be cached (each reads the signal). Never for a
    // listed member (absent-members-v1 is refused with --no-composition; its IC is never cached).
    if (cfg.no_composition && ic_scope && cache.hits[k] && !listed) {
""")

r.once("""          signal_buffer,meter,progress));
    }
    const std::span<const f64> signal(signal_buffer);
""", """          signal_buffer,meter,progress));
    }
    // absent-members-v1 (P9 lane ABS, OD3P-1): a listed member's data is removed here, after the
    // candidate cache served or stored its exact bytes. Every cell non-finite: its IC rows, its
    // TRAIN orientation and every composition rule see a member with no data on any date.
    if (listed) std::fill(signal_buffer.begin(),signal_buffer.end(),quiet_nan);
    const std::span<const f64> signal(signal_buffer);
""")

r.once("""    const auto ic_started=std::chrono::steady_clock::now();
    if (ic_scope) {
      if (!acquired.entry || !hash_valid(acquired.entry->payload_sha))
""", """    const auto ic_started=std::chrono::steady_clock::now();
    // A listed member's IC is scored from its blanked signal and never read from or written to the
    // IC-result cache, whose entries key on the real signal bytes (absent-members-v1).
    const bool ic_cached=ic_scope.has_value() && !listed;
    const bool legacy_cached=legacy_scope.has_value() && !listed;
    if (ic_cached) {
      if (!acquired.entry || !hash_valid(acquired.entry->payload_sha))
""")

r.once("""      daily_series=scratch_series(scratch);
      if (ic_scope) {
        ATX_TRY_VOID(ic_cache_store(ic_entry,*ic_scope,candidate,acquired.entry->payload_sha,scored,
""", """      daily_series=scratch_series(scratch);
      if (ic_cached) {
        ATX_TRY_VOID(ic_cache_store(ic_entry,*ic_scope,candidate,acquired.entry->payload_sha,scored,
""")

r.once("""    if (legacy_labels) {
      if (legacy_scope && !legacy_probed) {
""", """    if (legacy_labels) {
      if (legacy_cached && !legacy_probed) {
""")

r.once("""        legacy_series=scratch_series(*legacy_scratch);
        if (legacy_scope) {
""", """        legacy_series=scratch_series(*legacy_scratch);
        if (legacy_cached) {
""")

r.once("""    // Both new options are fully validated here, before any role payload.
    ATX_TRY(const auto pinned,composition_weights(cfg,lib,themes));
""", """    // Both new options are fully validated here, before any role payload.
    ATX_TRY(const auto pinned,composition_weights(cfg,lib,themes));
    // absent-members-v1 (P9 lane ABS): the list, checked against the library before any role
    // payload or output (nullopt without the flag).
    ATX_TRY(const auto absent,absent_members(cfg,lib));
    const std::span<const u8> absent_mask=absent?std::span<const u8>(absent->absent):std::span<const u8>{};
""")

r.once("""    // Not a method input (recipe unchanged): the pass scored IC and orientations only.
    if (cfg.no_composition) report["composition"]="skipped";
""", """    // Not a method input (recipe unchanged): the pass scored IC and orientations only.
    if (cfg.no_composition) report["composition"]="skipped";
    // Present only with --absent-members (P9 lane ABS): the list the pass applied. Not a method input
    // either: the recipe and every cache key are those of the run on the members' data removed.
    if (absent) report["absent_members"]=absent_members_record(*absent);
""")

r.once("""      auto scored=score_role(cfg,lib,role,known,pinned,signs,orientations,recipe_sha,
          report.value("orientations_artifact_sha256",std::string{}),progress);
""", """      auto scored=score_role(cfg,lib,role,known,pinned,absent_mask,signs,orientations,recipe_sha,
          report.value("orientations_artifact_sha256",std::string{}),progress);
""")

r.once("""  IcRunnerConfig cfg;
  try {
""", """  IcRunnerConfig cfg;
  // absent-members-v1 (P9 lane ABS): each of its two flags at most once ([0] the list, [1] its pin).
  std::array<bool,2> absent_given{};
  try {
""")

r.once("""               "[--label-terminal imputed-v1 --train-label-role MANIFEST --train-label-role-sha256 SHA "
               "[--validation-label-role MANIFEST --validation-label-role-sha256 SHA]] "
               "| --list-rules --json\\n"
""", """               "[--label-terminal imputed-v1 --train-label-role MANIFEST --train-label-role-sha256 SHA "
               "[--validation-label-role MANIFEST --validation-label-role-sha256 SHA]] "
               "[--absent-members JSON --absent-members-sha256 SHA] "
               "| --list-rules --json\\n"
""")

r.once("""               "    also prints its IC under today's labels (ic_missing_exit_v1) and the missing-exit share.\\n"
""", """               "    also prints its IC under today's labels (ic_missing_exit_v1) and the missing-exit share.\\n"
               "  --absent-members: absent-members-v1, an atx.od3-flagged-members/v1 list ({schema, members:\\n"
               "    [id, ...]}; other keys ignored); each listed candidate is treated on every date as a\\n"
               "    member with no data (its mass stays in its theme; weights, signs and verify unchanged);\\n"
               "    summary.json records absent_members. Each flag at most once; refuses --no-composition.\\n"
""")

r.once("""      else if (key=="--validation-label-role-sha256") cfg.validation_label_role_sha256=value;
      else throw std::invalid_argument("unknown option: "+key);
""", """      else if (key=="--validation-label-role-sha256") cfg.validation_label_role_sha256=value;
      else if (key=="--absent-members" || key=="--absent-members-sha256") {
        // A second list or pin would silently replace the first: refused (contract section 3).
        const bool pin=key=="--absent-members-sha256";
        if (std::exchange(absent_given[pin?1U:0U],true))
          throw std::invalid_argument(key+" given more than once");
        (pin?cfg.absent_members_sha256:cfg.absent_members_path)=value;
      }
      else throw std::invalid_argument("unknown option: "+key);
""")
r.save()

# ---- atx-impl/CMakeLists.txt: tail append ----
c = File(ROOT + "atx-impl/CMakeLists.txt")
assert c.text.endswith("target_link_libraries(atx-impl-pipeline PUBLIC atx::engine-research-ledger)\n")
c.text += """# P9 lane ABS: the IC exe's absent-members-v1 list (strategy_ic_absent.hpp, ruling OD3P-1). Metadata
# only (read once before any payload), so it keeps the default flags and the shared PCH.
target_sources(atx-impl-strategy PRIVATE src/strategy_ic_absent.cpp)
"""
c.save()
print("ok")

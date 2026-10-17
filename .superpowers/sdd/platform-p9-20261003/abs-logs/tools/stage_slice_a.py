"""ABS commit slicing: stage strategy_ic_runner.cpp WITHOUT the score_role semantics hunks (slice A =
parsing, refusals and the summary record), leaving the working tree untouched. The staged blob is
the working file with the slice-B hunks reverted; slice B then stages the working file itself."""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from abs_edit import File  # noqa: E402

ROOT = Path("C:/atx-wt/pool-28")
REL = "atx-impl/src/strategy_ic_runner.cpp"
f = File(str(ROOT / REL))  # read only; never saved

B = [  # (slice-B text in the working file, the HEAD text it replaced)
    ("""// `absent` (absent-members-v1, P9 lane ABS; empty: no list): per candidate 1 when listed.
co::Result<Json> score_role(const IcRunnerConfig& cfg,const Library& lib,const Role& spec,
    const KnownManifests& known,const PinnedWeights& pinned,std::span<const u8> absent,
    std::vector<int>& signs,Json& frozen,const std::string& recipe_sha,
    const std::string& orientation_pin,std::ostream& progress) {
""", """co::Result<Json> score_role(const IcRunnerConfig& cfg,const Library& lib,const Role& spec,
    const KnownManifests& known,const PinnedWeights& pinned,std::vector<int>& signs,Json& frozen,
    const std::string& recipe_sha,const std::string& orientation_pin,std::ostream& progress) {
"""),
    ("""    const bool listed=!absent.empty() && absent[k]!=0; // absent-members-v1
""", ""),
    ("""    // Under --label-terminal both IC results must be cached (each reads the signal). Never for a
    // listed member (absent-members-v1 is refused with --no-composition; its IC is never cached).
    if (cfg.no_composition && ic_scope && cache.hits[k] && !listed) {
""", """    // Under --label-terminal both IC results must be cached (each reads the signal).
    if (cfg.no_composition && ic_scope && cache.hits[k]) {
"""),
    ("""    // absent-members-v1 (P9 lane ABS, OD3P-1): a listed member's data is removed here, after the
    // candidate cache served or stored its exact bytes. Every cell non-finite: its IC rows, its
    // TRAIN orientation and every composition rule see a member with no data on any date.
    if (listed) std::fill(signal_buffer.begin(),signal_buffer.end(),quiet_nan);
""", ""),
    ("""    // A listed member's IC is scored from its blanked signal and never read from or written to the
    // IC-result cache, whose entries key on the real signal bytes (absent-members-v1).
    const bool ic_cached=ic_scope.has_value() && !listed;
    const bool legacy_cached=legacy_scope.has_value() && !listed;
    if (ic_cached) {
""", """    if (ic_scope) {
"""),
    ("""      if (ic_cached) {
        ATX_TRY_VOID(ic_cache_store(""", """      if (ic_scope) {
        ATX_TRY_VOID(ic_cache_store("""),
    ("""      if (legacy_cached && !legacy_probed) {""", """      if (legacy_scope && !legacy_probed) {"""),
    ("""        if (legacy_cached) {""", """        if (legacy_scope) {"""),
    ("""    const std::span<const u8> absent_mask=
        absent?std::span<const u8>(absent->absent):std::span<const u8>{};
""", ""),
    ("""      auto scored=score_role(cfg,lib,role,known,pinned,absent_mask,signs,orientations,recipe_sha,
""", """      auto scored=score_role(cfg,lib,role,known,pinned,signs,orientations,recipe_sha,
"""),
]
for new, old in B:
    f.once(new, old)
staged = ROOT / "build-equity" / "abs-slice-a-runner.cpp"
staged.write_bytes(f.text.encode("utf-8"))  # LF: the blob form
sha = subprocess.run(["git", "-C", str(ROOT), "hash-object", "-w", "--path", REL, str(staged)],
                     check=True, capture_output=True, text=True).stdout.strip()
subprocess.run(["git", "-C", str(ROOT), "update-index", "--cacheinfo", f"100644,{sha},{REL}"],
               check=True)
staged.unlink()
print("staged", REL, sha)

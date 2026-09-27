# Task T11 re-review, fix round 1 (`6093e7d2..1507a2d8`, fitter + test only; docs commits ignored)

`F:` = `atx-impl/tools/fit_composition_weights.py` at HEAD (unchanged in `e885687b`), `T:` = its test file,
`R:` = `atx-impl/src/strategy_ic_runner.cpp` at the root worktree HEAD.

### Finding Verdicts

- **I1. The fitter cannot find v3 cache entries under the runner's landed layout (VM-identity root + fields
  directory).** ADDRESSED.
  - The layout now comes from a pinned `--runner-summary` (`F:1130`, `F:969`). `CacheLayout.__init__` (`F:260-306`)
    reads exactly the keys the runner writes at `R:1259-1264` and `R:1272`.
  - The summary is bound to the orientations and the role before any entry is read:
    - status `complete` (`F:276`);
    - `orientations_artifact_sha256` equals `--orientations-sha256`, and `recipe_sha256` equals the orientations
      recipe (`F:277`). This matches `R:1389-1395`.
    - roles == [train] (`F:282`), and the manifest SHA equals `--train-sha256`.
  - **(a) ROOT.** The directory basename must equal the TRAIN SHA. A non-legacy identity needs
    `parent.name == identity` (`F:289-296`). This mirrors `cache_root` at `R:685-688`.
  - **(b) Field candidates.** `fields_directory` must be `ROOT/<research_fields.manifest_sha256>` (`F:305`).
    `resolve()` tries the base directory, then the fields directory. It accepts only an entry that names this id
    and DSL SHA, requires exactly one (`F:308-343`, `F:325`), and requires `fields_manifest_sha256` to be absent
    or equal (`F:329-331`). This mirrors `R:689-694` and `R:749-750`.
  - **(c) cached_payload_sha.**
    - `vm_identity` must be equal. Keyless sidecars follow the legacy rule: legacy identity, a base entry, and
      `engine_git_sha` in the two listed builds (`F:333-336`). This mirrors `R:697-703` and `R:752`.
    - The schema, role SHA, eval mode, layout, geometry, bytes, payload name and hash are all checked, plus
      role == train.
  - **Work records.** Records are bound to `fields_manifest_sha256` and `vm_identity`. Field records are named
    `<payload>.f-<F>.json` (`F:710`, `F:689-692`).
  - **Fixtures.**
    - Field-directory and identity-root cases: `T:855`, and the `Admission` fixture at `T:693`, which uses the
      fma identity plus 2 field candidates through the whole incremental path.
    - Fields mismatch in both directions (`T:874`). Foreign, keyless and wrong-engine identities (`T:881`).
    - Same-id foreign-DSL entries are ignored, and duplicates are refused (`T:892`).
    - Every summary binding refusal (`T:903`).

- **M1. The cache key's code component was a manually bumped string.** ADDRESSED.
  - `SCRIPT_SHA256` hashes the fitter's own bytes (`F:110`). It is stored in and required of:
    - every factor record (`F:673`, `F:689`);
    - the context meta, and therefore the context digest (`F:530`, `F:615`).
  - Any edit recomputes everything, so the silent-reuse path no longer exists.
  - Tests: `T:1007` (script change) and `T:1011` (tag change). The numpy version is still not bound; see New
    Breakage Minor 2.

- **M2. `Context.save` was documented as atomic but did rmtree then rename.** ADDRESSED.
  - The new code builds the context aside, moves the old one aside, renames, then deletes the old one
    (`F:586-606`).
  - The docstring now states the real kill outcomes.

- **M3. Late refusals after compute.** ADDRESSED.
  - A stale `.<out>.pending` is now refused up front. The check runs next to `out.exists()` and before the
    role, layout or any compute (`F:966`).
  - Test `T:923` asserts that the work dir is never created.
  - Residual: sub-bullet 2 is unchanged. `os.rename` replaces an empty directory on POSIX (`F:893`). This is
    race-only and does not affect the Windows target.

- **M4. The test's runner-parser port was stale.** ADDRESSED.
  - `runner_accepts` (`T:321-344`) now ports `R:638-668` faithfully:
    - `train_manifest_sha256` must equal the TRAIN SHA;
    - `signs` must be an object with known ids and integer ±1 values (bool and float are excluded);
    - every weighted id must have a sign.

- **M5. Boundary and invalidation gaps.** ADDRESSED.
  - Boundaries (`T:939`, `T:967`):
    - tau == 0.70 is admitted;
    - exactly 250 FIT days is sufficient;
    - exactly 250 common days is correlated, and 249 is low-overlap;
    - HOLD with 0 days is unstable;
    - |rho| == limit is admitted, and nextafter(limit, 0) is redundant.
  - Invalidation (`T:1011`, `T:1015`): a tag change and a record bound to another context digest.
  - Carried over from T9: an orientations identity mismatch (`T:923`) and sidecar identity mismatches (`T:874`,
    `T:881`).

- **M6. Column semantics a reader could conflate.** ADDRESSED.
  - New `redundant_rho` column: the |rho| at rejection time (`F:834`).
  - `undefined_rho_with` (enough overlap, zero variance) is now separate from `low_overlap_with` (< 250 common
    days) (`F:821-826`). Admission decisions are unchanged.
  - Both appear in the JSON and the CSV (`F:849-852`), plus `cache_entry` (base or fields).

- **M7. Structure: no module split, and T9 M4/M5 were open.** NOT ADDRESSED. The root said this was optional,
  and the fix declined it explicitly.
  - Only T9 M5 is fixed: the `m` variable is renamed `mkt` (`F:410`).
  - The module split is not done, and the literal `0.1`/`0.9` remains (`F:740` vs `SHRINK_LAMBDA` at `F:78`).

### Named check: will the fitter accept `build-equity/mega-v1-train-r2/summary.json`? YES

- **The summary.**
  - SHA `8c47e0ce…6766cf`, 268 KB, which is under the 16 MiB bound.
  - status `complete`, roles == [`train`].
  - `manifest_sha256` is `210fff96…c48d1de`: the repaired role v2, `build-equity/recent-fast-train-2020-2022-v2/manifest.json`.
  - `recipe_sha256` `fd933769…` equals the recipe in `orientations.json`.
  - `orientations_artifact_sha256` `db9b8aa6…` equals sha256(`orientations.json`).
- **The cache block.**
  - `candidate_cache.directory` is `build-equity/mega-candidate-cache\210fff…`: the legacy flat layout, with no
    identity subdirectory.
  - `vm_identity` is `dslvm1_clang18.1`, the legacy identity.
  - hits 0 / misses 48.
  - There is no `fields_directory` and no role `research_fields`, which is correct for library v1.
- **Sidecar keys: they are not keyless.**
  - All 48 sidecars share one key set, and each carries an explicit `vm_identity: "dslvm1_clang18.1"`.
  - `engine_git_sha` is `97e6b392…`. This is irrelevant, because the legacy engine-SHA rule applies only to
    keyless sidecars.
  - Every sidecar has role `train`, `role_manifest_sha256` `210fff…`, and matching eval_mode and layout.
  - None has `fields_manifest_sha256`.
  - So `resolve()` takes the base path with `fields_match` (key absent) and `vm_match` (equal identity).
- **Already demonstrated.**
  - `build-equity/mega-weights-v1r2-run1/receipt.json` records this HEAD fitter with exactly these pins, exit 0,
    60.7 s. The fitter's `script_sha256` there is `9a784695…`, which equals the current working-tree bytes of
    the 1507a2d8 file.
  - That run computed 48, admitted 8 (19 unstable, 21 redundant), and produced `weights_sha256` `90235117…`.
- **Caveat.** The summary directories are relative, so the fitter must run with cwd `C:/atx-wt/pool-2`, the
  runner's cwd. From any other cwd it refuses loudly with a missing entry. It never scores stale data.

### New Breakage in the Fix Diff

- **Critical/Important:** none.
- **Minor 1 (`F:110`): `SCRIPT_SHA256` hashes the checked-out bytes.**
  - This worktree is CRLF (`core.autocrlf=true`), so the value is `9a784695…`, while the LF git blob gives
    `a14207c1…`.
  - As a result, `provenance.script_sha256` cannot be matched against `git show` without eol normalization.
  - A checkout with a different eol recomputes the whole work dir (about 210 s for v3). That is
    over-invalidation only, never stale data.
  - Suggestion: record the eol-normalized SHA as well, or note this in the provenance.
- **Minor 2 (`F:110`, M1 residual): the numpy version is not part of the record or context binding.**
  - After a numpy upgrade, old records would be reused. The reuse is consistent but the provenance does not show
    it.
- **Minor 3 (`F:586-606`): strays are only cleaned for the same PID.**
  - Only `.partial-<pid>`/`.old-<pid>` directories from the same PID are removed.
  - A kill during save under another PID leaves a stray directory that nothing reads. At the measured context
    size, each one is about 148 MB.
- **Minor 4 (report only).**
  - The fix-round section of `task-T11-report.md:155-160` names neither the new tests nor their output. I
    verified 21 → 33 `test_` methods, with 12 new ones at `T:833-1020`.
  - The earlier "Root: exact real run" and smoke commands (`:131-138`) still pass the removed
    `--candidate-cache` and would exit 2. The root's real invocation (v1r2) already uses the correct flags.

### Out-of-Scope Observations

- **The v3 TRAIN run on role v2 needs new TRAIN fields.**
  - The existing TRAIN fields manifest `build-equity/recent-fast-train-2020-2022-v1-fields-v1/manifest.json` binds
    `role.manifest_sha256 = 3f53ee9a…`, which is role v1, not the repaired `210fff…`.
  - The runner's `bind_fields` refuses a mismatched role binding (`R:548`).
  - So the v3 TRAIN IC run on role v2 needs role-v2 fields rebuilt first. After that, the fitter follows the
    summary's new `fields_directory` automatically.
- **The fitter is more lenient than the runner about foreign-DSL sidecars.**
  - The fitter ignores a same-id sidecar with a different DSL (`F:322`). The runner refuses one in its own key
    directory (`R:753-758`).
  - This leniency is needed for a candidate that moved from base to fields, and it is safe, because the entry
    actually used must match id, DSL, role, identity and payload SHA. Ledgered for the final review only.
- **M7 remainder** (module split, `SHRINK_LAMBDA` literal): carried to the final review.

### Verdict

**Fix round:** Findings remain open: M7 only, which the root marked optional and the fix declined.

- The required I1 and M1 are ADDRESSED.
- M2-M6 are ADDRESSED.
- There is no new Critical/Important breakage.
- The r2 TRAIN summary is accepted, and a real exit-0 run already exists.

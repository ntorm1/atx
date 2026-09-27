# Task T11 report: admission screen `v3-admit-v1` + resumable fitter

Status: **DONE**. The interpretation choices are listed under "Decisions" and marked where the brief was silent.
Worktree `C:/atx-wt/pool-3`, branch `feat/mega-alpha-weights-20260926`.

## Commits
- `4b983cbd feat(impl): v3-admit-v1 screen + resumable weight fit [mega T11]`, on top of T9 `403e56fe`. It changes only:
  - `atx-impl/tools/fit_composition_weights.py`
  - `atx-impl/tools/test_fit_composition_weights.py`

No C++ and no CMake. Nothing to build.

## Tests (synthetic only)
`"C:\Program Files\Python312\python.exe" -m unittest discover -s atx-impl/tools -p test_fit_composition_weights.py -v`
gives **21/21 OK in about 5 s**. pytest agrees and pyflakes is clean. The 16 T9 fixtures were ported to the new CLI, and the T9 weights still match the independent slow reference to 1e-9. Five new fixtures:

- **ScreenRules** (hand-built factor rows, 400 FIT + 200 HOLD) covers every rule:
  - admitted; `reject_redundant(a)`; `reject_unstable` (FIT good, HOLD mean < 0); `reject_insufficient` (200 FIT days); `reject_turnover` (tau .9);
  - precedence: insufficient and turnover together give `reject_insufficient` with `failed_checks = [insufficient, turnover]`;
  - s_k = -1 admitted;
  - a low-overlap pair (160 < 250 common days) treated as uncorrelated and noted;
  - admission order equals descending FIT Sharpe;
  - FIT Sharpe and HOLD mean match the formulas to 1e-12.
- **Admission, one per class** (runs the full pipeline on a synthetic 2020-05 to 2022-03 world with persistent alphas). There is one candidate per rejection class plus two admitted; the correlated pair is resolved by FIT Sharpe (`slow_a` beats `slow_a_twin`).
  - Weights are > 0 only for admitted candidates.
  - `signs` equals the screen signs, and `slow_b` = -1.
  - `sign_conflicts == ["slow_b"]`: its runner sign is +1 and its screen sign is -1.
  - The weights still pass the Python port of the runner parser.
- **Admission, schema/CSV:** admission.json structure, CSV header and rows, canonical bytes.
- **Admission, incremental byte identity:** every one of these paths yields byte-identical `composition_weights.json`, `admission.json` and `admission.csv` equal to a fresh no-work-dir run.
  1. `main(... --max-new-candidates 2)` returns 3 and publishes nothing; 2 records and the context are persisted.
  2. `--max-seconds 1e-9` stops before any candidate (0 computed, 2 reused).
  3. Resume computes 4 and reuses 2.
  4. A fully cached run computes 0.
  5. A tampered record fails its SHA check and only that record is recomputed.
  6. A corrupt `basis.bin` plus a missing record rebuilds the context, and its `context.json` comes out bit-identical.
  7. With `close.f64` removed and every record cached, the run still succeeds: the price payload is never read.
- **Admission, nothing admitted:** exit 4, with only admission.json/csv published.

## Performance
**Synthetic, full geometry** (1155 x 5627, about 3000 members per decision, 12 candidates, real CLI with disk reads and SHA checks):
- **First run, `--max-seconds 25`:** context built in 18.8 s, 10 candidates at about 0.6 s each, then a clean stop at 24.7 s (exit 3). Peak RSS 385 MiB.
- **Resume:** context reused in **0.3 s** (the old fixed cost was about 19-30 s), then 2 candidates plus screen and fit. 1.9 s wall, 315 MiB.

**Real-data estimate:** the T9 v1 run measured about 30 s fixed plus about 1.4 s per candidate. For 120-130 candidates:
- run 1 (`--max-seconds 140`) does the context plus about 75-80 candidates;
- run 2 takes about 1 s fixed plus about 70 s for the rest, then the screen and fit take seconds.

Two runs, each under 180 s and about 0.5 GiB.

## Keys the T7-lane runner fix should verify (confirmed)
- **TRAIN role manifest SHA:** top-level **`train_manifest_sha256`**. `provenance.role_manifest_sha256` carries the same value.
- **`signs`**, a top-level object:
  - keys are library candidate ids;
  - values are the JSON integers `1` or `-1` (never 0);
  - an entry exists for every id whose applied sign is nonzero, so **every id with weight > 0 has one**;
  - an absent id always has weight 0.
  - Screen mode applies the `v3-admit-v1` s_k. `--screen none` applies the runner orientation sign (T9 rule).
  - `provenance.signs` names the rule (`v3-admit-v1-FIT-mean-sign;apply-pinned-signs`).
- `sign_conflicts` lists ids whose runner sign differs from the applied sign:
  - `admission.json.sign_conflicts` covers all candidates;
  - `provenance.sign_conflicts_weighted` covers only w > 0.
- Unchanged: `schema`, `library_sha256` and `weights` (every library id, finite >= 0). The file stays <= 1 MiB.

## Output (new directory, published atomically, never overwritten)
The output directory is `--output DIR`. It is built in `.DIR.pending` and then renamed. It contains:
- `composition_weights.json`: the runner input. Its pin is `weights_sha256` in the stdout summary.
- `admission.json`: `atx.dsl-admission/v1`, holding:
  - screen id, rules, inputs + SHAs, window, counts, `admitted` (in admission order), `sign_conflicts`;
  - per candidate: id, family, status, failed_checks, redundant_with, admission_rank, s_k, runner_sign, sign_agrees, tau, tau_over_limit, fit_days, fit_mean, fit_sharpe, hold_days, hold_mean, hold_sharpe, max_abs_rho, max_abs_rho_with, low_overlap_with, cache_payload_sha256.
- `admission.csv`: the same table, with fixed columns and repr floats.
- `composition_weights.json.provenance.admission_sha256` binds the weights to the table.

Exit codes:
- **0**: complete.
- **1**: refused; nothing published.
- **3**: incomplete budget stop; nothing published; rerun.
- **4**: admission published but no weights, because nothing was admitted or no weight was positive.

## Incremental design
**Work dir** is `--work-dir W`, laid out as `W/<train-manifest-sha>/<semantics-tag>/`.

**`context/`**: raw little-endian arrays and `context.json`.
- Arrays: columns, used, basis (T x 4 x W), forward, used_rows.
- `context.json` holds each array's SHA-256, dtype and shape, plus the role SHA, window and refused decisions.
- It is verified in full on load; any mismatch means a rebuild, written atomically.

**`factors/<cache-payload-sha256>.json`**: one per candidate, holding:
- the unsigned series `f_unsigned` (null on flat days), `tau` and `live_decisions`;
- the role SHA, semantics strings, `context_sha256` (the digest of the context it used), the context refusals and used rows;
- `content_sha256`, a self-hash.

**Key and verification**
- Records are keyed by (TRAIN manifest SHA, semantics tag, cache payload SHA). The cache payload SHA comes from the runner sidecar, which is still validated field by field. That lets a cached record be reused without hashing the 52 MB payload.
- A record is valid only if all of these hold:
  - the self-hash matches;
  - schema, semantics, role, payload SHA and decision count match;
  - the value types are right;
  - it names the current context digest, whenever the context is loaded.
- Anything else is recomputed.
- The semantics tag is sha256(`CONTEXT_SEMANTICS|FACTOR_SEMANTICS`)[:16]. Bump those strings whenever the math changes.

**Budget**
- `--max-seconds S` stops *before* starting a candidate when elapsed + the slowest candidate so far > S. So a run never overruns by more than the uninterruptible context build, which is about 30 s real and only happens on the first run.
- `--max-new-candidates N` gives a deterministic stop.
- Both need `--work-dir`.

**Cheap path:** when every record is valid, neither the context nor the role price payload is read. Only the pinned manifest and `sessions.i64`/`ids.u64` are verified.

## Decisions (brief silent or ambiguous)
1. **Windows by decision session.** FIT is decisions with a session in [2020-01-01, 2022-01-01); HOLD is [2022-01-01, 2023-01-01). This matches the study. The last two FIT decisions' labels (d+2) fall in early January 2022.
2. **"Finite days" means live days.** A day is live when the neutralized book is non-flat. Flat days (fewer than 2 finite names, a spanned residual, or a refused decision) are NaN.
   - s_k, FIT Sharpe, HOLD mean/Sharpe, fit_days and correlations use live days only.
   - **The MV fit uses all TRAIN decisions with flat days = 0**, the T9 rule ("fit on full TRAIN"). A flat book earns 0.
3. **Status precedence:** insufficient > turnover > unstable > redundant. `failed_checks` lists every failed check among the first three. A turnover reject can therefore carry `unstable` too, so the library owner sees both.
4. **Redundancy:**
   - Pearson rho on the unsigned f (|rho| does not depend on sign) over FIT days where both are live.
   - Ties in FIT Sharpe keep library order.
   - `redundant_with` is the already-admitted candidate with the largest |rho|.
   - A pair with < 250 common days, or an undefined rho, counts as uncorrelated and is listed in `low_overlap_with` (by id).
   - `max_abs_rho` / `max_abs_rho_with` are reported for every candidate against the *final* admitted set, excluding itself.
5. **Signs, per root ruling (b):** s_k is used for admission, for the fit, and in `signs`. Conflicts are listed; there is no `reject_sign_conflict`. A candidate with s_k = 0 (no live FIT days, or a FIT mean exactly 0) fails as insufficient or unstable and is absent from `signs`.
6. **`--screen none`** reproduces the T9 rule and its weight values; the T9 fixtures still match the reference. Its JSON bytes differ from the T9 v1 artifact because of the new keys. Reproduce those exact bytes with `403e56fe`.
7. **Orientations** are still a required pinned input: they bind the library, TRAIN and cache run, and supply the runner signs for conflict reporting.

## Root: exact real run (180 s / 1536 MiB cap)
Run from `C:/atx-wt/pool-2` after cherry-picking `4b983cbd` (the tree must be clean). Repeat the command with a **new bounded-runner `--output` each time** (`-run1`, `-run2`, ...) until the fitter exits 0 (or 4). The bounded runner records an exit-3 pass as `outcome: process-error, exit_code: 3`; that is expected and means "rerun".

The fitter's `--output`, `--work-dir` and every other argument stay the same across reruns. Placeholders for v3: `<LIB>`/`<LIB_SHA>` is library v3; `<ORIENT>`/`<ORIENT_SHA>` is the `orientations.json` of the v3 TRAIN IC run made with `--candidate-cache build-equity/mega-candidate-cache`.

```powershell
& "C:\Program Files\Python312\python.exe" scripts/run_bounded_research.py --output build-equity/mega-weights-v3-run1 --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --bind atx-impl/tools/fit_composition_weights.py --bind <LIB> --bind build-equity/recent-fast-train-2020-2022-v1/manifest.json --bind <ORIENT> -- "C:\Program Files\Python312\python.exe" atx-impl/tools/fit_composition_weights.py --library <LIB> --library-sha256 <LIB_SHA> --train build-equity/recent-fast-train-2020-2022-v1/manifest.json --train-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493 --orientations <ORIENT> --orientations-sha256 <ORIENT_SHA> --candidate-cache build-equity/mega-candidate-cache --screen v3-admit-v1 --work-dir build-equity/mega-fit-work --max-seconds 140 --output build-equity/mega-weights-v3
```

**Smoke test available today (v1, 48 candidates):** use the same command with these substitutions:
- `--library atx-impl/strategies/slow_price_volume_ic48_v1.json --library-sha256 1ec75242f0328a459ac114256f99f534eb0b8ec2e642794d3f4ad846779a09ff`
- `--orientations build-equity/mega-v1-train-cache-b/orientations.json --orientations-sha256 c014e71f7d6a16da894d680dda583d1e644e70d5ccc43e7f19c983d32d9f9312`
- `--output build-equity/mega-weights-v1-screen`, with bounded `--output build-equity/mega-weights-v1-screen-run1`.

This is a screen trial on v1, so ledger it as one if you run it.

**Expected:**
- run1 takes about 170 s wall at most: about 30 s for the context plus candidates until 140 s. It exits 3.
- run2 reuses the context in about 1 s and finishes the rest. It exits 0 in about 60-80 s for 120-130 candidates.
- Peak RSS is about 0.5 GiB either way.

**stdout summary** (one JSON line):
- `weights_sha256`: the value for `--composition-weights-sha256`;
- `admitted`, `counts`, `sign_conflicts`, `sign_conflicts_weighted`;
- `weighted_standalone_turnover`: the NR denominator;
- `tau_flagged`, `refused_decisions`, `computed_this_run`, `reused`.

**Runner input:** `--composition-weights build-equity/mega-weights-v3/composition_weights.json --composition-weights-sha256 <weights_sha256>`. For the weights to mean what they were fit on, the runner must apply `signs` (T7-lane change).

## Fix round 1 (review task-T11-review.md) — appended by root (subagent was blocked from writing)

Commit f0c223e5 (pool-3, on 4b983cbd); 33/33 OK, pyflakes clean.
- I1 fixed: new pinned `--runner-summary PATH --runner-summary-sha256 SHA` (TRAIN-only runner summary; `--candidate-cache` removed). Summary binding: status complete; orientations_artifact_sha256 == --orientations-sha256; recipe sha matches orientations; roles == [train]; train manifest sha == --train-sha256. Layout from roles[train].candidate_cache: directory = ROOT/R, fields_directory = ROOT/F (F = research_fields.manifest_sha256), vm_identity (absent = legacy dslvm1_clang18.1; non-legacy requires ROOT basename == identity). Per candidate: directory/<id>.json then fields_directory/<id>.json; exactly one entry with matching id + DSL sha; sidecar checks mirror runner cached_payload_sha (schema/id/DSL, role sha, eval_mode, layout, geometry, payload sha, fields sha presence/equality, vm_identity; keyless legacy only with legacy identity + engine sha in {429cbe43, 6d85ac2a}). Work-cache field records named <payload>.f-<F>.json.
- M1 fixed: SCRIPT_SHA256 (fitter file bytes) in every factor record + context digest.
- M2-M6 and T9 M5 fixed (atomic context save; stale .pending refused early; runner_accepts ports landed parser incl. train_manifest_sha256 + signs; boundary fixtures; redundant_rho / undefined_rho_with / cache_entry columns). Not changed: M7 module split; T9 M4 literal 0.1/0.9.

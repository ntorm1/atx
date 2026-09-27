# Task T34c review: `studies/v51_train.sh` (base e620d3c1, head 8adfd75a, pool-8)

Reviewer: Opus 5.5, read-only. Diff read once in full (the hunk is complete, file lines 1-316 = diff lines 19-334). File
line numbers below are for `.superpowers/sdd/mega-alpha-20260926/studies/v51_train.sh` at 8adfd75a. Commands run: `bash -n`
(LF and CRLF copies), DRY=1 invocations in pool-8 (pool-8 `git status --porcelain --ignored` hash unchanged before and
after), `sha256sum` of pinned inputs, and `cmp` of command blocks against v4/v5. Nothing was built, run on real data or
written outside the scratchpad and this review file.

### Spec Compliance

- ✅ Spec compliant. Each requirement was checked against the code:
  - **Req 1** (v5 fit/w/nav plus the v4 u phase): u at :107-127, fit at :127-207, w at :208-252, nav at :253-311. The env
    names (COMP, COMBINED, THETA, DUST, RATE, LEV) are kept.
  - **Req 2** (library v5.1 asserted before any phase):
    - :99-100 computes the library SHA and requires it to equal the full `9e5ea08c…58e0`. The recipe, R2 and fields pins
      are checked at :101, before the phase loop at :105.
    - `sha256sum` of `fund_industry_ic_v5.json` and `.recipe.json` in pool-8 and pool-2 gives `9e5ea08c…58e0` and
      `a26670b0…3aea`, and the pool-2 R2 and fields-v6 manifests give `210fff96…` and `32565c32…`: all match the constants
      at :36-42.
    - The output names (`mega-v51-train-u`, `mega-weights-v51-{ew,aim}`, `mega-fit-work-v51`, `mega-v51w-train-{ew,aim}`,
      `mega-nav-v51-*`) collide with none of the v4/v5 entries listed in `pool-2/build-equity`.
  - **Req 3** (SHA pin chain): see named risk (b).
  - **Req 4** (TRAIN only, refusals): see named risk (a).
  - **Req 5** (both compositions): :128-132 selects `W_EW` or `W_AIM` from COMP. Each composition has its own weights dir,
    weighted-pass prefix `$WT-$COMBINED` (:214) and NAV dir.
  - **Req 6** (reference cell by default): :259-264. See named risk (d).
  - **Req 7** (bounded runner): the four runner command blocks are byte-identical to v4/v5 (named risk c).
  - **Req 8** (DRY prints, writes nothing): verified by the pool-8 status hash check.
- Two disclosed deviations, both judged acceptable (see Minor 1 and 2):
  - u keeps at most 3 passes where v4 allowed 6.
  - Some directory names follow the family convention rather than the literal `mega-v51-*` prefix. The brief's own
    `mega-nav-v51-*` does the same.
- ⚠️ Cannot verify from the diff alone:
  - The two compositions share the fitter work dir `mega-fit-work-v51` (:49, :148). The report says its records are keyed by
    payload SHA. v5 shared `mega-fit-work-v5` the same way (v5_train.sh:40), so the pattern is already exercised.
  - `mega-candidate-cache` (:48) is shared with v4/v5 and written by the u and w passes. Brief req 7 requires this bind,
    and the cache is additive per candidate key. It is not a v4/v5 output directory, but it is shared mutable state: the
    controller should confirm that is intended.
  - The nav phase needs the T37 `atx-equity-strategy-targets.exe`, which supports `--rule aim-partial-v5 --dust-multiple
    --aim-leverage`.
  - The paired reference `mega-nav-v5-ew-t.05-d.1-fixed` does not exist in pool-2 yet, and the DSR N choice is still open.
    Both are root and gate decisions (report §6 concerns 1-3).

### Named-risk checks

- **(a) TRAIN only.** Checks, and the evidence for each:
  - **Structural whitelisting.** Every manifest, library and path is a constant (:35-53). Any env value that reaches a path
    is restricted by a `case` statement:
    - COMP: :128-132.
    - COMBINED: :209-213 and :254-258.
    - RATE: :289-293.
    - Phases: :104.
  - **No date input.** No date flag is passed anywhere. Dates come only from the SHA-pinned TRAIN manifest.
  - **`bad()` scan** (:54-64):
    - It runs over every argument and every env value the script reads (:97), plus `$PWD` and every constant (:98).
    - It runs again on every bounded-runner argv (:67) and on the NAV dir (:296).
  - **nav re-checks the role.** nav requires `role == "train"`, `role_manifest_sha256 == R2S` and
    `train_manifest_sha256 == R2S` in the combined header (:280-281). Those keys match the IC runner's manifest writer
    (`atx-impl/src/strategy_ic_runner.cpp:461-482`).
  - **Validation manifest names.** The pool-2 validation manifests are all named `recent-fast-validation-2023-2024-v1*`
    and the validation runs `mega-v4-VAL-*`. `bad()` refuses every one of those names.
  - **DRY probes** (all exit 2):
    - Refused by `bad()`: `u Val2`, `COMBINED=VAL`, `COMP=x-2024`.
    - Refused by the whitelists: `COMBINED=ew-2026`, `valset` and `holdout`.
  - **Result:** no path found.
- **(b) SHA pin chain.**
  - **u → fit.** fit reads `$U.final` (:134). It pins `orientations.json` and `summary.json` against the SHAs u recorded
    (:135-138, recorded at :124-125), then passes those SHAs to the fitter (:146).
  - **fit → w.** w pins `composition_weights.json` against the recorded `$W.weights.sha256` (:217, :221; recorded at :159).
    A provenance check (:222-235) then requires the weights file to show:
    - `library_sha256 == LS` and `rule == RULE`;
    - `orientations_sha256` and `runner_summary_sha256` equal to the recorded u pins;
    - `train_manifest_sha256 == R2S`.

    Those keys exist in the fitter's output (`fit_composition_weights.py:1529-1534`, `:1696-1717`).
  - **w → nav.** nav pins C and W against `$P.combined.sha256` and `$W.weights.sha256` (:266-271; recorded at :250-251). It
    checks that the combined header's `composition_weights_sha256` equals the recorded W SHA of the same composition, plus
    library v5.1, fields-v6 and `status == "complete"` (:280-285).
  - **Library first.** The library is asserted at :100, before the usage check (:103) and before the phase loop (:105).
  - **No overwrites.** Before writing, each phase refuses when its output exists:
    - `$U.final` (:108);
    - `$W` (:133);
    - `$P.final` (:215);
    - `$N` / `$N-run` (:297);
    - `fresh()` refuses an output dir that has no receipt dir (:94).
  - **DRY chain.** `DRY=1 COMP=ew-theme-v1 COMBINED=ew bash … u fit w nav` passes every placeholder SHA through consistently:
    u to fit, fit to the w provenance check, and w to the nav header check.
- **(c) Commands and flags.** Each block was normalised (leading space, the `>/dev/null 2>&1` redirect and the `run`/`"$PY"`
  prefix stripped; `$WT` rewritten to `$P` for w) and compared with `cmp`. All four are byte-identical:
  - u (:114-116) vs v4_train.sh:28-30;
  - fit (:144-148) vs v5_train.sh:61-65;
  - w (:239-242) vs v5_train.sh:119-122;
  - nav (:299-303) vs v5_train.sh:148-152.

  Common to all four blocks:
  - Bounded-runner limits are `--seconds 180 --max-rss-mib 1536 --min-free-mib 512` (:31).
  - nav always passes `--rule aim-partial-v5 --cadence 1`. The reference cell is `.05` / `.1` / fixed / `--aim-leverage 1`,
    and `--band-multiple` is never passed. The DRY output shows these exact flags.
- **(d) Opt-in for a non-reference NAV cell.** Every probe below was run with DRY:
  - Exit 2: `THETA=.08`, `THETA=.08 EXTRA_CELL=yes`, `LEV=1.0` and `RATE=per-name`.
  - Exit 2: `THETA=".05 .1" DUST=fixed`. Each of the four values defaults to a non-empty value, so a space-joined equality
    cannot be spoofed.
  - `THETA=.08 EXTRA_CELL=1`: allowed.
  - `RATE=bogus EXTRA_CELL=1`: exit 2.
- **(e) Windows Git Bash robustness.**
  - **CRLF.** pool-2 has `core.autocrlf=true`, and its `v5_train.sh` is checked out CRLF (163 CR bytes). A CRLF copy of
    v51_train.sh (316 CR bytes) passes `bash -n` under GNU bash 5.2.15 (msys). A DRY nav run of that copy printed the
    correct command and exited 0, so the library pin, which would fail with a trailing CR, passed.
  - **Heredocs.** `bash -n` on the CRLF copy shows the `<<'EOF'` terminators close; otherwise the `fi` after them would be
    missing and parsing would fail.
  - **The Python path with spaces.** It is always quoted:
    - `"$PY" "$@"` (:69);
    - `-- "$PY"` passed as one argv element (:144);
    - the `rc`, helper and nav_summ calls quote it (:74, :166, :225, :276, :308-310).
  - `q()` quotes it for DRY display.
  - **State files.** The `.final` and `.sha256` state files sit under `build-equity/`, which `.gitignore:12` (`build-*/`)
    ignores. The bounded runner's clean-tree guard therefore still passes between phases.

### Strengths

- **Faithful derivation.** The four real-data command blocks are byte-identical to v4/v5 after normalisation, so the new
  script cannot drift from the audited flags.
- **Tighter chain than v5.** The weights provenance check at w (:222-235) and the combined-header check at nav (:273-288)
  bind every link to library v5.1, TRAIN role v2 and the recorded upstream SHA. v5 relied on the file pins alone.
- **Layered TRAIN-only defence.** The script combines three layers:
  - whitelisted env values;
  - `bad()` over arguments, env values, constants and every runner argv;
  - the `role == "train"` header check.
- **Fails closed.** Phase names are validated before any work (:104). The library SHA is asserted in DRY mode too (:100).
  Absent pins are handled explicitly in DRY (:81, :87-89).
- **DRY is really side-effect free.** `rc`, `logs`, `hits`, `record` and the helpers all short-circuit in DRY. The pool-8
  status hash, ignored files included, is unchanged after a full chain dry run.
- **Honest report.** Its claims match the code. The 3-vs-6 u-pass decision and the naming convention are disclosed rather
  than hidden.

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

None.

#### Minor (Nice to Have)

1. **:110, the u pass cap.**
   - Problem: u allows at most 3 passes, where brief req 1 says "bounded re-passes as v4_train.sh does" and v4 allowed 6.
     Because `fresh()` (:93) skips existing run dirs, a fourth attempt is impossible without editing the script. Editing
     then needs a commit, because of the clean-tree guard.
   - Impact is low: 37 of the 38 candidates are warm, and v4's cold u finished in 1 pass.
   - Fix: match v4's `1..6`, or state the cap in the ledger ruling.
2. **Directory naming** (:48-53).
   - Problem: `mega-weights-v51-*`, `mega-fit-work-v51` and `mega-v51w-train-*` do not all carry the literal `mega-v51-*`
     prefix from brief req 2.
   - The collision-free intent is met, and the brief's own `mega-nav-v51-*` deviates the same way.
   - Fix: record the convention in the ledger so the T39 ledger lines name the directories unambiguously.
3. **:304-311, a failed nav still exits 0.**
   - Problem: nav prints `rc` but never tests it. On `process-error`, `time-limit` or `none no-receipt` it still runs
     `nav_summ.py` against a missing or partial `$N`, and the script exits 0 (the last pipeline's status).
   - This is inherited from v5_train.sh:153-158. u, fit and w all fail closed, so nav is the odd one out.
   - Fix: after :304, run `case "$(rc $N-run)" in 0\ *) ;; *) echo "nav: failed"; exit 1 ;; esac` (the check is a no-op
     in DRY).
4. **:91 and :159, pin files can be overwritten or empty.**
   - `record` writes with `>` and no existence guard. The phase guards (:108, :133, :215) cover `.final` and the output
     dir, but not `.orientations.sha256`, `.summary.sha256`, `.weights.sha256` or `.combined.sha256` on their own.
   - `sha()` at :159 records an empty value if the fitter reports completed but did not write `composition_weights.json`.
     That fails closed later: an unquoted empty `$WS` makes `pin` abort on `set -u` with `$2: unbound variable`. The
     failure is correct but the message is opaque.
   - Fix: have `record` refuse an existing file or an empty value.
5. **:60 and :259-264, input checks are not exhaustive.**
   - `bad()` refuses 2023-2025 but not 2026+, although the global constraint says "2025+ stays reserved". The structural
     whitelists make this belt-and-braces only.
   - THETA, DUST and LEV are not checked to be numeric. With `EXTRA_CELL=1` they flow into the path `$N` (:294-295), so a
     value such as `THETA=../x` could create a runner directory outside the family.
   - Fix: add `20(2[3-9]|[3-9][0-9])` to `bad()`, and check `[[ $THETA =~ ^[0-9]*\.?[0-9]+$ ]]` (same for DUST and LEV).
   - Also: the `else unknown phase` branch at :312-313 cannot be reached since :104 was added. It is dead code.

### Assessment

**Task quality:** Approved

**Reasoning:**
- The script reproduces the audited v4/v5 bounded-runner commands exactly and asserts library v5.1 before any phase. It
  chains u → fit → w → nav through recorded SHAs, with provenance checks that are stricter than v5's.
- It enforces TRAIN-only both structurally and by refusal, and it is robust to a CRLF checkout and to the quoted Python
  path.
- The remaining issues are minor hardening items; none can let a validation input, an unpinned artifact or a v4/v5
  overwrite through.

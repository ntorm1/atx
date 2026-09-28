# Task T34c report: `studies/v51_train.sh` (library v5.1 pipeline)

Implementer: Opus 5.5 (t34c-v51-script-2), pool-8, branch `feat/mega-alpha-v51-pipeline-20260927`, base `e620d3c1`.
Continued from the committed `b2c957ff` (previous implementer, paused, no report). No Python or C++ changes. Nothing
was built, and no real-data or bounded-runner command was run. pool-2 was only read: `git status --porcelain` is still
empty there, and the only v51 entry in `build-equity/` is the existing `mega-v51-plan-run`.

**Status: DONE_WITH_CONCERNS.** The concerns are minor and listed in §6.

## 1. Commits

| SHA | Subject |
|---|---|
| `b2c957ff` | feat(studies): v51_train.sh pipeline for library v5.1 (T34c), from the previous implementer |
| `d88638ce` | fix(studies): v51_train.sh refusals, nav defaults, chain check (T34c) |
| (this report) | docs(mega-alpha): T34c report, committed with `git add -f` |

The root cherry-picks `b2c957ff` and `d88638ce`. The script is the only code file, and it needs no build, CMake registration or test target.

## 2. What the script does (b2c957ff, as amended by d88638ce)

`.superpowers/sdd/mega-alpha-20260926/studies/v51_train.sh` combines the fit, w and nav phases of `v5_train.sh` with the u phase of `v4_train.sh`, using library v5.1:

- **Pins, checked before any phase runs.**
  - `L=atx-impl/strategies/fund_industry_ic_v5.json`: the script computes its SHA-256 and requires it to equal the full `9e5ea08c…58e0`.
  - Recipe `fund_industry_ic_v5.recipe.json` `a26670b0…`.
  - TRAIN role v2 `recent-fast-train-2020-2022-v2/manifest.json` `210fff96…`.
  - fields-v6 `…-fields-v6/manifest.json` `32565c32…`.
  - I checked all four against pool-2: they match.
- **u**: the unweighted TRAIN IC pass. It runs `atx-equity-strategy-ic.exe` with the same flags and binds as `v4_train.sh` u (`--save-combined --candidate-cache mega-candidate-cache --workers 4 --min-names 1000 --max-memory-mib 1536`).
  - Output goes to `build-equity/mega-v51-train-u-<i>`, with at most 3 bounded passes. A pass left behind by an earlier invocation is skipped, so the phase can resume.
  - It records `mega-v51-train-u.final`, `.orientations.sha256` and `.summary.sha256`.
- **fit** (`COMP=ew-theme-v1` or `COMP=ew-theme-aim-v1`): the T31 fitter with `--orientation prior --screen v4-prior-v1 --max-seconds 150`.
  - It reads the u pass named in `.final` and pins that pass's orientations and summary SHAs.
  - Outputs: `mega-weights-v51-ew` or `mega-weights-v51-aim`. Both compositions share the work dir `mega-fit-work-v51`; its records are keyed by payload SHA, so they do not clash.
  - At most 3 passes; exit code 3 means "resume".
  - It records `<W>.weights.sha256` and prints a report: the admission diff against v4 `880a0a6a`, the `opex_at` row, and the theme / g_k table. For ew it also prints the maximum |w − w_v4| against `9a9c949a`.
- **w** (`COMBINED=ew|aim`): a weighted TRAIN IC pass with the recorded weights (`--composition-weights` plus the pinned SHA).
  - Before it runs, it checks the weights' provenance: library v5.1, the rule, the recorded u-pass orientations and summary SHAs, and the TRAIN manifest.
  - Output goes to `mega-v51w-train-<ew|aim>-<i>`, with at most 3 passes. It records `.final` and `.combined.sha256`.
- **nav** (`COMBINED=ew|aim`): `atx-equity-strategy-targets.exe nav --rule aim-partial-v5 --cadence 1 --trade-fraction θ --dust-multiple d --aim-leverage L --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824`. It never passes `--band-multiple`.
  - It pins the recorded combined signal and weights. It also checks that the combined header binds the recorded weights SHA, library v5.1, `role=train`, role v2, fields-v6 and `status=complete`.
  - Output goes to `mega-nav-v51-<C>-t.05-d.1-fixed`. It then runs `nav_summ.py --weights W --reference build-equity/mega-nav-v5-ew-t.05-d.1-fixed`, dropping `--reference` when that directory is absent.
- **Every real-data command** goes through `"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/<new> --bind … -- <cmd>`, with the same binds as v4/v5.
- **Never overwrites.** Each phase refuses if its output, its `.final`, or an output directory without a matching receipt directory already exists.
- **`DRY=1`** prints every bounded-runner and helper command line and writes nothing.

## 3. Gaps found in b2c957ff and fixed in d88638ce

1. **Refusal was too narrow** (brief req. 4). Only `*validation*` and `*2023*` were refused.
   - VAL tokens were missed (for example `mega-nav-v4-VAL-b2`, `val2`), and so was 2024.
   - Phase arguments and env values (COMP, COMBINED, THETA, DUST, RATE, LEV, EXTRA_CELL) were never checked.
   - Now every argument and every env value the script reads (plus `$PWD` and all constant paths) is checked up front, before any pin or phase: `validation`, `val` as a whole token in any case, 2023, 2024 and 2025 (reserved) all exit 2.
   - 64-hex SHAs are skipped. Words like `interval`, `eval`, `value` and `valid` still pass.
2. **nav had no default** (brief req. 6: "reference cell only by default"). THETA, DUST and RATE were required.
   - They now default to the reference cell (.05 / .1 / fixed / LEV 1).
   - Any other cell needs `EXTRA_CELL=1` exactly. Before, any non-empty value opted in, so `EXTRA_CELL=0` counted as opt-in.
3. **Chaining could be tighter** (req. 3). nav now checks that the combined it consumes was produced from the recorded weights of the same composition, library v5.1, TRAIN role v2 and fields-v6. The w provenance check also binds `train_manifest_sha256`.
4. **Phase names are validated before any phase runs**, so `u bogus` no longer runs u and then fails.
5. **`DRY_ROOT=<checkout>`** (honoured with DRY=1 only; exit 2 otherwise) points a dry run at another checkout read-only. I used it to dry-run against pool-2's real TRAIN pins.
6. **Robustness and docs.** The fit report uses `weights.get('opex_at', 0.0)`. The header now says construction is +1 per composition run, lists the v51-tagged directories, and documents EXTRA_CELL and DRY_ROOT.

**Decisions (small ambiguities):**
- **u keeps at most 3 passes** where v4 used 6. Measured history: the cold v4 u took 1 pass (86 s), v4.2 with 3 cold candidates took 1 pass (30 s), and the v5.1 plan-only run shows 37 of 38 entries warm. If a pass times out, the next pass resumes from the candidate cache.
- **Directory names follow the family convention** (`mega-weights-v51-*`, `mega-v51w-train-*`) rather than the literal `mega-v51-*` prefix. The brief's own `mega-nav-v51-*` does the same. None of them collides with a v4 or v5 artifact.
- **The fit work dir `mega-fit-work-v51` is fresh**, not a reuse of `mega-fit-work-v5`, so no v5 artifact is mutated. The fit cost is about 50 s cold (v5 aim: 50 s / 485 MiB, one pass).
- **The nav paired-dSR reference is the v5 R5' reference cell** `mega-nav-v5-ew-t.05-d.1-fixed`, as committed in b2c957ff. It answers whether v5.1 beats the frozen-v4 reference.

## 4. Verification

- `bash -n`: clean. A CRLF copy (what pool-2's `core.autocrlf=true` will check out, like `v5_train.sh`) also passes `bash -n` and dry-runs identically under GNU bash 5.2.15 (msys).
- DRY runs:
  - **From pool-8 itself:** R2 and fields are reported as `DRY pin (absent here)`; the library and recipe pins were checked.
  - **With `DRY_ROOT=C:/atx-wt/pool-2`:** every data pin was checked against the real artifacts. The v4 admission `880a0a6a` and weights `9a9c949a` diagnostic pins resolve too.
- Abridged DRY outputs (with `DRY_ROOT=C:/atx-wt/pool-2`):

```
[all phases] library v5.1 9e5ea08c…58e0 recipe a26670b0… train 210fff96… fields 32565c32… [DRY: nothing runs or is written]
[u]   DRY: python run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/mega-v51-train-u-run1
        --bind …ic.exe --bind …fund_industry_ic_v5.json --bind …train-v2/manifest.json --bind …fields-v6/manifest.json -- …ic.exe
        --library …ic_v5.json --library-sha256 9e5ea08c… --train … --train-sha256 210fff96… --train-fields …fields-v6 --train-fields-sha256 32565c32…
        --output build-equity/mega-v51-train-u-1 --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache build-equity/mega-candidate-cache
      DRY record mega-v51-train-u.final <- 1 ; .orientations.sha256 / .summary.sha256 <- DRY-sha(...)
[fit ew]  --output build-equity/mega-weights-v51-ew-run1 --bind ic_v5.json --bind ic_v5.recipe.json --bind train manifest --bind u-1/orientations.json
          --bind u-1/summary.json -- python fit_composition_weights.py --library …ic_v5.json --library-sha256 9e5ea08c… --orientation prior
          --screen v4-prior-v1 --composition ew-theme-v1 --recipe …v5.recipe.json --recipe-sha256 a26670b0… --work-dir build-equity/mega-fit-work-v51
          --max-seconds 150 --output build-equity/mega-weights-v51-ew ; record mega-weights-v51-ew.weights.sha256 ; fit report vs v4 880a0a6a / 9a9c949a
[fit aim] identical, with --composition ew-theme-aim-v1, output build-equity/mega-weights-v51-aim(-run1)
[w ew]    provenance check (W, 9e5ea08c…, ew-theme-v1, u-pass SHAs, 210fff96…) ; --output build-equity/mega-v51w-train-ew-run1 … --output
          build-equity/mega-v51w-train-ew-1 … --composition-weights build-equity/mega-weights-v51-ew/composition_weights.json --composition-weights-sha256 <recorded>
[w aim]   same with ew-theme-aim-v1, mega-weights-v51-aim, build-equity/mega-v51w-train-aim-1
[nav ew]  combined provenance check ; --output build-equity/mega-nav-v51-ew-t.05-d.1-fixed-run … nav --combined build-equity/mega-v51w-train-ew-1/train_combined.json
          --combined-sha256 <recorded> --role …train-v2/manifest.json --role-sha256 210fff96… --fields …fields-v6/manifest.json --fields-sha256 32565c32…
          --output build-equity/mega-nav-v51-ew-t.05-d.1-fixed --rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1
          --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824
          ; nav_summ.py --weights mega-weights-v51-ew/composition_weights.json --reference build-equity/mega-nav-v5-ew-t.05-d.1-fixed <N>
[nav aim] same with mega-v51w-train-aim-1 → build-equity/mega-nav-v51-aim-t.05-d.1-fixed
```

- Refusals (all exit 2, before any pin or phase):
  - Arguments: `u validation`, `mega-nav-v4-VAL-b2`, `fit-2024`.
  - Env values: `COMBINED=VAL` / `val`, `THETA=2023` (even with EXTRA_CELL=1), `COMP=validation`, `RATE=2025`, and a DRY_ROOT containing `-VAL-`.
  - Also non-DRY: `bash v51_train.sh validation` and `COMBINED=VAL … nav` exit 2 without touching anything.
- Non-reference NAV cells:
  - Refused (exit 2) without opt-in: THETA .08, `DUST=0 EXTRA_CELL=0`, RATE per-name, LEV 1.1.
  - Allowed with `EXTRA_CELL=1`, each writing to its own directory (`…-per-name`, `…-t.08-d.1-fixed-L1.1`).
- Other exit-2 cases: an unknown phase (nothing runs), no arguments (usage), a bad COMP or COMBINED, nav without COMBINED, and DRY_ROOT without DRY.
- Chain tests in a synthetic scratch root (DRY):
  - fit passes the recorded u SHAs through.
  - w passes the recorded W SHA through; nav passes the recorded C SHA through.
  - Tampering with the orientations, weights or combined file gives `PIN MISMATCH`, exit 1.
  - An existing `.final`, `W`, nav run directory, or an output directory without its receipt directory is refused (exit 1).
  - DRY wrote no files.
- The Python helpers were run standalone against existing v5 TRAIN artifacts in pool-2, read-only:
  - The w check passes on `mega-weights-v5-aim` with its own pins and fails with MISMATCH (exit 1) against the v5.1 library or the wrong rule.
  - The nav combined check passes on `mega-v5w-train-aim-1` with W_aim `54f823c1` and fails with MISMATCH on wrong weights or library.
  - The fit report runs for both rules on copies where `gpa` was renamed to `opex_at`.
  - These are already-ledgered v5 TRAIN artifacts. No v5.1 statistic and no validation data was read.

## 5. Root command lines for T39 step 2b, in order

Run in C:/atx-wt/pool-2 after cherry-picking `b2c957ff` and `d88638ce` (the tree must be clean for the bounded runner):

```
S=C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/studies/v51_train.sh
bash -n $S
DRY=1 bash $S u
DRY=1 COMP=ew-theme-v1 bash $S fit
DRY=1 COMP=ew-theme-aim-v1 bash $S fit
DRY=1 COMBINED=ew bash $S w
DRY=1 COMBINED=aim bash $S w
DRY=1 COMBINED=ew bash $S nav
DRY=1 COMBINED=aim bash $S nav
bash $S u                                  # mega-v51-train-u-1 (opex_at the one cache miss)
COMP=ew-theme-v1 bash $S fit               # mega-weights-v51-ew  (+1 composition trial)
COMP=ew-theme-aim-v1 bash $S fit           # mega-weights-v51-aim (+1 composition trial)
COMBINED=ew bash $S w                      # mega-v51w-train-ew-<i>
COMBINED=aim bash $S w                     # mega-v51w-train-aim-<i>
COMBINED=ew bash $S nav                    # mega-nav-v51-ew-t.05-d.1-fixed  (+1 construction)
COMBINED=aim bash $S nav                   # mega-nav-v51-aim-t.05-d.1-fixed (+1 construction)
```

Ledger each printed SHA (orientations, summary, W_ew, W_aim, C_ew51, C_aim51) as it lands. Trials: admission 38 (v4-prior-v1 unchanged), composition +2, construction +1 per composition (+2).

## 6. Concerns

1. **NAV binary dependency.** nav needs `atx-equity-strategy-targets.exe` with `--rule aim-partial-v5 --dust-multiple --aim-leverage`. That comes from T31 and is in the source at `e620d3c1`. The ledger order is T37 (the v5-1 build) before T39, so run nav on the T37 binary.
2. **Paired reference.** `mega-nav-v5-ew-t.05-d.1-fixed` does not exist yet in pool-2; the T38 grid step 3 produces it. If the v5.1 nav runs before that, no paired dSR is printed. Re-run `nav_summ.py --weights <W> --reference <v5 ref> <v5.1 N>` once the reference exists.
3. **DSR N.** `nav_summ.py` uses its default DSR N (10, per R6'), and V[SR_n] comes only from the directories given. How to deflate over the v5 grid plus the v5.1 cells is a root decision for the gate write-up; the script does not settle it.
4. **Non-canonical reference values.** `THETA=0.05` (as opposed to `.05`) is treated as a non-reference cell and refused without EXTRA_CELL. That errs on the safe side; just use the defaults.
5. **DRY semantics.** Any non-empty `DRY` value, including `DRY=0`, means dry. That errs on the safe side too (it only prints).
6. **Helpers outside the bounded runner.** The small in-script Python helpers (fit report, provenance checks) and `nav_summ.py` run outside the bounded runner, as in `v5_train.sh`. They only read small JSON headers or NAV outputs, never raw real data.

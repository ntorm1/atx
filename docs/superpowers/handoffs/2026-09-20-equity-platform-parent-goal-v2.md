# Equity platform parent goal — handoff v2 (2026-09-20, end of day)

Supersedes the stop point in `2026-09-20-equity-platform-parent-goal.md` (v1). Every
constraint in v1 still binds unless restated here. Read v1 §"Constraints" first, then this
file top to bottom. The exact stop point is in §1.

## 1. Exact stop point

- Checkpoint 12 (security transition planners): validated, receipt immutable. Unchanged today.
- Checkpoint 13 (claims-aware replay seam): MINIMAL-CLOSED. T1 (claims state, atomic commit)
  and T2 (replay seam) complete, reviewed, natively measured, receipted. T3 (engine scenarios
  B/C), T4 (atx-impl allocation certification) and T5 (equity-book wiring, `--transitions`
  flag) are DEFERRED by ruling, not done. The seam is not wired into `atx-impl`.
- Checkpoint 14 (cross-sectional forecast evaluation, Stage 1): COMPLETE. Engine unit, trial
  ledger library, `atx-impl equity-ic` subcommand, independent exact oracle + comparator, one
  real-data run on the frozen 189-observation 2013 context, receipt + addendum, docs.
- Checkpoint 15: NOT STARTED. A read-only research agent (`cp15-research`) was dispatched to
  choose between Stage 2 (2013–2019 per-year folds), D-3 (decay-driven cadence/hysteresis)
  and a walk-forward protocol, then STOPPED on user instruction before writing. If
  `.superpowers/sdd/equity-platform-parent-goal/research-cp15.md` exists it is partial and
  unreviewed; treat it as a draft. The research brief text is in the ledger (progress.md, last
  lines) and can be re-dispatched verbatim.
- No commit or merge was made at any point. None is authorized by this handoff.
- Nothing is in flight. No background agents, shells or monitors.

## 2. Where everything is

Worktree: `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`, HEAD
`dffb609b7a3c` (unchanged all day; all work is uncommitted). `git status --short` lists
~204 entries (modified + untracked); that is expected. Shared checkout `C:\atx` untouched
except the git-ignored data directory `C:/atx/data/equity_ic_training_2013_20260920`
(new, written by the real-data run). The shared stash stack (9 entries) was never touched.

SDD ledger (append-only, every ruling and measurement, 78 lines):
`.superpowers/sdd/equity-platform-parent-goal/progress.md`. Reports/reviews in the same
directory: `cp14-design-review.md`, `cp14-design-rereview.md`, `cp14-design-rereview2.md`,
`cp14-task-{T1,T2T3,T4,T5,T7a}-report.md`, `cp14-task-{T1,T2T3,T4,T5}-review.md`,
`cp14-task-T1-rereview.md`, `final-branch-review.md`, `research-alpha-pipeline.md`; plus the
checkpoint-13 records `task-T1-*`, `task-T2-*`, and checkpoint-12 `task-B/C-*`.

### 2.1 Pinned artifacts (SHA-256 at handoff time)

| artifact | sha256 |
|---|---|
| cp12 design (must stay unchanged) `atx-engine/reviews/2026-09-20-iteration12-security-transition-design.md` | `0a964aebc15f14ea25ddd19fad5be723835b6b7a6e99a6aa308f3b7ecf95caee` |
| cp12 receipt `atx-engine/reviews/2026-09-20-security-transition-validation.json` | `09112ffe8e89a09d7bc9d5cc70371fd18d6cc4f7b4b66048f7a0125310f37f2a` |
| cp13 design `atx-engine/reviews/2026-09-20-iteration13-claims-aware-replay-design.md` | `b95d24eee73c09403a45af420d30a62ead01d443704c16d0bc0585ca5045d6b0` |
| cp13 receipt `atx-engine/reviews/2026-09-20-claims-aware-replay-validation.json` | `982edfc422961ee3bb4cd9c697712aae905aee81dd0340a66ec50beab7475072` |
| cp14 design (Revision 5 + §11.1–§11.9; embedded in the stage and the ledger) `atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md` | `888c726b123c02b39636876a667cfab890b4304794f6be42d46d35befbedbe25` |
| cp14 receipt `atx-engine/reviews/2026-09-20-cross-section-ic-validation.json` | `38dd653f7069cb4935b51d003baf9cdf87401434eccc9af5df15e907b319d723` |
| cp14 receipt addendum `atx-engine/reviews/2026-09-20-cross-section-ic-validation-addendum.json` | `81e6809ceb9446164f06554cd67c71d7d99728ae1d0ba2311daccef062c28c66` |
| trial ledger `atx-engine/reviews/trial-ledger.jsonl` (2 lines) + sidecar `trial-ledger.manifest.json` | `cc888bde6b28f84b6e4860a7d2b507b2cb967022eed566fb067790f017809be7` |
| `build-equity/bin/atx-impl.exe` | `ac3ab17f1202b0802ba7889ad220d065ee18f4fbea9493b6e372120d492b7eff` |
| `build-equity/bin/atx-impl-tests.exe` (post close-out edit) | `5d8f398c0375ea483881424f5fe149ddbcd77bad9c348921298102307e63bc08` |
| `build-equity/bin/atx-engine-eval-tests.exe` | `d19f3e1ca93b2de7830626055e700e8c022cc4fef114f43af948083ce112dfd4` |
| `build-equity/bin/atx-engine-book-tests.exe` | `0012ccb68929847ad5feaffe218461b17b71c15eb5a047d36d38050e8378ac2c` |

The cp14 design SHA above MUST NOT change: it is embedded as `kDesignNoteSha256` in
`atx-impl/src/stage_equity_ic.cpp`, written into both trial-ledger lines and `request.json`,
and verified fail-closed by the runner preflight. Any further design edit requires
re-embedding the constant and is a new pre-registration.

Note: `atx-impl.exe` was built BEFORE the close-out edit to `stage_equity_ic.cpp` (§5.3); the
real-data run used the receipt-pinned source. `atx-impl-tests.exe` was rebuilt after it
(32/32). Rebuild `atx-impl` before any new real-data run.

### 2.2 Native evidence (all under `build-equity/audits/`, junit under `build-equity/`)

Checkpoint 13: `iteration13-configure.log`, `iteration13-check-t1.log`,
`iteration13-claims-state-tests{,-fix1}.log`, `iteration13-build-t1fix1.log`,
`iteration13-book-baseline-tests.log` (59/59 pre-T2), `iteration13-build-t2.log` (FAILED,
preserved), `iteration13-build-t2b.log`, `iteration13-book-t2-tests.log` (61/61),
`iteration13-build-t2fix1.log`, `iteration13-book-t2fix1-tests.log` (62/62; junit landed at
`build-equity/build-equity/iteration13-book-t2fix1.xml` because a relative `--output-junit`
resolves against the build dir — pass absolute paths).

Checkpoint 14: `iteration14-configure.log` (groups `risk;data;core;book;eval`),
`iteration14-check-t1.log`, `iteration14-build-t1.log`, `iteration14-eval-t1-tests.log`
(18/18), `iteration14-eval-group-tests.log` (100/100), `iteration14-build-t1fix1.log`
(FAILED: static_assert caught wrong sealed day count 19'723 → fixed 19'358),
`iteration14-build-t1fix2.log`, `iteration14-eval-t1fix2-tests.log` (24/24),
`iteration14-build-t2t3.log`, `iteration14-eval-t2t3-tests.log` (62/62, 41 marker lines),
`iteration14-native-comparison-v1oracle.json` (pass 41/41 but against the defective v1
oracle; not final), `iteration14-build-t2t3fix1.log`, `iteration14-eval-t2t3fix1-tests.log`
(63/63, 42 marker lines, FINAL engine), `iteration14-native-comparison-v2.json` (FINAL:
42/42), `iteration14-configure-t4.log`, `iteration14-check-t4.log` (FAILED unconfigured),
`iteration14-check-t4b.log`, `iteration14-build-t4.log` (FAILED, blocked by in-flight engine
TU), `iteration14-build-t4fix1.log` (FAILED, -Wunused-result), `iteration14-build-t4fix1b.log`,
`iteration14-impl-t4fix1b-tests.log` (16/16), `iteration14-build-t5.log`,
`iteration14-impl-t5-tests.log` (15/15), `iteration14-build-t5fix1.log`,
`iteration14-impl-t5fix1-tests.log` (16/16, FINAL stage), `iteration14-build-atx-impl.log`,
`iteration14-impl-full-wrapper.log` + `iteration14-impl-full-tests.log` (988 tests: 983 pass,
4 NOT_BUILT placeholders, 1 pre-existing failure), `iteration14-smoke-rerun.log`,
`iteration14-run-dryrun2.log`, `iteration14-equity-ic-measurement.json` (real run, attempt 1,
`accepted=false` due to a runner defect), `iteration14-equity-ic-measurement-attempt1-reeval.json`
(`accepted_reevaluated=true`), `iteration14-build-closeout.log`,
`iteration14-impl-closeout-tests.log` (32/32 after the close-out edit).

Scripts (never rerun a receipt writer; version new attempts):
`iteration14_cross_section_oracle.py` (v1, defective F3 fixtures, kept),
`iteration14_cross_section_oracle_v2.py` (+ `-v2.json`, 42 cases, current),
`iteration14_native_comparator.py` (defaults to v2), `iteration14_run_equity_ic.py`
(real-data runner; `--attempt N` versions outputs; pin bug fixed at the re-pin site),
`iteration14_reevaluate_attempt1.py`, `iteration14_write_validation_receipt.py`,
`iteration14_write_receipt_addendum.py`, `iteration13_write_validation_receipt.py`.

### 2.3 Real-data outputs

`C:/atx/data/equity_ic_training_2013_20260920/`: `ic.csv`, `ic_decay.csv`,
`quantile_spread.csv`, `coverage.csv`, `signal_autocorr.csv`, `seal.json`, `request.json`,
`manifest.json`, `ic_summary.json`. Inputs: context
`C:/atx/data/tickerhistory_training_native_20260919/context.bin`, baseline
`C:/atx/data/equity_baseline_training_2013_20260919`, audit manifest
`C:/atx/data/equity_source_reconciliation_2013_20260919/manifest.json` (34 required-mark IDs;
the stage asserts 146189/37648/35715/39970 membership at runtime).

## 3. What checkpoint 14 established (and did not)

Pre-registered (before computing, ledger line 1, `trial_id iteration14-cross-section-ic-0001`,
N=30 = 3 signals × 5 horizons × 2 forward-return variants): signals `momentum_252`,
`momentum_126`, `blend_equal` read from the published `combo.bin` (never recomputed);
horizons {1,5,10,21,63} panel rows; Q=10; bootstrap B=2000, seed 20260920, circular blocks
L_h = max(5, ceil(h/2)) → {5,5,5,11,32}; reportable iff n ≥ 20 and floor(n/L_h) ≥ 10 and
draws ≥ 1; variants DropMissingForward and IncludeAuditedTerminalV1 (only HNZ/DELL/MOLX
priced; DELL USD 0.13 only if session ≤ 2013-10-28; PCS never; 29 unclassified IDs not
flagged); `_ex34` restriction and a 126-date common-sample prefix as reported restrictions
(not new trials); cost model trade_bps = `constexpr EquityAllocationConfig{}.trade_bps` (5),
365 bps borrow ACT/365 on calendar days from session keys, gross-2.0 long/short decile book.

Result, full sample, DropMissingForward, rank IC h=1/5/10/21/63:
momentum_252 +0.029/+0.056/+0.070/+0.092/+0.146; momentum_126 +0.030/+0.052/+0.064/+0.093/
+0.154; blend_equal +0.031/+0.057/+0.071/+0.099/+0.160. Pearson-IC bootstrap lower bound > 0
from h=5 for all three (h=1 below zero for momentum_252 only). h=63 intervals null by
pre-registered prediction. Net decile spread positive at every reportable horizon. Variant
and `_ex34` move IC means by ≤ 0.0022 and spreads by ≤ 0.0008; ICIR moves up to 0.022 at
h=21 and 0.080 at h=63 (thin sample).

NOT established: anything out-of-sample; alpha acceptance; net P&L; capacity; sign stability
across years (one year, 2013, a strong momentum year); anything about the sealed 2023–2025
period (seal non-vacuous by code, vacuous by data at Stage 1). Overlapping horizons make
naive t invalid and ICIR non-comparable across h.

## 4. Rulings made today (all also in progress.md)

1. Re-plan under the alpha-priority directive: research (`research-alpha-pipeline.md`) found
   the factory path (CPCV/PBO/DSR/lockbox) has zero real-data evidence and the equity-book path
   has zero forecast evaluation. Adopted D-1 (cross-sectional forecast evaluation) as
   checkpoint 14; minimal-closed checkpoint 13; deferred cp13 T3–T5.
2. cp13 T2: all 10 implementer deviations accepted (no `marked_equities` parameter on
   `commit_cash_claim_payment`; event batch for period p requested at iteration p−1; policy
   structurally never queried outside 1 ≤ period < dates−1, dead branch deleted).
3. cp14 design rulings §11.1–§11.9 (read them; the load-bearing ones): three evidenced terminal
   events only; DELL record-date gating; net spread priced on the gross-2.0 book (w = ±1/n,
   `short_leg_gross` 1.0); L_h re-pinned pre-run to max(5, ceil(h/2)); common-sample as a scalar
   prefix (126) with any gap voiding the whole common block (reason 2); `unreportable_reason`
   0..4 lowest-nonzero-wins (1 n<20, 2 prefix gap, 3 block length, 4 draws 0); stream key
   six byte-aligned fields incl. `sample_id`; `rho_rank` drives implied turnover; calendar-day
   borrow drag; terminal leg only when no close exists at t+h (A-6); ledger at
   `atx-engine/reviews/trial-ledger.jsonl`; `--evaluation-start/-end` required; no `--config`;
   kMaxIcDates/kMaxIcInstruments = 4096; exported seal constants; `spread_reportable` fields;
   `n_terminal_unevidenced` is a measured output (expected PCS only); wall-time-only runtime
   in-process (runner samples peak WS); integral reals serialize with `.0`.
4. cp14 T4 ledger: verify cross-checks the sidecar (tail edit/deletion detectable unless both
   files are regenerated consistently — documented limit); exclusive lock via `fopen "wx"`;
   no auto-repair of a torn tail (verify reports the repair offset); declared N per checkpoint
   = SUM of pre-registered lines (conservative).
5. cp14 T5: audit-ID set resolved as `<parent of --baseline-dir>/equity_source_reconciliation_2013_20260919/manifest.json`;
   stage appends both ledger lines; every exit after pre-registration appends a terminal
   line (C-1); design SHA embedded and preflight-verified.
6. Runner false negative on attempt 1 (compared path strings, digests identical): fixed the
   runner, wrote a versioned re-evaluation, did NOT re-run (a re-run appends two ledger lines
   and declares 30 more trials for no information).
7. Process (user directive mid-turn): fewer review/test cycles; scoped re-reviews dropped for
   fix rounds; T2+T3 and T5+T6+T7b merged into single implementers; one end review per large
   unit; one final whole-branch review. Also (earlier directive): use Edit/Write tools for file
   changes, not Python workarounds; receipt writers as Python audit scripts are the accepted
   pattern.

## 5. Open items

### 5.1 Pre-existing, unrelated, open
`StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` fails with
`invalid stod argument` (CTest #944). `atx-impl/src/stage_run.cpp` (+18 uncommitted lines:
cost-bps / replay-trade-bps guard) and the test were last modified 07:34 today, before this
session; the test builds `RunConfig` in-process so cp14 wiring cannot reach it. Some report
kv is non-numeric. Not diagnosed further.

### 5.2 Deferred by ruling
cp13 T3 (engine scenarios B/C), T4 (atx-impl allocation certification), T5 (equity-book
wiring, `movements.csv`/`claims.csv`, `--transitions`). cp14 Stage 2 (2013–2019 panel;
~4.4× the 445-date context's 1,026,482,176 B peak WS against a 3 GB budget; relief via an
evaluation-only union, see `annual-panel-memory-review.md:73-75`). `MaskSealedV1` and
`reserve_window` routing untested (design §9.5). Comparator enforces only reason codes 1/2
(full 0..4 enum enforcement deferred). Trial-ledger unknown-key rejection on read (T4 D-3).

### 5.3 Close-out edits after the final review (recorded in the addendum)
`PLATFORM_PROGRESS.md` false "< 0.002" sentence replaced; `atx-engine/README.md` stale cp13
pointer replaced; `stage_equity_ic.cpp` terminal-ledger-line construction confined in a try
(residual C-1 shape). The receipts were not mutated; the addendum carries the post-edit pins.
The cp14 receipt's prose says "64 native tests" — wrong; 63 is measured and pinned (addendum
I-2). The cp13 receipt's `engine_cmake` pin and the cp14 receipt's `docs_equity_ic` pin no
longer match the files (legitimate later edits; addendum records both).

### 5.4 Deferred Minors
Enumerated with references in `final-branch-review.md` "Open items": cp13 T1 D1–D5 + F4–F12,
cp13 T2 Minors 1–11 + 2, cp14 T1 MINs + NEW-1..4, cp14 T2T3 M-1..M-10, cp14 T4 M-1..M-8,
cp14 T5 M-1..M-13 (notably `reserve_directory` misreporting I/O errors as "output root must
not exist").

## 6. How to resume

1. Read this file, then v1 §"Constraints", then `progress.md` (all of it), then
   `final-branch-review.md`.
2. Verify the pins in §2.1 with `sha256sum`; the cp12 design/receipt and the cp14 design MUST
   match exactly. Verify the trial ledger: 2 lines, statuses pre-registered/completed.
3. Native loop (parent-only, serialized, ≤ 2–3 wrapper calls per PowerShell process,
   `CCACHE_DISABLE=1`, never raw cmake/ninja):
   ```powershell
   $env:CCACHE_DISABLE='1'
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 configure -Preset equity-dev -Groups 'risk;data;core;book;eval' '-DVCPKG_MANIFEST_MODE=OFF' '-DVCPKG_INSTALLED_DIR=C:/Users/natha/vcpkg/installed' '-DFETCHCONTENT_BASE_DIR=C:/atx/.worktrees/equity-platform/deps/equity-dev'
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 check 'atx-impl\src\<file>.cpp' -Preset equity-dev
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 build atx-impl-tests -Preset equity-dev -Jobs 4
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/atx-build.ps1 -Ctest -Preset equity-dev -Jobs 1 -R '^(ConfigEquityIc|EquityIcTerminalValue|StageEquityIc|TrialLedger)\.' -VV --output-junit 'C:/atx/.worktrees/equity-platform/build-equity/<name>.xml' --output-log 'C:/atx/.worktrees/equity-platform/build-equity/audits/<name>.log'
   ```
   Traps: PowerShell 5.1 swallows ctest `-O` and `--verbose` (use `--output-log`, `-VV`);
   `check` on a new TU needs a reconfigure first ("no object target found"); a relative
   `--output-junit` resolves against the build dir; `atx-impl-tests` does NOT rebuild
   `atx-impl.exe` — build the `atx-impl` target explicitly before a real-data run;
   `-Wunused-result` under `/WX` on `Json::parse` — cast to `(void)`.
4. Engine gate: `-R '^EvalCrossSectionIc\.'` (63) then
   `python build-equity/audits/iteration14_native_comparator.py --logs <log> --out <new versioned json>`
   (expects 42/42 against oracle v2; refuses an existing `--out`).
5. Real-data run (only if a NEW pre-registration is intended; each run appends 2 ledger lines
   and declares N more trials): rebuild `atx-impl`, then
   `python build-equity/audits/iteration14_run_equity_ic.py --dry-run` then without
   `--dry-run` with `--attempt 2` (attempt-1 paths are immutable). Preflight verifies the
   embedded design SHA and refuses a stale `trial-ledger.jsonl.lock`.
6. Checkpoint 15: re-dispatch the research brief (progress.md last entry, "Checkpoint 15
   research dispatched") — Stage 2 vs D-3 vs walk-forward. Candidate order per today's
   research: Stage 2 with per-year folds (each year a restriction of the same 30
   configurations, not new trials — confirm against design §11.2 AR-7 wording before
   pre-registering), then D-3 cadence/hysteresis (`atx-impl/src/equity_allocation.cpp` and
   `stage_equity_book.cpp` were deliberately left untouched for it), then the cp13 remainder.

## 7. Agent/process conventions used today

Fresh implementer per task, edit-only (no native commands), reports to
`.superpowers/sdd/equity-platform-parent-goal/cp<N>-task-<T>-report.md`; parent builds/tests
and appends every measurement and ruling to `progress.md` before dispatching the next step;
independent read-only reviewers write `...-review.md` with `SPEC:`/`QUALITY:` verdict lines;
design writer keeps rulings inside the design as numbered `§11.x` blocks; receipts are
immutable Python-written JSON with pins, failed attempts are versioned (`-attemptN`,
`-fix1`, `-v2`), never overwritten. Agents cannot be resumed across a session restart
(SendMessage to a prior-session name fails); re-dispatch fresh with the same brief.

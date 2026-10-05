# M2h finish: pre-run list (written before the runs; root, 2026-10-04)

Head `59893e9f`. Synthetic only: the tiny world (`scripts/tests/fixtures/tiny_world.py`, seed 7), its own ledger
`trials.jsonl` inside the world root (empty, N 0). The pool trial ledger `build-equity/trials.jsonl` is never named.
Zero trials: every wave stops at `--until verify` (no judge, no record). No real data, nothing dated 2024+.

## Exes (no build)

Release tree `build-equity-rel/bin`: IC `atx-equity-strategy-ic.exe` p9-2j `fccaa07e` (built at `c2416d62`), NAV
`atx-equity-strategy-targets.exe` p9-2l `53ca5dd4` (built at `3fd0bf98`). `git diff --stat <src>..HEAD` over
`*.cpp *.hpp *.h *.cc *.ipp *.inl CMakeLists.txt *.cmake CMakePresets.json vcpkg.json`: empty for both sources, so
no build and tag `p9-2m` stays free. The Debug tree's NAV exe (p9-2d, built at `a25453be`) predates C++ changes and is
not used.

## Layout

Base world `build-equity/p9-m2h-tw` (short root: W1-I1 margin larger than the canary's `%TEMP%` root): the world as
built (Release), `tiny.json` plus a `summ` block whose `ledger` is `trials.jsonl` (wave preflight compares the parent's
`summ.ledger` with the wave's; the committed `scripts/specs/tiny.json` has no summ block), the kinds templates copied
to `scripts/specs/p9/kinds/templates/`, `git init` (the wave commits). Parent run: `research_cycle.py run tiny.json
--root <base> --stop-after nav`. Each wave runs on its own copy `build-equity/p9-m2h-tw-<label>`.

## Waves and expectations

1. `eng0`: P9-C's shape on real exes, kind `rule`, `gross_match: pm6-6-engine`, template = committed fixture
   `rule-composition.json` (keeps the parent's `--aim-leverage {leverage}`). SUBSTITUTION: P9-C names a NAV rule that is
   no leverage rule (C3's trade-rate row); the live NAV table lists only `fixed-v1` / `vol-target-v1` /
   `risk-target-v1` (C3 not merged; vol/risk-target need a risk store the tiny world lacks; fixed-v1 sets the
   leverage itself), so the non-leverage rule is the IC rule `within-theme-v1` (fit `--composition ew-theme-v6`).
   Expect: preflight passes; run stops after w (ENGINE_STOP); match writes `<cell>-cal.json` at the cell's L 1.247
   with `--calibrate-gross repr(G_parent) --calibrate-tolerance T`; the NAV exe's `summary.json` carries a complete
   `calibration` record with `leverage_requested == 1.247` and `target == G_parent` (the check at
   `wave_stage_cell.py:374`); mechanics reader G == the exe's `calibration.gross` bit for bit; if `corrected`, the cell
   of record `<cell>-gm.json` resolves to `nav.leverage` = L' to 4 dp with no `--calibrate-*` flag and its NAV argv
   carries `--aim-leverage L'`; verify passes and pins `match.record_spec_digest`. If the composition change leaves
   |G - G_parent| within tolerance, `corrected` is false and L' = L (reported as such; not a red).
2. `py` and `eng` (O1, ruling E2-16): the same rule wave on a template that sets its OWN whole `ref` block (no
   `leverage`, no `flags`: output `tiny-ref-m2h`, combined = the parent's w `train_combined.json` as
   `change.inputs.reference_combined`), under `pm6-6` and under `pm6-6-engine`. Reported: the resolved cell of record's
   effective `ref.leverage` after the match stage, and the `--aim-leverage` on the ref NAV argv(s) actually run.
   Expected from the code (re-review 4 O1): `pm6-6` -> L' (gm_doc pins no ref.leverage for a template); `pm6-6-engine`
   -> the cell's L 1.247 (record_doc / calibrated_doc pin it). If the Python run refuses the ref re-run at a new L
   (same ref output dir), that is reported as observed.
3. `pin`: fixture `new-nav-rule.json` shape (`fixed-v1`, `aim_leverage` 1.5) under `pm6-6-engine`. Expect a stop
   with exit 3 (EXIT_PIN): either at match (calibration ran at L 1.5, not the cell's L 1.247: the `:374` check) or
   earlier at preflight; whichever, reported verbatim.
4. W1-I1 margins (re-review 2 items 6 and 11): `cycle.phases.deepest_cache_path` over every committed cell spec with
   an IC pass at pool-2 (longest library id per spec, Release and audit-exact layouts); expected worst
   `scripts/specs/v70-lo3.json` 249 (margin 1 to 250); plus the margin of each tiny-world root used here.

Not probeable here: C3's real trade-rate rows (C3 not merged); noted, not run.

## Amendment after attempt 1 (written before attempt 2)

Attempt 1 (logs `m2h2-tw-base-attempt1.log`, `m2h2-wave-eng0-attempt1.log`; roots deleted):
- The base world as a plain `git init` root failed at the parent's u pass: in git mode the bounded runner runs in
  ITS worktree (`run_bounded_research.py` `--root` defaults to the script's `parents[1]`, cwd pool-2), so the IC exe
  could not open `tiny_world_v1.json`. The base is now repository-shaped: a `git archive` of HEAD's `scripts`,
  `atx-engine/tools`, `atx-impl/tools`, `atx-impl/strategies` extracted into the world root and committed there, and
  every script (cycle, wave driver, runner, fitter, readers) runs from that byte copy; tiny.json's script paths are
  relative to the world root.
- `eng0` ran preflight .. match on real exes but `within-theme-v1` is a no-op on the tiny world (one admitted member per
  theme: weights bit-identical to the parent's), so G = G_parent exactly and `corrected` false. Every other IC rule
  needs a standardised composition, which the tiny world refuses (fitter: "member cap 1/(2T) ... no other theme can
  take the excess of theme earnings_momentum"; probed for theme-erc, theme-resid, theme-tsmom, mom-volman,
  theme-cov-hedge, ew-theme-std, ic-shrink, ic-shrink-aim).
- Verify stopped on (i) mechanics `gross_all_rows` 1.0972511252536685 outside [0.9, 1.05] (the tiny world's gross at
  L 1.247) and (ii) two date tokens that are the mechanics readers' own `"started_utc"` stamps.

Changes for attempt 2 (synthetic world only; no committed file edited):
- Parent `nav.leverage` 1.12 in the world's own tiny.json copy (gross about linear in L: about 0.985, inside the band).
- The gross mismatch comes from a FREE NAV flag constant `rule_cell.constants.flags nav --trade-fraction .10` (parent
  .05): a trading-rate change, the mechanism P9-C's trade-rate row would use; the named rule stays `within-theme-v1`
  (kind `rule`). `--trade-fraction` is not a flag of the live NAV table (table flags: --aim-leverage, --risk-model,
  --risk-model-sha256, --risk-target, --risk-target-bias, --risk-target-cadence, --vol-target).
- Seal tokens: per SEAL-ALLOW (b), every hit is checked to be a `"started_utc"` stamp line (each hit printed); only then
  the wave resumes with `--seal-allow <date>=SEAL-ALLOW`; any other hit stops.
Expectations otherwise unchanged (items 1-3 above, L = 1.12 in place of 1.247).

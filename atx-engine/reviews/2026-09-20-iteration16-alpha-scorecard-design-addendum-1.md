# Checkpoint 16 design — addendum 1 (measure-first deviations, 2026-09-20)

Frozen design: `2026-09-20-iteration16-alpha-scorecard-design.md`, SHA-256
`a83484037cae527e2fbfba5dce04289742d1c81666b2c03c132f3da9268c04d1` (unchanged by this addendum).
This addendum records two code deviations from design §4 ("three panel-side edits") that the
measure-first cell (2015, top-3000) forced BEFORE any scorecard number existed, plus the
measured memory figure. Both are parent rulings in `progress.md` (R16-24, R16-25). Neither
changes the pre-registered recipe (§2), the bars (§3) or the ledger accounting (§8).

## A1. Baseline recipe pin widened (R16-24)

`atx-impl/src/stage_equity_baseline.cpp` `require_context_recipe` pinned the checkpoint-14
screen values (`top_n_by_adv 1000`, `min_adv_usd 20e6`, `min_raw_price_exclusive 5.0`) and
therefore refused every cp16 context ("unsupported context field/universe recipe"). It now
accepts EITHER that cp14 screen OR the cp16 membership recipe, recognised by the context recipe
carrying `membership_rule == "year-union-plus-last-prior-rebalance;not-as-of"`,
`universe_membership_sha256`, `universe_cut`, and the PIT floors (`top_n_by_adv 0`,
`min_adv_usd 0`, `min_raw_price_exclusive 1.0`). Nothing in between is accepted. The cp14 path
is byte-identical. This drifts the cp13/cp14 receipts' source pin on
`stage_equity_baseline.cpp` (legitimate later edit; receipts untouched).

## A2. Signals-only baseline commit for membership contexts (R16-25)

With the pin widened, the baseline built its signal panel but its book replay refused the run
(`replay: missing/nonpositive required close at period=3 … security_id=34865`,
2015-01-07): the PIT top-3000 admits thin names whose closes are missing on some sessions, and
the replay's `held_missing_price_policy` is `reject-entire-run`. The downstream `equity-ic`
stage consumes only `evaluation.bin` + `combo.bin` and their parentage
(`stage_equity_ic.cpp:862-874`), never the books or the replay report. For membership
contexts the baseline therefore stops after `combo.bin` and commits a manifest with
`status = "complete-signals-only"`, `qualification = "not-attempted"`, recipe keys
`membership_mode = true`, `replay = "skipped-membership-context-signals-only-no-book-result"`,
and parents `source-context / evaluation / combo` only. These directories are NOT book
baselines and must never be cited as one. cp14-screen contexts still run the full replay.

## A3. Measured memory (design §7, check 1)

Panel 2015 top-3000: 510 dates × 3,608 instruments (allow-list 3,608 = the predicted
"+1 prior" union; cap 4,096 respected), wall 82.7 s, peak working set 2,611,576,832 B
(2.61 GB) — under the 3 GB budget but far above the §7 extrapolation of ≈ 1.46 GB: the peak is
set by the ≈ 9k source columns × 510 dates before compaction, not by K. Baseline signals-only
17.8 s / 383 MB; equity-ic 118.0 s / 383 MB; `ic.csv` 13,920 rows, all emitted.

## A4. Process deviations (harness only)

Runner `iteration16_run_cells.py` was patched by the parent (receipt tag for partial runs;
phase logs moved under `build-equity/audits/iteration16-cells-logs/`; only the panel dir is
pre-created because the baseline/ic stages demand a fresh root; `--resume` skips a phase whose
marker file exists). Failed attempts are versioned (`*_failed1..3`, `*_failed2/3.log`).
Two dead launches (unknown flag; missing argument) wrote nothing.

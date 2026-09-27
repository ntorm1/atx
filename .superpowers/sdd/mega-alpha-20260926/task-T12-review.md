# Task T12 review: role factor-break repair and QA scan (rule `factor-break-v1`)

Reviewer: t12-review, 2026-09-27. Read-only. Package `review-T12.diff` (root commit 99421a5f, 2 new files, +896).
Line numbers below are **file** lines: tool = `atx-impl/tools/repair_role_factor_breaks.py`, test =
`atx-impl/tools/test_repair_role_factor_breaks.py`.

## Spec Compliance

- ✅ Spec compliant. Every requirement from the dispatch is met:
  - **Declared first-principles rule.** Constants are at tool:70-78 and the rationale at tool:39-48. It is justified from dividends and splits, not from strategy outcomes. The rule is recorded in the manifest (`rule_statement`, `parameters`, tool:466).
  - **New exclusive role directory.** The tool checks the output does not exist (tool:505), creates it with `mkdir(exist_ok=False)` (tool:431), opens every file with `xb` (tool:379-383), and publishes the manifest last by hard link (tool:412-420).
  - **Only `close.f64` changes.** The other six payloads are re-hashed from disk and must equal the source (tool:438-442). Cells outside a repaired name's pre-break rows are gated unchanged (tool:291-305).
  - **Exact rescale.** For each repaired name, `close[0:t] *= k` (tool:274-283). The return-error gate is ≤ 1e-12 (tool:299-302).
  - **Genuine same-day actions are kept.** Split-follow and small-increase classes are kept (tool:248-252). The one exception is a declared residual (see Minor 2).
  - **`repair` block, `--scan-only` mode, and RSS < 1 GB.** The block is at tool:463-481 and the scan mode at tool:512-515. The real run peaked at 312 MiB.
  - **All five fixtures requested by the brief are present and assert real bytes:**
    - mass break repaired exactly: test:157
    - genuine split and dividend untouched: test:187
    - deterministic bytes: test:232
    - exclusive output: test:240
    - SHA refusal: test:252
  - **Downstream consequences.** Report §6 covers `shares_out` (confirmed below), the regeneration cascade and the command order.
- **One deliberate deviation, declared and justified.** The brief's cell condition is used as the mass *detector* only. The *repair set* is sign- and raw-follow-based (report §2.2), so that opposing-sign and sub-1% artifacts are also removed. I agree with it: on TRAIN it repaired 2114 steps against 1185 jump cells, of which 712 have |s| < 0.01. None of them is a genuine action by construction (see the check in the next section).
- ⚠️ **Cannot verify from the diff:**
  1. **C++ loader acceptance.** No run shows the C++ loader (`strategy_data.cpp`) actually loading the v2 role. Reading `strategy_data.cpp:75-140`, it reads only named keys (so the `repair` key is ignored) and only the files named in `files` (so `repair_cells.csv` is ignored). The manifest is 22.8 KB, under the 1 MiB limit at :76. It should load, but root should confirm with the first IC run on v2.
  2. **Level-sensitive `close` in the v3 library.** Check whether any v3 library subalpha uses `close` as a *level*: a cross-sectional rank or threshold of `close`, or `1/close`. Such an expression leaks future corporate actions with or without T12 (see the look-ahead section).
  3. **Validation-scan hygiene.** Root ran the validation scan, which computes adjusted/raw log moves on 2023-2024 data. It is acceptable as data QA only if nothing from it (for example the 47) feeds back into rule parameters. See Important 1 for how to keep it that way.

## Global-constraint checks (look-ahead, threshold provenance, margin)

**Real run used the reviewed code.** In the v2 manifest, `repair.tool.code_git_blob_sha1 = 4f5502a2…` equals the diff's blob (`index 00000000..4f5502a2`). The real TRAIN manifest also shows:

| Field | Value |
|---|---|
| repaired | 2114 (2062 decreases, 52 increases) |
| kept_split_follow | 2 |
| kept_distribution | 7 |
| kept_gap | 0 |
| post_repair_jump_cells | 1 |
| max return error | 2.33e-15 |
| changed cells | 1,324,464 (about 2114 names × about 626 pre-break sessions) |

**Kept and repaired steps are cleanly separated on TRAIN.** From the `repair_cells.csv` sidecar:

- The repaired increases have minimum k = 1.95.
- The kept_distribution steps have maximum k = 1.030.
- Both kept_split_follow steps are exact consolidations whose raw close followed: 38366 (k = 1/7, raw ×8.7) and 111028 (k = 1/6, raw ×5.5).
- No repaired step has |raw move| > 0.2. The largest raw-up repaired decreases are 7 names at +10..+20% raw with k between 0.92 and 0.997. That is consistent with dividend artifacts on an up day, not consolidations.

**Look-ahead: none is introduced in returns. Levels inherit the leak the 2026 snapshot already has.**

- **Pre-break returns.** For any decision time τ < t, every return built from rows ≤ τ is a ratio of two cells scaled by the same k. It is unchanged up to about 1 ulp (≤ ~2e-16 relative). Pre-break features are therefore not bit-identical, but no information moves across time. The IC cache is keyed on the role SHA, so it recomputes anyway.
- **The break return.** The repaired return on the break session equals the raw return from p to t, which is observable at t using only those two prices.
- **Levels.** k encodes the vendor's post-2021 corporate actions, which are future information, so repaired pre-break *levels* carry it. The unrepaired post-break block already carried the same k from 2021-01-04 onward, and the pre-break block carried actions up to 2020-12-31. Backward-adjusted levels from a 2026 snapshot leak future actions everywhere. T12 makes the pre-break level basis match the post-break one; it does not add a new kind of leak.
- **Does this matter for a backtest deciding on prices ≤ t?**
  - Not for return-based features, and not for NAV P&L on adjusted returns, which the repair fixes.
  - Not for liquidity or membership. Dollar volume and vwap use `raw_close` under the default `RawDailyCloseV2` (`datafields.hpp:201-209`, `real_panel.hpp:169`). Membership uses raw price and raw ADV (`strategy_data.cpp:110-112`), and `member.u8` is byte-identical.
  - It matters only for a level-sensitive use of adjusted `close`, which was already contaminated (⚠️ 2).

**The thresholds were not tuned on validation.**

- The constants are committed in 99421a5f, the blob recorded in the real run.
- The report says the implementer read no role payload.
- The real-data inputs to the rationale are TRAIN-side: root's 1185 / 105 / 1073 counts, nav_recon section E on the TRAIN role, and VA1's existence ruling C-35, which supplies no count threshold.
- The validation numbers (47) arrived after the rule was fixed.

**Margin of the 50-cell threshold. It is weak on the legitimate side.**

- TRAIN non-mass jump counts from `detector_jump_cells_by_session`: 44 (2022-12-29), 33, 30, 25, 22. Validation: 47 (2024-03-27).
- 50 sits 1.14× above the TRAIN legitimate maximum, 1.06× above validation's (3 cells of headroom), and 24× below the break (1185). The geometric midpoint of 47 and 1185 is about 236, so the placement is very asymmetric.
- **A lower threshold is strictly worse.** Any value ≤ 47 already trips on both roles. On a false MASS the genuine dividends are kept (s > 0 is `kept_distribution`), so they remain jump cells. The post-repair gate (tool:306-308) then refuses publication. That failure is closed, but it blocks the role.
- **A higher absolute threshold** (about 200-250) would give about 4-5× on both sides for these two roles. It is still an absolute count, though. It scales with universe size and dividend seasonality, and it misses a partial re-anchoring of fewer than about 200 names.
- **A different detector is the robust fix.** Count unexplained factor steps, which is exactly the `repaired` predicate the classifier already uses (tool:248-252): `s < -NOISE` without raw follow, plus `s > ln 1.25` without raw follow.
  - Under this vendor convention, a genuine dividend, spin-off or forward split raises the factor. Only a consolidation lowers it, and there the raw close follows. The legitimate baseline is therefore structurally about 0; VA1 / C-35 treats every non-split decrease as an artifact.
  - On 2021-01-04 this count is 2114.
  - Express the threshold as a fraction of steps (for example ≥ 1% of steps and ≥ 20), derived from that structural baseline and not from 44 or 47.
  - Root can sanity-check the baseline without new runs: the existing scan output prints the `dec` column for the top-N rows. Look at 2022-12-29 (TRAIN) and 2024-03-27 (validation); `dec` should be about 0-3 there.

## Strengths

- **Correct, well-argued mechanism.** The report traces the defect to the vendor block boundary and cross-checks it against the independent atx-db VA1 / C-35 analysis. It finds a real gap in VA1: increases from later consolidations are unrepaired (report §1.3).
- **Minimal repair that is fully gated before publication:**
  - loader-contract check (tool:289-290)
  - per-step return error ≤ 1e-12 (tool:299-302)
  - no change outside the pre-break rows of repaired names (tool:303-305)
  - no mass session left (tool:306-308)
  - six unchanged payloads re-hashed from disk against the source (tool:438-442)
  - manifest published last by exclusive hard link (tool:412-420)
- **Multi-session handling is correct.** k for a later session is computed from source cells at p2 ≥ t1, which are never modified by the earlier repair. Deduplication by (j, t) is sound because p is determined by (b, t). This is exercised by test:336.
- **Strong provenance.** The manifest records the source role SHA and close SHA, the tool SHA and git blob, the rule text and parameters, per-session class counts, the per-session detector vector, and the SHA-bound sidecar with every crossing step, kept and repaired (tool:463-481). `--expect-sessions` and `--role-sha256` are required even for the scan (tool:490-491, 517-522).
- **Tests use real bytes, not mocks.** For example:
  - `close' / raw == k` to 2e-15 and post-break rows bit-identical (test:168-171)
  - untouched columns bit-identical (test:172-173)
  - the reloaded output binds by its own SHA (test:227-228)
  - the scan's big/small columns checked against a literal nav_recon section E port (test:283-294). I checked it against `nav_recon.py:171-186`: the conditions are identical.
- **Budget.** 2.45 s and 312 MiB on the real TRAIN role.

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

1. **The mass detector's legitimate-side margin is about 6%, and the stated ≥ 2× rationale is falsified by real data** (tool:72, rationale tool:43-44, report §2.1 lines 107-108; post-repair gate tool:306-308).
   - **What is wrong.** 50 is an absolute jump-cell count. The legitimate quarter-end dividend peaks are 44 (TRAIN) and 47 (validation). The docstring and report claim 50 is "at least 2x above a pessimistic legitimate peak".
   - **Why it matters.** The QA verdict on the validation role, CLEAN, rests on 3 cells of headroom. On any future role, a larger universe or a strong up-day on an ex-dividend cluster yields a false MASS. The same detector drives the post-repair gate, so the tool then refuses to publish. A partial re-anchoring below 50 cells is missed. The first failure mode is closed, not silent corruption, and the current outputs are right, so this does **not** invalidate the TRAIN v2 role (210fff96…).
   - **Fix** (as rule `factor-break-v2` for future roles only):
     1. Detect mass sessions by the count of unexplained factor steps (the existing `repaired` predicate), thresholded as a fraction of steps. Derive the threshold from the structural baseline of about 0, not from 44 or 47.
     2. Gate post-repair on "no unexplained step remains on a repaired session" instead of the jump count.
     3. Correct the docstring and report margin claim.
     4. Add a fixture: a legitimate ≥ 50-cell dividend cluster with no artifact must scan CLEAN under v2. Under v1 it refuses at tool:308, which is currently untested.
   - **Keep TRAIN v2 as it is.** Its v1 provenance is honest and its close bytes would not change. Keep the validation role's disposition justified by construction: its window starts 2021-06-01, after the only known break. Do not re-decide it with numbers seen on validation.

### Minor (Nice to Have)

1. **The `kept_distribution` premise ignores combined later actions** (tool:250-252, tool:31-33). The docstring says the artifact raises the factor only for a later consolidation of ≥ 1.25×. A later consolidation times later dividends or a later forward split can net to k in (1, 1.25), which would be kept as a distribution.
   - TRAIN is empirically clean: repaired increases have minimum k 1.95 and kept distributions maximum 1.030.
   - One kept row is questionable: 35139, k 1.030, raw +0.58%, adjusted +3.55%. A genuine 3% ex-date would normally show a raw drop. Root could check it against the vendor `returnFactor`.
   - Document the premise's limit.
2. **Declared residual: a genuine same-day dividend combined with an artifact is repaired as a whole** (report §2.3). For those names the brief's "genuine same-day corporate actions untouched" is not met. The effect is at most about the dividend yield on one day. It cannot be separated with close/raw alone; the exact route is a rebuild from `returnFactor`. There is no fixture for this residual.
3. **Internal inconsistency in the docstring.** tool:40 says "far below any split (>= ln 1.2)", while tool:45-46 says 1.25 (5:4) is "the smallest common split". 6-for-5 exists. The classification is unaffected, because increases below ln 1.25 are kept and decreases need a raw follow, but the docstring text is wrong.
4. **Exception coverage** (tool:531). A malformed manifest raises `TypeError` or `AttributeError` (for example `int(None)` at tool:166), which escapes as a traceback with exit 1, outside the documented codes 0/2/3. Nothing is written, because loading precedes writing. Add `TypeError` to the catch.
5. **`close_basis` kept verbatim** (tool:472-474, decision 3). The string now describes a basis the file no longer strictly has. This is disclosed in `repair.close_basis_note` and bound by the role SHA. A future C++ change could accept a repaired-basis string.

## Downstream (for the controller)

- **`shares_out` concern: confirmed.** `prepare_research_fields.py:709` builds `q[src] × crf[t]`, where `q = shares / factor(lag row)` (:644) and the caveat at :149 applies. For TRAIN sessions from 2021-01-04 until the 90-day lag row passes the break (about 62 sessions), `shares_out` is off by k: ×10..×80 for later-consolidation names and ×1/4..×1/10 for later-forward-split names. Route this before any TRAIN swap-fin tier flag or size/SI-ratio alpha.
- **Regeneration cascade.** Everything bound to the TRAIN role SHA must be rebuilt on v2 (report §6.2): the IC run, orientations, blends, weights and NAV. Pre-2021 TRAIN statistics will replicate to about 1e-15, not bit-exactly.

## Assessment

**Task quality:** Needs fixes. The fix is scoped to the detector for future roles; it is non-blocking for consuming TRAIN v2 (210fff96…).

**Reasoning:** The repair is exact, local, well gated, fully provenanced and introduces no return look-ahead. The real TRAIN run is internally consistent: 2114 repaired, 9 plausibly genuine steps kept, error 2.3e-15. The QA detector's absolute 50-cell threshold, however, clears legitimate dividend clusters by only 3-6 cells, against a documented ≥ 2× margin. It should be replaced by the structurally near-zero-baseline "unexplained factor step" count before the tool gates any new role.

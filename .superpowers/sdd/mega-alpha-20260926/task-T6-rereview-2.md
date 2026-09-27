# Task T6 re-review, fix round 2 (0c212388..b723d487)

Package `review-T6-fix2.diff`: 1 commit (`b723d487`), 2 files (+430 / -21). P = `atx-engine/tools/prepare_research_fields.py` at b723d487 (blob `a17f83c6`, equal to the report and to `code_git_blob_sha1` in both real fields-v2 manifests). T = `atx-engine/tools/test_prepare_research_fields.py` at b723d487. The report names the covering suite (19 tests OK, log sha given); not re-run. Real-data checks below are read-only reads of already-published TRAIN artifacts (role v2, `...-v1-fields-v1`, `...-v2-fields-v2`) plus the two published fields-v2 manifests; no producer run, nothing computed on validation matrices.

### Finding Verdicts

**1. shares_out restated with the unchained vendor factor across 2021-01-04 (T12 report §6.1), ~62 TRAIN sessions corrupted** — ADDRESSED.
- Detection is a faithful port of factor-break-v1 (P:590-648 vs T12 `repair_role_factor_breaks.py` 2b00345b:198-271): same constants (P:84-90), same jump-cell test, same classification order (noise, gap, follow, distribution, repaired), same (j, t) dedup. T6 takes s = ln f_t - ln f_p directly where T12 takes s = a - r; this differs only at ulp level. Real TRAIN counts equal T12's role block exactly (jump 1185, crossing 2123, repaired 2114, follow 2, distribution 7, gap 0).
- The correction divides k of every repaired step with lag < t <= session (P:832-838). Only `FB_REPAIRED` steps are divided out, so kept_split_follow and kept_distribution stay in the ratio, and off-mass genuine splits are never touched. Lag and session rows are both observations (P:739-744, 759-761), so L < t implies L <= p and the division is exact.
- **Empirical, on TRAIN:** v1-fields-v1 and v2-fields-v2 `shares_out` differ in exactly 130,018 finite cells, which equals the manifest's `restated_cells`. All of them fall in 62 sessions, 2021-01-04..2021-04-01. v2/v1 ranges from 0.0046 to 25.2 (median 1.015): the x10..x216 consolidation and x1/4..x1/25 split artifacts are undone and dividend payers move about 1.5%. Every other finite cell is bit-identical, so the change to the observation contract (P:739-742) had no side effect on TRAIN. The only other change is the 25,411 cells NaN'd by the new domain rule.
- **Point in time.**
  - The mass session b reads rows <= b. Classifying (p, t) reads rows p and t only (P:618-632).
  - The correction starts at the step end t (`rt <= day`, P:833), not at b.
  - The first corrected session is 2021-01-04 itself. The repair uses the 2021-01-04 row, which is known at the 22:00 mark, before the 23:00 decision.
  - The kept_gap NaN window follows the same rule (P:836).
  - The truncation fixture pins this: a role ending 2021-01-05 is byte-identical to the full role on common sessions, although a step ending 2021-01-06 exists in the full run (T:764-767).
- **Fail-closed binding.**
  - The check sits at P:769-773: the mass sessions strictly inside the role (first < b <= last) must equal the role's `repair.mass_sessions[].session`.
  - It raises before any `shares_out` byte or manifest is written. The producer publishes last (P:1117, 1197).
  - Tests: an unrepaired role and a wrong repair list are both refused with no manifest (T:778-782). A role starting on the break is accepted (T:769-776).
  - Real runs: TRAIN v2 is inside_role true and matches `["2021-01-04"]`. Validation v1 detects the break pre-role (2006 repaired), so no repair block is required.
- Validation `restated_cells` = 0 is consistent: validation-scored sessions (>= 2023) cannot have a lag row before 2021-01-04 (90 + 400 days). The warmup only has one if a line has no valid share row for 2+ months.

**2. Open Minor M3 (rereview-1): root ruled shares_out outside [1e5, 5e10] -> NaN, counted** — ADDRESSED.
- The domain is `SHARES_OUT_DOMAIN = (1e5, 5e10)` (P:79), compared in float64 and inclusive (P:849). Out-of-domain values become NaN and are never clamped (P:854).
- It is applied after the C-81 and factor-break rules, and `restated` is counted after the domain (P:855).
- `plausibility` carries the same keys as IV (P:866-873). Fixture lines 1061 (5e4) and 1062 (6e10, a non-member for 3 sessions) pin the all-cell and member counts (T:745-749).
- The real manifests show below_min 25,411 and above_max 0 on TRAIN (10,420 member), and 14,455 below and 0 above on validation (7,180 member).

### Root question: is ~0.3% of member cells below 1e5 plausible, or a units problem?

**It is a units problem.** These are not genuine small share counts; they are vendor counts about 1000x too small (the thousands field carrying millions). Measured on TRAIN only:

- **Scale.** 10,420 member cells in 105 lines. All of them were already finite in v1-fields-v1 (the domain NaN'd pre-existing values; the factor-break fix created none).
- **Turnover.** Members need ADV > $5M and price > $5 (role `membership_recipe`). Daily volume/shares_out for the NaN'd member cells:
  - median 108x per day; 95.8% above 1x per day and 80.2% above 10x per day;
  - against a median of 0.0078 per day for in-domain member cells.
  - Example: id 5405356 shows 4,000 shares at $38 with $78M/day traded.
- **Structure.**
  - 7,447 cells sit in lines with no in-domain value at all: whole-line mis-scaled.
  - 734 cells sit in lines whose in-domain values are 100x or more higher, typically ~1000x (for example 5880595 at 7.6e4 vs 9.9e7, 6667173 at 3.5e4 vs 1.0e8, 7058850 at 3.4e4 vs 4.0e7). These are in-line unit breaks, often in the first months after listing.
  - 2,239 cells are other.
- **Why A9/C-81 misses it.** They only test thousands against units above 1e8; a millions-coded row sits below that ceiling.
- **Implication: the lower bound is a partial guard.** The same defect on a line whose true count exceeds ~1e8 lands inside [1e5, 5e10] and passes. TRAIN v2 in-domain member cells:
  - daily turnover above 10x: 1,381 cells in 103 lines;
  - per-line median turnover above 5x: 10 lines, 1,275 cells;
  - `si_shares/shares_out` above 1: 6,851 cells in 130 lines; above 10: 3,258 cells in 48 lines;
  - of the 1,381 cells above 10x turnover, 1,226 have an SI ratio above 1.
- **Why it matters for T10.** These feed the swap-fin-v1 mcap < $1bn and SI/shares > 10% tier flags directly. Some ETFs genuinely exceed 100% SI, but not 10x.
- **Recommendation.** Before T10 freezes its tiers, root should declare one more guard, for example NaN when trailing volume/shares_out exceeds a declared bound, or when si_shares/shares_out > 1-2, with the cells counted. This is a vendor data defect, not a T6 implementation error, so it does not block this round.

### New Breakage in the Fix Diff

No Critical or Important.

- **N1 (Minor): the ported detector is v1, with the thin margin T12's Important flagged** (P:87, P:615).
  - The producer's own validation manifest shows `max_non_mass_jump_cells` 47 on 2024-03-27, inside the role: 3 cells of headroom.
  - A legitimate ex-dividend cluster of 50 or more inside a future or larger role makes the gate refuse a correct role (fail closed, not silent).
  - The gate would also refuse a role repaired by T12's new `factor-break-v2` that lists a sub-50-cell re-anchoring.
  - Before binding any v2-repaired role, port v2's unexplained-step detector, keyed to `repair.rule`.
- **N2 (Minor): the gate binds session names only** (P:770-771).
  - It checks neither `repair.rule` nor the per-session repaired/crossing counts.
  - A population drift between the producer and the role's close repair (a different observation contract) would pass silently. Root matched 2114 = 2114 by hand this time.
  - Optional hardening: require the inside-role `repaired` count to equal the role's v1 `mass_sessions[].repaired`.

### Out-of-Scope Observations

- **The upper bound is inert on current data** (above_max 0 in both roles). The validation member maximum is 3.12e10, above the largest real US line (~24.6B NVDA; `bodies_0327.py:114`). The 5e10 cap cannot catch an over-restatement of 2x or less. I did not investigate this on validation, per selection hygiene.
- **TRAIN peak RSS is 635 MiB against `--max-rss-mib 700`** (91%). A larger role would refuse at budget admit, which is fail closed.
- Earlier new Minors N1-N3 from rereview-1 (IV `min_effective`, `vintage_safe_from` wording, and "every field known by 22:00" wording) are unchanged. They were outside this round's findings.

### Verdict

**Fix round:** All findings addressed, no new Critical/Important breakage (2 findings ADDRESSED, 0 NOT ADDRESSED; 2 new Minors). The [1e5, 5e10] bound is catching a vendor x1000 units defect, and it catches that defect only partly. Root should declare a turnover or SI-ratio guard before T10 tiers are measured.

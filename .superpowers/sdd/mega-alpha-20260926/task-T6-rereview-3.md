# Task T6 re-review, fix round 3 (7ec09cef..be3deb3f)

Package `review-T6-fix3.diff`: 1 commit (`be3deb3f`), 2 files (+316 / -67).

- P = `atx-engine/tools/prepare_research_fields.py` at be3deb3f. Its blob is `f5e38b97`, which equals the report and `code_git_blob_sha1` in both fields-v3 manifests.
- T = `atx-engine/tools/test_prepare_research_fields.py` at be3deb3f.
- The report names the covering suite (22 tests OK, log sha given). I did not re-run it.

Real-data checks were read-only and limited to:

- the published TRAIN role v2, `...-v2-fields-v2`, `...-v2-fields-v3` and `...-v1-fields-v1` payloads;
- the lake `line_types` table;
- the validation manifests (manifest only; nothing was computed on validation matrices).

Scripts are in my scratchpad: `t6r3_train.py`, `t6r3_train2.py`.

### Finding Verdicts

**1. Root ruling: shares_out becomes NaN when (a) the trailing-21-session median volume exceeds 1.0 x shares_out (PIT), or (b) si_shares/shares_out exceeds 1.5 (si_shares visible at t). Only shares_out is NaN'd, and each rule is counted (source: rereview-2 units problem).** ADDRESSED.

- **Order.** The rules run after the factor-break, gap, C-81 and domain rules (P:927-933). `finite` is re-taken after the domain NaNs are applied (P:943).
- **(a)** P:936-946.
  - u = volume/f, with f = close/raw on present days only.
  - The window is a 21-slot ring that includes t (P:939). Its median is exact (P:488-494).
  - Evaluation needs `seen >= 11` and a finite median x f_t (P:944). The comparison is strict `>` (P:946).
- **(b)** P:949-953. It reads row t of this run's own `si_shares.f64` (size-checked at P:893-895). The comparison is strict `>`.
- **Scope of the NaN.** Only `shares_out` is NaN'd. `si_shares` is untouched: T:914-916, and the TRAIN si_shares sha is identical in v2 and v3.
- **Counting.** Each rule is counted: `turnover.to_nan[_member]`, `si_ratio.to_nan[_member]` (b-only) and `also_above_si_ratio` (P:954-960, 981-996).
- **Point in time.**
  - Row t reads role rows <= t and si row t, which is strictly before the session.
  - volume_t is known at the 22h mark, before the 23h decision.
  - The truncation fixture (T:918-923) pins it.
- **Binding.** The role streams are sha-verified against the role manifest before publication (P:445-485, 967), and a test covers this (T:578).
- **Independent recomputation on TRAIN.** I recomputed both rules with a pandas rolling median (window 21, min_periods 11), f = close/raw, and si_shares from the published v3 field. The result reproduces the dropped set exactly (15,341 cells; `array_equal` true) and every manifest count:
  - turnover: 12,988 cells (9,961 member);
  - SI only: 2,353 (1,405 member);
  - overlap: 4,819 (3,857 member);
  - not evaluable: 43,503.
- **v3 is v2 with cells NaN'd.** Every finite v3 cell is bit-equal to v2, and v3's finite cells are a subset of v2's.
- **Consistency with the re-review-2 estimates (TRAIN in-domain member cells):**
  - SI ratio above 1.5: 5,262 cells, all caught (1,405 + 3,857 = 5,262 exactly).
  - Daily turnover above 10x: 1,381 cells, 1,288 caught. The other 93 are isolated spike days that a 21-session median correctly ignores.
  - SI ratio between 1 and 1.5 is kept by design.
  - The ~105 below-domain lines are unchanged: `member_below_min` is still 10,420, and `below_min` is still 25,411.
  - Score-window coverage fell from .9934 to .9895 (8,885 member cells), which matches the manifest.
  - `factor_break.restated_cells` fell from 130,018 to 129,588. The 430 difference is exactly the restated cells that the units rules then removed (checked against v1-fields-v1).
- **Validation (manifest only).**
  - Manifest sha `699ee8d2` matches.
  - Every other field's sha equals fields-v2.
  - Totals reconcile: 14,455 + 11,917 + 1,917 = 28,289, and member 7,180 + 8,307 + 915 = 16,402.
  - Rates are of the same magnitude as TRAIN.

### Implementer decisions (root asked for a judgement)

1. **Volume restated by the role close/raw ratio: ACCEPT.**
   - The role's `close_basis` is `f64(raw-f32-close)*f64-cumulReturnFactor`. On TRAIN v2 it is the T12-repaired chain, so f_t/f_d is the same corporate-action ratio that the shares_out restatement uses (the same factor-break-v1 population, 2114 = 2114).
   - Fixture 1065 pins the 1:10 case, and the mutation sweep kills the no-conversion variant.
   - Dividend drift over 21 sessions is immaterial at 1.0x.
2. **At least 11 of 21 present days, else kept and counted not evaluable: ACCEPT.**
   - On TRAIN it affects **0 member cells**, because members need a complete prior-63 window.
   - All 43,503 not-evaluable cells are non-members, 40,730 of them in the first 10 warm-up rows.
   - It changes no tier and no score.
3. **`implausible_to_nan` widened to total all rules: ACCEPT.**
   - No code reads it. The C++ `field_definition` reads only min/max/inclusive/rule (`atx-impl/src/strategy_ic_runner.cpp:628-631`).
   - The per-rule keys keep every part, the `rule` text states the widening, and the totals reconcile in both roles.
4. **shares_out now requires si_shares in the same run: ACCEPT.**
   - The ruling says "si_shares visible at t", and this run's si_shares field is exactly that contract (strict available_at < session, 45-day staleness).
   - The FINRA group runs before the TH group (P:1253-1262), and the si writer is fsynced and closed.
   - The refusal happens before `mkdir` (P:1236-1239), `depends_on` is recorded (P:1293), and the root commands already list si_shares.
   - Where si_shares is NaN, rule (b) is silently inapplicable; see Minor N2.
5. **Strict `>` in both rules: ACCEPT.** This matches the ruling's wording, and T:895-898 pins it.

### New Breakage in the Fix Diff

There is nothing Critical or Important in the implementation.

- **N1 (Minor).** `factor_break.restated_cells` and `restated_member_cells` are now counted after the units rules (P:961). TRAIN went from 130,018 to 129,588.
  - The meaning ("restated cells that were published") is unchanged, but the block's `use` text does not say that the count is taken after the units rules.
  - Anyone diffing v2 against v3 will see an unexplained difference of 430.
- **N2 (Minor).** Rule (b) has no not-evaluable count for cells where si_shares is NaN (no FINRA row, or older than 45 days). This is asymmetric with (a)'s `not_evaluable_cells`, and so (b)'s effective coverage is not visible in the manifest.

### Root-ruling consequence (not a fix defect; Important for root's decision before T10 freezes)

**Yes, a large legitimate class is swept up: leveraged, inverse and volatility ETPs, plus XRT.**

The implementation is exact to the ruling. The false positives come from the declared 1.0x/1.5x thresholds. All figures are TRAIN member cells.

- **How I split them.** For each line I compared the dropped cells with the median of its own kept (v3-finite) member values.
  - A cell sitting 30x or more below that median is **defect-like**.
  - A cell within 10x of it is **genuine-like**.
  - A line with no kept value is defect-like when its SI ratio is above 5 or its implied size is below $20M.
  - This split uses the whole line, so it is a diagnostic only and must not become a producer rule.

| Class | Member cells | Lines | Score window | Typical ratios |
|---|---|---|---|---|
| Defect-like | 3,778 | 72 | 2,551 | turnover ratio r median 5.9 (p10 2.6); SI ratio q median 24 (p10 5.3) |
| Genuine-like | 6,925 | 105 | 5,752 | see below |
| Unresolved | 663 | 37 | not computed | |

- **Defect-like lines.** AMJ is mis-scaled across the whole line (1,092 cells). The rest are post-IPO unit breaks: CADE, TME, GOTU, SPOT, MNSO, PCOR, TUYA, ZI, ONON, AVTR, AMCR and others.
- **Genuine-like lines:**
  - 5,086 cells are in 55 leveraged, inverse or volatility ETPs, with r median 1.48 (p10 1.07, p90 2.83) and q between 0.06 and 0.4. Examples: LABD 452, UVXY 399, SQQQ 384, SOXS 384, DRIP 292, DUST 282, ERY 251, TZA 248, KOLD 240, JDST 204, BOIL 194, SPXS 153, QID 126, SDOW, TNA, SPXL, TQQQ, UPRO, SOXL.
  - XRT has 997 cells via rule (b), with SI ratio 2.84: real, well-known SI above 100%.
  - SMH has 163 cells and HUYA 95.
  - 2021 meme names are also caught: AMC 38, BBIG 36, SOS 32, MARA 17, ATER 16.
  - These cells sit at $20M-$2bn implied size. They are not the "real micro-caps" that the ruling's cost-if-wrong line anticipated.
- **Tier effect.** 5,183 genuine-like cells (4,088 in the score window, 97 lines) had market cap below $1bn and SI ratio of 0.10 or more under their correct v2 values, which is two flags: special. `strategy_nav_replay.cpp:527-531` now sends them to warm as a missing predictor. That understates borrow for the hardest-to-borrow ETPs, concentrated in Mar-Jun 2020 and 2022.
  - Leveraged ETPs in the role: 158 lines and 99,394 member cells, of which 5,678 (5.7%) were dropped.
- **Separability.** A 1000x units error puts r and q roughly in the 3-100 range, while genuine ETP turnover is 1-3x a day. The table counts caught cells for alternative thresholds, as defect-like caught out of 3,778 / genuine-like caught out of 6,925:

| (a), (b) | Defect-like caught | Genuine-like caught |
|---|---|---|
| 1.0, 1.5 (declared) | 3,778 | 6,925 |
| 2, 3 | 3,654 | 2,018 |
| 3, 5 | 3,558 | 568 |
| 5, 5 | 3,523 | 233 |

- **Validation.** It will behave the same way: the same ETPs trade in 2023-2024. I did not measure it, per selection hygiene.
- **Options for root** (all TRAIN-only evidence):
  - (i) Keep the rule as declared, and record that about 55 ETPs plus XRT drop to warm.
  - (ii) Declare amended thresholds, for example (a) 3x and (b) 5x, before T10 is re-scored on v3.
  - (iii) Add a T10 fallback for cells that the units rules NaN'd (implementer concern 1).

### Out-of-Scope Observations

- TRAIN peak RSS is 642 MiB against `--max-rss-mib 700` (92%, up from 635). This fails closed.
- The alpha library uses `shares_out` (`atx-impl/strategies/generate_pv_fields_ic121_v3.py:72,124`, for SI/shares_out and size). Any candidate scored on fields-v2 must be re-scored on v3, as progress already notes for the tiers.
- rereview-2 Minors N1 (v1 detector, 3 cells of headroom; validation max_non_mass is still 47) and N2 (session-name-only binding) are unchanged.

### Verdict

**Fix round:** All findings addressed, no new Critical/Important breakage (1/1 ADDRESSED; 2 new Minors). The declared thresholds also NaN about 6.9k genuine TRAIN member cells, mostly leveraged and inverse ETPs plus XRT, which changes about 4.1k score-window cells from special to warm. That calibration is root's call and should be ruled before T10 is re-scored on fields-v3.

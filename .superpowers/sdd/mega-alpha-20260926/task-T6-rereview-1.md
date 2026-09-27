# Task T6 re-review, fix round 1 (d3016d0b..c099cade)

Package: `review-T6-fix1.diff`, with 1 commit and 2 files changed (+311 / −50).

- P = `atx-engine/tools/prepare_research_fields.py` at c099cade. Its blob is `482da7bb`, which matches the report and the diff index line.
- T = `atx-engine/tools/test_prepare_research_fields.py` at c099cade.
- Tests: the report names the suite (14 tests, OK, log sha given), and root re-ran it with an OK result. I did not re-run it, because nothing in the code raised a doubt that needed a run.

### Finding Verdicts

**Important 1: IV fields had no plausibility check. Root ruled that values outside [0.02, 5.0] become NaN and are counted.** ADDRESSED.
- **The domain is declared once and attached to each IV spec.** See P:70-74 and the `"domain"` key on each `iv_atm_*` spec.
- **The raw vendor value is kept until the write.** That write happens after duplicate-key quarantine.
- **Out-of-domain values become NaN.** At P:668-679, `low = seen & ~(v >= lo)` and `high = seen & (v > hi)`. Both are float32 comparisons. `-inf` counts as low and `+inf` as high, and null or NaN is left alone. Out-of-domain cells are written as NaN and never clamped (P:679).
- **Counts are per tenor, for all cells and for member cells,** in `plausibility` (P:682-686).
- **Every field now reports `member_finite_quantiles`.** They are computed in the same read that produces the published sha (P:398-424), and that read is cross-checked against the writer's `vcount`. `vcount` uses the same member & finite predicate (P:358-373), so the two agree.
- **The fixture covers the garbage and boundary cases** (T:27-34 and T:311-345):
  - a 1e16 cell, and 9.0 on a non-member cell;
  - ±inf, negative values, and 0.0199;
  - both inclusive bounds kept exactly and never clamped.
- **I re-derived the fixture's expected counts myself:** 21d gives 3/3 and 63d/126d give 1/4. They match the test's expectations.

**Important 2: `is_common` is not point in time. It must be opt-in and carry a machine-readable `point_in_time` flag.** ADDRESSED.
- **`is_common` is flagged false with a reason.** It has `point_in_time: False`, `non_pit_aspects: ["values"]` and a `point_in_time_reason` (P:166-169).
- **The default field list is now PIT only.** `DEFAULT_FIELDS` keeps only point-in-time fields (P:185), and the CLI default and `run()` default both use it.
- **Every manifest entry carries the flag and the aspects list,** and the reason is added when the flag is false (P:995-998). The top level gains `non_point_in_time_fields` (P:1023).
- **Tests cover the flag and the default run** (T:401-423 and T:584-596):
  - the flags on all fields;
  - that the default run publishes only the 8 PIT files, byte-identical to the full run;
  - that the CLI run with `is_common` flags it false.

**Minor 1: FINRA data before 2021-06 is republished, and no machine could mask it.** ADDRESSED.
- **`vintage_safe_from` is now structured.** It is set as cutoff + 1 day (P:545).
- **Two new session-level keys** are `last_session_with_republished_visible_cell` and `first_session_vintage_safe` (P:528-544). Both are tested with a cutoff injected inside the role and a hand count (T:425-452).
- **The optional masking flag was not added.** The finding marked it optional, and leaving the choice to the controller is a sound decision. See new Minor N2 on how these keys are named.

**Minor 2: the manifest did not say what NaN means in `mktcap_lagged` / `size_grp`.** ADDRESSED.
- The `staleness` text now says a NaN means the line is outside the spine universe (not "large" and not a data gap), and names the `shares_out x raw_close` fallback (P:156, P:164).

**Minor 3: the `shares_out` restatement has no C-79 artifact guard.** NOT ADDRESSED. The implementer deferred it on purpose:
- The deferral is reasonable, because a bound on the restated count or the factor ratio is a policy threshold. Root should declare it before measurement, as it did for IV.
- **It matters more now.** Root plans to use `shares_out * raw_close` as the market-cap predictor, and the T8 generator already does (`atx-impl/strategies/generate_pv_fields_ic121_v3.py:475`).
- **The known extremes now land in that predictor.** The member minimum is 20 shares in both roles, and the VAL maximum is 3.12e10.
- **Recommendation: root should declare the bound before T10 freezes its size tiers.** For example, NaN when `|log(crf_t/crf_lag)|` is beyond a split-range bound or the restated count is below 1e4, with the cells counted. It could be one line in the T6 v2 rerun. The new `member_finite_quantiles` will show the tails without opening the matrices.

**Minor 4: C-81 withholding used rows after the session.** ADDRESSED.
- **The per-line flag became a date.** `first_above` is now the date of each line's first above-ceiling row (P:593 and P:641, via `np.minimum.at` over `ext_days[e]`, which is aligned with `sub` rows at P:607-616).
- **Withholding starts at that date.** A cell is withheld when `first_above <= date(session)` (P:711). That row is known at its 22:00 mark, before the 23:00 decision.
- **Counters:** `shares_lines_withheld_c81` keeps its meaning (P:661), and `shares_cells_withheld_c81` is added.
- **The staleness text states the deviation from the spine's whole-line rule.**
- **The test pins both sides of the date.** 303 is finite through 2024-11-29 and NaN from 2024-12-02. 404, whose offending row falls before the role, stays withheld throughout (T:347-370).

**Minor 5: nothing bounded how old the spine formation could be.** ADDRESSED.
- **A stale formation is refused.** The tool refuses when any session's latest formation is more than 35 days old (P:820-826), and also refuses when there is no formation at all (P:801-802).
- **The 35-day bound is sound.** The largest real gap between consecutive month-end sessions is about 33 days (for example 2021-02-26 to 2021-03-31).
- **Tested:** a lake that ends at the October formation fails at 2024-12-06 (36 days) and publishes no manifest (T:394-399).
- **The bound covers the tail only.** Sessions before the first formation get age 0, so they stay NaN rather than being refused (P:821). This is the same as before and covers the case the finding described. The real TRAIN lake starts at 2017-01, well before the role.

**Minor 6: `code_sha256` depended on line endings.** ADDRESSED.
- The fix is additive: `code_sha256_lf` and `code_git_blob_sha1` (P:225-230, P:1027).
- The formula `sha1(b"blob %d\0" % len(lf) + lf)` has the correct precedence.
- I confirmed that `git rev-parse c099cade:<path>` = `482da7bb…` = the report's value.
- Tested with a CRLF copy (T:511-521).

### Implementer's extra call: `mktcap_lagged` / `size_grp` flagged non-PIT (presence)

**Verdict: the reasoning is right, and the leak is stronger than the report states.** I checked it against `C:/atx/atx-db/src/atx_db/research/spine.py`.

**Presence is not PIT.** Spine eligibility is `security_type ∈ {common, common_unverified}` and `first_earn_date <= F` (spine.py:890, 973). Three things feed that test:
- **The type** comes from the 2026-09-18 directory for lines still listed then, and from whole-history vendor earnings evidence for every other line (spine.py:840-852).
- **`first_earn_date`** is the first bar with `nEarnCnt_504d > 0` (spine.py:878). T6 showed that this counts future events, so a line enters up to about 2 years before its first actual report.
- **Lines that die without ever reporting earnings** are `unknown` and are never present.

**The sharpest leak is one the report does not name.** The spine docstring records it (spine.py:833-835):
- A listed (surviving) ADR, REIT or LP is typed by its directory name, so it is excluded.
- A delisted one with earnings becomes `common_unverified`, so it is present.
- For those types, a finite `mktcap_lagged` therefore means "delisted before 2026-09". In the 2023-2024 validation role that is a forward-looking survival signal.

`isnan(mktcap_lagged)` would carry this even with `is_common` withheld, which is exactly the implementer's argument.

**Values are PIT, so `non_pit_aspects: ["presence"]` is accurate:**
- `me_line` is the formation price times A8-lagged shares, restated by the repaired-factor ratio and withheld across C-79 breaks (spine.py:999-1039).
- `size_grp` compares `me_line` with the external French NYSE breakpoint file joined by month (spine.py:1023-1027, 1042). The thresholds are not computed from the survivor-biased spine cross-section.

**Root's substitute, `shares_out * raw_close`, is PIT on its clocks:**
- `shares_out` is the A8 90-day lag with per-row A9 and same-date duplicate quarantine, plus the C-81 withholding that is now PIT.
- `raw_close` is the session close, known at 22:00.
- Coverage is about 99.7%.

**Caveats for the consumer:**
- **M3 is open.** Unguarded factor-ratio artifacts reach the predictor directly (see above).
- **It is one line's value, not the issuer total.** Multi-class issuers are understated, and an ADR is counted as ADS times the ADS price, which is consistent.
- **ETFs and funds now get a value.** The spine excluded them.
- **It is a daily series, where the spine is monthly.** Hard tier thresholds may flip more often, so T10 may want hysteresis or monthly sampling.
- **Simplify the rerun.** T8 already lists `mktcap_lagged`, `size_grp` and `is_common` as unused (generator :479). Root can drop `mktcap_lagged,size_grp` from the v2 `--fields` (report lines 354-356). The payload is then all-PIT, and T7 needs no allow ruling.

### New Breakage in the Fix Diff

No new Critical or Important breakage. Three new Minors:

**N1 (Minor): the machine-readable `plausibility.min` does not match the effective bound** (P:683 vs P:673).
- The manifest publishes `"min": 0.02, "inclusive": true`.
- The kept bound is `float32(0.02)`, which widens to 0.019999999552965164 in the published f64 bytes.
- A T7/T8 loader that checks `value >= plausibility.min` from the manifest would flag the boundary cells as violations. So would a check that `member_finite_min >= min`.
- The rule string does disclose the float32 comparison.
- Fix: add `"compare_dtype": "float32"` and the effective f64 bounds, for example `min_effective: float(np.float32(0.02))`.

**N2 (Minor): `vintage_safe_from` is an `available_at` threshold, but nothing says so** (P:545).
- Its value is cutoff + 1 = 2021-06-10. The first original dissemination in the real schedule is 2021-06-24, so the report's wording "first available_at of original-vintage rows" is inaccurate.
- A consumer that reads it as a session start would still include republished rows that stay visible for up to 45 days (into late July 2021).
- The correct session start is `coverage.vintage_risk.first_session_vintage_safe`.
- Fix: add a one-line description to the field entry. Downstream, T8/T10 should use `first_session_vintage_safe`.

**N3 (Minor): two statements claim every field is known by 22:00** (P:79, P:1021).
- `visibility_mark` and `POINT_IN_TIME_DEFINITION` say "every finite cell of every field is known by the session-date 22:00 UTC mark".
- That is false for opt-in non-PIT fields: `is_common` values and spine presence use 2026 information.
- Fix: qualify both to "every point_in_time field", so a consumer does not set `available_at_ns` from them for flagged fields.

**Cosmetic:** `months =[(` is missing a space (P:803).

### Out-of-Scope Observations

- **The T8 generator still applies an IV floor and cap** (`generate_pv_fields_ic121_v3.py:50`, `IV_FLOOR, IV_CAP, IV_BACKFILL = '0.02', '5', 5`). Against the v2 payload the clamp is a no-op, because out-of-domain cells are already NaN.
  - Whether `IV_BACKFILL` fills the new NaN cells should be checked in T8's review. It must not reintroduce a clamped 5.0 or 0.02 where T6 now publishes NaN.
- **`iv_atm_*` and `earn_recent` are `point_in_time: true`** even though the vendor's earnings-calendar vintage is unproven. This follows the declared definition, where vintage is excluded from the flag, and the global `historical_vintage_verified: false` is present.
  - A consumer keyed only on `point_in_time` will not see the caveat.
  - Consider a per-field `vintage_verified: false`.

### Verdict

**Fix round:** Both Important findings are addressed, with no new Critical or Important breakage.

- **Still open:** Minor 3, the C-79 guard on `shares_out`. It needs a bound that root declares, and it matters more now that `shares_out * raw_close` is the planned market-cap predictor.
- **New Minors:** N1-N3 are wording and manifest-contract clarifications and do not block.
- **Result:** from the review side, T6 can be re-run as `-fields-v2`.

# Task F-3 report: v8 field specs (F-A done; F-B, F-C, F-D not started)

Lane F3, pool-9, branch `feat/platform-v8-f3-20260929` (base `76aa05e3` = lane F F-1 + lane C C-3). Python only;
synthetic tests only. Stopped at the owner's instruction after F-A (see "STOPPED HERE").

## STOPPED HERE

| field | status | commit |
|---|---|---|
| F-A `grp_ff12f49` | DONE, 8 tests | `230b39e1` |
| F-B `k8_item402_63` | not started | - |
| F-C `gscore7_lowbm` | not started | - |
| F-D `eps_consist_4y` | not started | - |

Design notes for whoever resumes F-B to F-D are at the end of this report.

## F-A `grp_ff12f49`: what was built

New module `atx-engine/tools/research_fields_v8.py`. It is an opt-in `FIELD_MODULES` module, registered at the end
of `prepare_research_fields.py` after the F-1 price hook.

**Interface.** The module follows C-3:
- `bind`, `FIELDS`, `OPTIONS = ()`, `add_arguments`, `check` and `compute`;
- `PRODUCERS = {"v8_ff12f49": ("ff12f49_rows",)}` and `HOST_HANDLES = ("h",)`;
- `producer_group`, `field_spec`, `reuse_inputs` (returns `{}`) and `entry_inputs` (returns `{}`).

**Code units.**
- `ff12f49_code(ff12, ff49)` is the pure cellwise rule.
- `ff12f49_rows` is the producer.
- `PublishedRows` is a generic reader of this run's published payloads. It hashes each payload as it reads it, and
  those hashes are recorded as the field's `sources`. F-C can reuse it.

**Registration without a fingerprint change.**
- `bind()` adds `FIELDS` to the builder's `ALL_FIELDS` itself. The builder hook is therefore two code lines:
  `import research_fields_v8 as _v8` and `FIELD_MODULES.append(_v8.bind(globals()))`.
- Measured: an F-1-style `ALL_FIELDS.update(_v8.FIELDS)` statement in the builder changes the SEC module's producer
  fingerprint. The SEC host closure reads `spec_definition`, which reads `ALL_FIELDS`. The cost would have been a
  recompute of all 14 SEC fields under `--reuse`.
- With the chosen form, the builder, SEC and holdings fingerprints are all unchanged. The test
  `test_registration_keeps_every_other_producer_fingerprint` pins this.

| item | value |
|---|---|
| formula id | `ff12-money-ff49-v1` (entry also carries `formula_sha256`, `producer`, `money_member_cells`) |
| inputs | this run's `grp_ff12.f64`, `grp_ff49.f64` (`requires`, manifest `depends_on`); both SHA-256 recorded in `sources` |
| rule | 100 + ff49 when ff12 == 11 and ff49 in {45, 46, 47, 48} (145 Banks, 146 Insur, 147 RlEst, 148 Fin); otherwise ff12 (Money with NaN ff49 gives 11; non-Money gives ff12 bit for bit; ff12 NaN gives NaN) |
| clock | that of grp_ff12 / grp_ff49 (fund-events-lagged-v1 on the SIC table, the same latest visible valid SIC row, `--fund-lag-sessions`); row t reads their row t only |
| staleness | inherits grp_ff12 (550 days): NaN exactly where grp_ff12 is NaN |
| validity start | wherever grp_ff12 is finite: role row L onward. The SIC table starts 2009q2 (FSDS), so the field is defined throughout TRAIN 2020-2023. |
| seal | no own read. The SIC rows are sealed by the builder (`SEAL_NS` in `load_events` / `load_sic_stage`), so the field follows W0-1 after the merge. |

**Registered choices.**
1. REIT (6798) is 148 Fin, as French's Siccodes49 has it.
2. A Money SIC that Siccodes49 does not list keeps 11. There are 592 such SICs in the pinned table, for example
   6001-6009.
3. The rule is written in the stated order: `otherwise ff12` also covers a Money cell whose FF49 is outside 45-48. The
   pinned table has none: every finite Money FF49 is in {45..48}, and no non-Money SIC maps to 45-48.
4. `lagged: False`, and the clock names grp_ff12's clock without a `{lag}`. This is F-1's `xrd0_ttm` precedent. A
   `{lag}` template would never match in C-3's `module_formula`, so the field would never be reused. A lag change
   recomputes grp_ff12, and the `requires` rule then recomputes this field.

## How root verifies

1. Tests:
   ```
   cd atx-engine/tools
   "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_research_fields_v8.py test_research_fields_price.py test_prepare_research_fields.py test_prepare_research_fields_sec.py test_research_fields_holdings.py test_prepare_research_fields_sic.py test_prepare_research_fields_sv.py test_prepare_research_fields_module_reuse.py test_prepare_research_fields_reuse.py test_record_store.py
   ```
   Here: **120 passed** (8 new). The new tests (fixture dated 2021, before the W0-1 seal):
   - `test_money_mappings`: the four Money codes, 11, 6 and 12, the late link and the 550-day staleness. It also checks
     the full array against a cell-by-cell oracle and checks `money_member_cells`.
   - `test_non_money_cells_equal_grp_ff12_bit_for_bit`: a `<u8` view comparison.
   - `test_nan_pattern_equals_grp_ff12`: also checks the canonical NaN bytes.
   - `test_field_at_t_unchanged_when_rows_after_t_mutate`: every SIC row at or after the t mark is changed, and rows
     are added after it (one exactly at the mark). Rows 0..t stay byte-identical and later rows move.
   - `test_existing_payloads_byte_identical`: grp_sic2, grp_ff12 and grp_ff49 have the same files and entries with or
     without the new field.
   - `test_reuse_copies_unchanged_and_recomputes_after_a_group_change`: a self reuse copies all four, and so does a
     reuse of a reuse. A changed SIC table recomputes the field with the reason "depends on grp_ff12, grp_ff49".
   - `test_requires_registration_and_cli`.
   - `test_registration_keeps_every_other_producer_fingerprint`.
2. Identity:
   - Without `grp_ff12f49` in `--fields`, the module computes nothing and every output is unchanged.
   - With it, only `grp_ff12f49.f64` and its entry are added. Every other field's `sha256` in the new manifest equals
     the prior's.
3. Fields v10 on the 4-year role: the v9 argv plus the following. `grp_ff12` and `grp_ff49` are in v9; F-A needs no
   new option.
   ```
   --fields <v9 list>,ret_overnight,ret_intraday,ceq_iss_5y,coskew_60m,vol_126,xrd0_ttm,grp_ff12f49
   --reuse <fields-v9 dir> --reuse-sha256 <its manifest sha> --reuse-hardlink
   --price-source C:/Users/natha/Downloads/TickerHistory3.parquet --max-rss-mib 1200
   ```
   Two blockers, both outside F-A:
   - **The F-1 price module has no C-3 reuse interface** (see Open risks). This argv fails as soon as `--reuse` meets
     any F-1 field.
   - **v10 has 63 + 6 + 1 = 70 fields**, above the 64-row manifest cap. B-2's cap lift or a lean manifest is needed
     first (draft section 8).

   If only F-A is wanted for the R-2 cell, use the v9 argv plus `grp_ff12f49` and `--reuse`. That build does not hit
   the first blocker, and 64 rows is within the cap.

## Cross-lane edits

- `atx-engine/tools/prepare_research_fields.py`: 4 lines at the end, after the F-1 hook (2 comment lines, then the
  import and the append). The seal constants and the research_window import are untouched.
- `atx-engine/tools/test_research_fields_price.py:457`:
  - The old assertion said the price fields are the last block of `ALL_FIELDS`. That is false once any later module
    registers.
  - The new assertion says they form one contiguous block after every builder and SEC field.

## Open risks

- **The price module is missing the C-3 reuse interface.** This is a merge gap between F-1 and C-3 on this base.
  - `research_fields_price.py` has `PRODUCERS` but no `HOST_HANDLES`, `producer_group`, `field_spec`, `reuse_inputs`
    or `entry_inputs`. Its entries record `producer_code`, not `producer`.
  - `run()` calls `reuse_module_fields` for every `FIELD_MODULES` module with requested fields. With `--reuse` and any
    F-1 field, `module_fingerprints` therefore raises `AttributeError: HOST_HANDLES`. This happens after
    `output.mkdir`, which leaves a partial directory.
  - Fix, about 20 lines:
    - add `HOST_HANDLES = ("h",)`;
    - `producer_group` returns `FIELDS[name]["group"]`, and `field_spec` returns the spec;
    - `reuse_inputs` and `entry_inputs` return `{}`. The price source is pinned to the role's `source_sha256`, and
      the role is pinned by `load_prior`;
    - record `"producer": {"module": ..., **h.module_code_identity(sys.modules[__name__])}` in the entries;
    - update the one test line (`test_research_fields_price.py:419`).
  - Not done here, on the stop instruction.
- Existing synthetic fixtures of other modules still use 2024 sessions (C-3 report). Mine use 2021 and survive the
  W0-1 seal.

## Design notes for the remaining fields (not implemented)

- **F-B `k8_item402_63` belongs in the SEC module's pass**, not in `research_fields_v8.py`, for three reasons:
  1. The K8 parse, calendar and link rule are `SecFieldModule` code. A separate module would have to import them, and
     imports are opaque to `code_fingerprint`: an edit there would not invalidate reuse.
  2. One pass reads and hashes `eight_k_items.parquet` and loads the SEC bridge once.
  3. There is no copy of the `_eightk` loop.

  How: add `"4.02"` as a second mask in `_eightk` (an `item402` Windowed with window 63, same `usable` / `event`
  clock), a spec with `stages ["sec_filings"]` and formula `sec-k8-item402-63-v1`, and add its stats key only when the
  field is requested.

  Cost: the `sec` group fingerprint changes, so all 14 SEC fields recompute once under `--reuse`. Their payloads stay
  byte-identical, which the ByteIdentity tests check. The W0E seal merge forces that same recompute anyway (C-3 report).
- **F-C and F-D need 16 to 24 fiscal quarters of history.** The 4-year role starts about 2018-06, so the daily
  panels cannot serve it. They must read `fundamental_events.parquet` through the builder's `load_bridge`,
  `resolve_links`, `load_events` and `advance` (via `h`, so the reads are fingerprinted), using the builder's own
  `--identity-bridge`, `--fund-events`, their SHA-256 pins and `--fund-lag-sessions`. These builder argument names
  would go in the module's `OPTIONS`, so that `main()` hands the same parsed values over; programmatic `run()` callers
  pass them in `module_options`.

  Per events row, derive values from the CIK's rows up to that row. Map fiscal quarters by calendar offset:
  quarter k = the known `period_end` nearest A - round(91.3125 k) days, within ±20 days (the contract's lag
  tolerance). Then pick the latest visible row per session with the anchor staleness rule, as the issuer fields do.

  Open questions for root:
  - F-C has no discrete-quarter revenue item. Quarterly sales growth would be `sale_ttm(P)/sale_ttm(P-1q) - 1`.
  - F-C: the minimum history for the two variances (proposed 12 of 16, as F-D).
  - F-C: use `me_company[t-1]` (the t-1 price rule) in bm.
  - F-C: the peer set is member names only.
  - F-D: split basis. EPS from the anchor row of each quarter is on the share basis as first reported, so a split
    inside the 8-quarter span distorts g.
  - F-D: the literal denominator `mean(EPS_q-4, EPS_q-8)` can be 0 or negative. Root was to check it against the CZ
    code.

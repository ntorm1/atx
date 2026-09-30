# Task F-3 report: v8 field specs F-A to F-D and the E-21 fix

Lane F3, pool-9, branch `feat/platform-v8-f3-20260929`. Python only; synthetic tests only.

## Status

| task | status | commit |
|---|---|---|
| Step 0: merge root (integration 3, W0-1) | DONE, no conflicts | `c6b3ee0b` |
| E-21: F-1 price module reuse interface | DONE | `eca04c18` |
| F-B `k8_item402_63` | DONE | `0687e82f` |
| F-C `gscore7_lowbm` | DONE | `1444310f` |
| F-D `eps_consist_4y` | DONE | `0f32d581` |
| F-A `grp_ff12f49` (predecessor) | DONE | `230b39e1` |

Tests (`atx-engine/tools`, synthetic, `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider`):
```
test_research_fields_v8_quarters.py test_research_fields_v8.py test_research_fields_price.py
test_prepare_research_fields.py test_prepare_research_fields_sec.py test_research_fields_holdings.py
test_prepare_research_fields_sic.py test_prepare_research_fields_sv.py test_prepare_research_fields_module_reuse.py
test_prepare_research_fields_reuse.py test_record_store.py test_research_window.py test_seal_partitions.py
```
Result: **147 passed, 6 subtests passed** (post-merge baseline of the first 11 files: 127). New fixtures are dated
2014-2021, except one synthetic row per F-A/F-B fixture dated just after the seal (the seal probe, F-A's `AFTER_SEAL`).

## E-21: the F-1 reuse interface (`eca04c18`)

- `research_fields_price.py` now has `HOST_HANDLES = ("h",)`, `producer_group` (the spec's group), `field_spec`,
  `reuse_inputs` and `entry_inputs` (both `{}`: the price source must hash to the role's `source_sha256`, and the prior
  is bound to the same role). Computed entries record `producer` (module code identity) instead of `producer_code`.
- Builder (`prepare_research_fields.py`): `MODULE_REUSE_INTERFACE` and `module_reuse_interface(module)`. `run()` calls
  it for every module with requested fields when `--reuse` is given, **before `output.mkdir`**. A module without the
  interface is refused with "`--reuse: field module X lacks the reuse interface (...)`" and no output directory.
- No producer fingerprint moved (builder 6/6, SEC, holdings 4/4, price 5/5, v8 equal to `c6b3ee0b`).
- Test `test_reuse_with_price_fields`: a mixed reuse (2 copied, 3 computed, files equal a fresh build); a self reuse
  copies all 5 byte for byte (entries verbatim plus `reused_from`); a chained reuse; a simulated `coskew_rows` edit
  recomputes only `coskew_60m`; a pre-E-21 prior (entries with `producer_code` only) is recomputed ("producing code not
  recoverable"); deleting `HOST_HANDLES` gives the named refusal before any output.
- A prior written by F-1 code before this fix has no `producer`: its price fields recompute once.

## F-B `k8_item402_63` (`0687e82f`)

In `research_fields_sec.py`'s 8-K pass, as the design note said:
- `_eightk` builds a second item mask (`item == "4.02"`, OR-reduced per accession like the material mask) and a
  `Windowed` of window 63 on the same usable/event clock.
- Spec: stage `sec_filings`, formula `sec-k8-item402-63-v1`, `K8_PRESENT` NaN rule and `LINK_RULE` (primary lines).
  1 if an original 8-K (form starts with 8-K, not an amendment) with item 4.02 became usable within the last 63
  sessions (e <= t < e + 63), else 0.
- Registered after every v7 SEC field, so the registry order of the others is unchanged.
- `source_checks.sec.sec_filings.accessions_used_item402` is added only when the field is requested. Without it, the
  SEC checks and every `k8_*` payload are byte-identical.
- Cost: the `sec` group fingerprint changes, so the 14 SEC fields recompute once under `--reuse` (same bytes).

Tests (2021 fixture, `test_research_fields_v8.py::K8Item402`):
- every cell against an oracle;
- the window edges e-1 / e / e+62 / e+63;
- a 22:30 UTC acceptance;
- point in time: rows at or after the t mark mutated, plus rows exactly at the mark and one hour after; rows 0..t
  byte-identical for all four `k8_*` fields;
- the 8-K/A and a 10-Q with 4.02 never count, and an amendment alone gives no presence;
- the seal: `tool.SEAL` equals `research_window.SEAL`. A patched `SEAL_NS` inside the role drops the later rows, the
  output equals the oracle at that seal, and `rows_sealed` is counted;
- `k8_*` payloads, entries and checks are unchanged without the field;
- reuse: mixed and self.

The existing SEC oracle gained the field (all 0/NaN there). The C-3 63-field recipe excludes it.

## F-C `gscore7_lowbm` (`1444310f`) and F-D `eps_consist_4y` (`0f32d581`)

Both live in `research_fields_v8.py` (one module, so every helper the producers reach is inside their fingerprinted
closure). `PRODUCERS`: `v8_gscore: (issuer_history, gscore_rows)`, `v8_epscons: (issuer_history, eps_consist_rows)`.

**Inputs.** The history is read through `h` (fingerprinted): `load_bridge`, `resolve_links`, `pinned_manifest`,
`load_events`, `advance`, `EVENTS_ADAPTER` and `NULL_PERIOD_DAY`.
- Sources are the builder's own `--identity-bridge`, `--fund-events`, their pins and `--fund-lag-sessions`. These
  names are the module's `OPTIONS`, so `main()` passes the parsed values.
- `run()` callers must pass them in `module_options` too. `check` refuses before any output if one is missing or the
  lag is outside [0, 5].
- The selection at session t is the issuer fields' own (`LatestRows`): the latest row with clock < mark(t-L), primary
  lines, and the 200/400-day staleness.

**Point in time.**
- Each events row r carries a fiscal-quarter view built from the CIK's rows up to r in (accepted_utc, accession) order
  (`QuarterIndex`, rule `fiscal-quarter-view-v1`).
- Quarter k is the period_end nearest A - floor(91.3125 k + 0.5) days within 20 days (ties: the later). Its items are
  those of the latest row up to r anchored at it.
- Every row of the view is visible whenever r is selected. Sealed rows are dropped by `load_events` (`SEAL_NS` from
  research_window).

**F-C**, formula `mohanram-g7-lowbm3-sic2-v1`:
- bm = be / me_company[t-1], with be > 0 and me_company from this run's payload of the previous row. Bottom tercile =
  bm <= the 1/3 quantile (numpy linear) over member names with a finite bm.
- Seven signals:
  - ROA, CFROA, RDA and CAPXA are above the peer median. They are scaled by `at`, and a missing R&D counts as 0;
  - CFO > NI;
  - VARROA and VARSGR are below the peer median. VARROA uses ni_q/at of each quarter, and VARSGR uses sale_ttm(P_k) /
    sale_ttm(P_k+1) - 1. Each is a ddof-1 sample variance over k = 0..15 with at least 12 finite.
- Peer median, per measure: over the tercile names of the same `grp_sic2` with that measure finite.
- A tercile name is scored only when `grp_sic2` and every input but R&D are finite. Otherwise, and outside the
  tercile, the value is NaN. Non-member cells are NaN.
- `requires: [me_company, grp_sic2]`.

**F-D**, formula `alwathainani-eps-consistency-16q-v1`:
- EPS_k = ni_q / shrs_q (shrs_q > 0, as reported).
- g_k = (EPS_k - EPS_k+4) / ((|EPS_k+4| + |EPS_k+8|) / 2), NaN on a 0 denominator.
- Value = the mean of the finite g_k, k = 0..15, when at least 12 are finite.
- NaN when g_0 or g_4 is missing, |g_0| > 6, or g_0 g_4 < 0.
- The entry carries `nan_reasons_member_cells` by reason.

**Entries.** Both record `identity_bridge_manifest_sha256`, `fund_events_manifest_sha256` and `fund_lag_sessions`.
These are `entry_inputs` / `reuse_inputs`, so a changed bridge, events or lag recomputes them ("inputs differ").
`source_checks.v8` holds the lag, the clock, the quarter rule, the bridge and the events checks.

**Tests** (`test_research_fields_v8_quarters.py`: 2014-2021 world with TH3, FINRA, role, bridge, SIC and events; 9
tests):
- both fields cell by cell against an independent oracle;
- point in time: events and SIC rows at or after the t mark are mutated or added (one exactly at the mark), and the
  role's raw_close is scaled from row t on, which moves me_company at t itself. Rows 0..t of both fields stay
  bit-identical;
- seal: rows added after a seal inside the role would move both fields unsealed. Sealed, the output equals the world
  without them;
- the base payloads, entries and checks are identical without the new fields;
- reuse: self, mixed, and a lag change;
- refusals before output, and the CLI;
- QuarterIndex against brute force (amendments, missing quarters, jitter, a null period_end, a tie);
- the G-score cross-section on 400 random names;
- EPS rule boundaries (|g_0| = 6 kept, a zero g, a zero denominator, exactly 12 finite).

**Registered choices** (not in the draft or E-20; root may overrule before the build):
1. The quarter rule itself.
2. "Prior 16 quarters" = k = 0..15, the anchor's quarter included.
3. The tercile is ties-inclusive at the cut, and U is every member name with a finite bm.
4. Per-measure peer medians.
5. Strict > / < comparisons.
6. ddof 1.
7. F-D: a missing g_0/g_4 gives NaN. "Opposite signs" is strict: a zero g does not change sign.

## How root verifies

1. The tests above.
2. Fingerprints (what I ran): `code_fingerprint.fingerprints` / `module_fingerprints` of `git show c6b3ee0b:<file>`
   against the working tree:
   - builder 6/6 equal;
   - holdings 4/4 equal;
   - price 5/5 equal;
   - `v8_ff12f49` equal;
   - `sec` differs (F-B, intended);
   - `v8_gscore` and `v8_epscons` are new.
3. Opt-in identity on the build: every new field is outside `DEFAULT_FIELDS`. After the v10/v11 build with `--reuse`,
   run the C-3 identity one-liner (task-C-3-report step 3) on prior versus new. Expected:
   - every prior field's `sha256` is unchanged, the 14 recomputed SEC fields included;
   - `reuse.not_reused` names only the SEC fields ("producing code differs (research_fields_sec.py group sec ...)")
     and the new fields ("absent from the prior manifest").

## Argv deltas and expected `--reuse` counts

Prior P = `v8-i3p4-c-fields2` (63 fields, integration-3 code, live regsho pin; manifest `5e5def8d...`), with its argv
(integration-log section c). Assume integration 4 merges this lane and moves no other builder or holdings closure.

**Fields v10** (v9 + F-1 + grp_ff12f49 = 70 fields). Argv delta:
```
--fields <v9 63 names>,ret_overnight,ret_intraday,ceq_iss_5y,coskew_60m,vol_126,xrd0_ttm,grp_ff12f49
--price-source C:/Users/natha/Downloads/TickerHistory3.parquet     (must hash to the role's source_sha256)
--reuse <P> --reuse-sha256 <P manifest sha> --reuse-hardlink
```
- Keep `--max-rss-mib 2048` from v9's argv (F-1 needs at least 1200).
- If the build stops with `FieldNeedsOpen`, drop `ret_overnight,ret_intraday` (OD-6).
- `grp_ff12f49` and `xrd0_ttm` need only `grp_ff12` / `grp_ff49` and `xrd_ttm` / `sale_ttm`, all in v9.

Expected counts:
- with F-B merged: **reused 49, computed 21**. The 14 SEC fields recompute once with the same bytes; the 7 new
  fields are computed.
- with only E-21 and F-A merged: reused 63, computed 7.

**Fields v11** (v10 + F-B, F-C, F-D = 73 fields). Argv delta:
```
--fields <v10 list>,k8_item402_63,gscore7_lowbm,eps_consist_4y
```
- No new option. F-B uses v9's `--sec-stages`, `--sec-filings-sha256` and `--sec-identity-bridge*`. F-C and F-D use
  v9's `--identity-bridge`, `--fund-events`, their pins and `--fund-lag-sessions`.
- `gscore7_lowbm` requires `me_company` and `grp_sic2`, both in v9.

Expected counts:
- from v10: **reused 70, computed 3**;
- directly from P: reused 49, computed 24.

## Deviations, with reasons

- F-C: per-measure peer medians (the draft's "median of the same measure"). My first cut used only fully scored
  peers; I changed it before commit.
- F-C/F-D: history comes from `fundamental_events` rows, not the daily panels (the design note: 16 to 24 quarters
  exceed the 4-year role).
- E-21's builder check runs only with `--reuse`, so builds without it are untouched.

## Cross-lane edits

- `prepare_research_fields.py` (lane C): `MODULE_REUSE_INTERFACE`, `module_reuse_interface()`, and 2 lines in `run()`.
  They are outside every closure.
- `research_fields_price.py` and `test_research_fields_price.py` (lane F): the E-21 interface, line 419, and one new
  test.
- `research_fields_sec.py` (W5a / C-3): F-B.
- `test_prepare_research_fields_sec.py`: the oracle's F-B key (1 line).
- `test_prepare_research_fields_module_reuse.py`: `SEC_V9` leaves out the opt-in F-B field, keeping the 63-field v9
  recipe.

## Open risks

- **Manifest cap:** v10 has 70 rows and v11 has 73, above the 64-row runner cap. B-2's cap lift or a lean manifest
  must land first.
- **Programmatic `run()` callers** pass the issuer inputs twice (kwargs and `module_options`). A mismatch would give
  F-C/F-D other inputs than me_company/grp_sic2, and nothing detects it. `main()` (every production build) is
  consistent by construction.
- **Quarter view:** a restatement of an older quarter that arrives with a later anchor is not seen. The quarter keeps
  its last value from while it was the anchor (caveat on both fields).
- **History reach:** events rows start 2014-06-01.
  - F-C (13 to 17 quarters) is defined from about 2017-08.
  - F-D (at least 20 quarters for 12 finite g) is defined from about mid-2019.
  - TRAIN 2020-2023 is covered by both. The early rows of the 4-year role are sparse.
- **Split distortion (F-D):** as-reported shares distort g for up to 8 quarters after a split (E-20 disclosed). At a
  Q4 anchor, `shrs_q` is often the fiscal-year count.
- **Seal constant names:** the builder must keep the `SEAL_NS` name that `load_events` / `load_bridge` read (they
  follow research_window).

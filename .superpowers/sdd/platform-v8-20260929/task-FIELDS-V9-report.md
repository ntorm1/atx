# Task FIELDS-V9 report: draft fields for the v9 candidates `nt_late` and `earn_season` (wave AG)

Lane FIELDS-V9, pool 11, branch `feat/platform-v8-lib3-20261001`, from `e9e1ee20`. Python only; no C++, no build, no
real data (synthetic pytest only). Ruling PM5-13: new module files, off by default, in no v8 field list; registered as a
draft list for a later version; merge after the v8 freeze gate.

## Status

| task | status | commit |
|---|---|---|
| 1. `nt_first_126` (C-3 `nt_late`) module + draft registration entry | DONE | `8cfd6dc8` |
| 2. `earn_season_rank` (C-4 `earn_season`) | DONE | `a14e0b02` |
| 3. synthetic tests (values, look-ahead, seal, identity) | DONE | `aab60d2e` |
| 4. draft status FIELD-BUILT + this report | DONE | this commit |

Both candidates are **FIELD-BUILT (uncompiled DSL, unrun on data)**. Field names: `nt_first_126`, `earn_season_rank`
(the `earn_season` DSL also reads the existing v9 field `ea_days_to_expected`). Draft list: fields v13 =
fields v12 + (`nt_first_126`, `earn_season_rank`).

## 1. Files and interfaces as coded

New files only (all under `atx-engine/tools/`); no existing file changed except the LIB3 draft doc the brief names.

**`research_fields_v9.py`** (platform v9 draft field module, the `research_fields_v8.py` pattern):
- `GROUP = "v9"`; `FIELDS`: `nt_first_126` (group `v9_nt`, formula id `sec-nt-first365-126-v1`), `earn_season_rank`
  (group `v9_earnseason`, formula id `chss-earnrank-ni20q-v1`, domain [3, 18]). Both `point_in_time: True`, not lagged.
- `PRODUCERS = {"v9_nt": ("nt_rows",), "v9_earnseason": ("earn_season_rows",)}`, `HOST_HANDLES = ("h",)`;
  `bind(host_namespace) -> V9FieldModule` appends `FIELDS` to the builder's `ALL_FIELDS` itself (never a module-level
  `ALL_FIELDS.update` in the builder); `producer_group`, `field_spec`, `reuse_inputs(name, options)`,
  `entry_inputs(entry)` (v8 C-3 `--reuse` contract).
- `OPTIONS` = the SEC module's `sec_stages`, `sec_identity_bridge(_sha256)`, `sec_filings_sha256` and the builder's
  `identity_bridge(_sha256)`, `fund_events(_sha256)`, `fund_lag_sessions`. **No new CLI option.**
- `imported_code(group, source=None, builder=None) -> {"module", "names", "sha256"}`: the producers call code imported
  from `research_fields_sec.py` (`Calendar`, `Latest`, `Windowed`, `accession_keys`, `cik_positions`) and
  `research_fields_v8.py` (`issuer_history`, `LatestRows`, `QuarterIndex`, `gathered`, `picked`), which no producer
  fingerprint follows (review B-1 of F-1). The AST closure of those names (code_fingerprint), plus the builder
  definitions they read through `h`, is recorded in each entry and is a `--reuse` input pin: an edit of the imported
  code recomputes the field (tested).
- nt helpers: `stage_batches` (a manifest-listed stage file hashed as a stream and verified before parsing, identity
  held until the last batch: `filings.parquet` is never held in memory), `form_codes`, `first_flags`, `nt_notices`,
  `periodic_filings`, `nt_rows`. earn helpers: `latest_anchor`, `average_ranks`, `earn_rank_history`,
  `earn_season_rows`.
- Entry extras: `producer`, `formula_id`, `formula_sha256`, `min_history`, `imported_code`; nt: `stage_manifests`
  (`sec_filings`), `identity_bridge_manifest_sha256` (SEC bridge), `lag_sessions` 1, `lag`, `nan_rule`,
  `nan_reasons_member_cells`, `flagged_member_cells`, `coverage_linked_primary`; earn: `identity_bridge_manifest_sha256`,
  `fund_events_manifest_sha256`, `fund_lag_sessions`, `domain`, `nan_reasons_member_cells`. Source checks under
  `source_checks.v9.<field>`.

**`prepare_research_fields_draft.py`** (the draft registration; the plain builder never imports it):
`DRAFT_VERSION = "v13"`, `DRAFT_MODULES = (research_fields_v9,)`, `FIELDS_V13_DRAFT = ("nt_first_126",
"earn_season_rank")`, `register(host_namespace) -> [bound modules]` (bind + append to `FIELD_MODULES`; idempotent),
`main(argv)` = `register(vars(prepare_research_fields))` then the builder's own `main` (same argv, same manifest, the
builder's own code identity in the manifest).

**Tests**: `test_research_fields_v9_nt.py` (11), `test_research_fields_v9_earn.py` (11),
`test_prepare_research_fields_draft.py` (4). The draft is registered only inside these test classes
(`mock.patch.dict(ALL_FIELDS)` + a copied `FIELD_MODULES`, undone on exit), so every other test module sees the plain
builder.

## 2. Point-in-time argument

**`nt_first_126`.** Inputs: `sec_filings/events.parquet` (NT 10-K / NT 10-Q / NT 20-F rows) and
`sec_filings/filings.parquet` (periodic forms), both on atx-db's `available_at` = EDGAR acceptance
(acceptance-per-file-clock-v1; else filing date + 1 day 00:00 ET), and the SEC identity bridge (LINK_RULE, primary
lines). Session assignment is research_fields_sec.py's `SEC_CLOCK` through its own `Calendar.usable_from`: a row is
usable at session t iff `available_at` < 22:00 UTC of session t-1 (role sessions inside the role, NYSE rule outside).
So a notice accepted 15:00 UTC on d is usable from d+1, one accepted 22:30 UTC on d (after the close mark) from d+2
(tested: 21:59 vs 22:30). "First" compares the notice with strictly earlier notices of the same CIK (by
`available_at`, all visible no later than the notice itself); presence uses only filings visible at t (same clock,
>= mark(t-1) - 400 days). The flag window is [e, e + 126) sessions from the usable session e. A duplicated accession
takes the later clock (conservative). Nothing in the value at t depends on a row with `available_at` >= mark(t-1).

**`earn_season_rank`.** Inputs: the issuer fields' own fundamental events (`--fund-events`, `--identity-bridge`,
`--fund-lag-sessions` L = 1) through research_fields_v8's `issuer_history` / `LatestRows`: the selected row at t is the
CIK's latest events row with `accepted_utc` < mark(t-L), stale after 200 / 400 days, primary lines only. Its quarter
view (`QUARTER_RULE`, E-30) holds only rows up to it in clock order, so the 20 ranked `ni_q` values and the anchor A are
known at t. U = A + 1 is the next quarter to be announced; the value uses only quarters U-4..U-23, all reported.
**The expected-announcement window is not derived from the realised date**: the DSL's `ea_days_to_expected` (existing
v9 field) takes, for the latest visible primary announcement, the pending atx-db `next_expected_date` of the CIK's
visible primary rows (yoy_364: first session >= that past announcement's session + 364 days, known at its own
acceptance; atx-db earnings_calendar docstring "PIT by construction"), fallback +91 days. The calendar stage does not
publish the realised future date point in time, and the field never reads it. One registered caveat remains (it is the
existing field's, unchanged): atx-db's `is_primary` label maps an announcement to a fiscal period with the latest
10-Q/10-K `report_date` of filings of any date, so whether a 2.02 row is primary can depend on a periodic report filed
after the announcement.

**Seal (both).** Every source row with `available_at` / `accepted_utc` >= the builder's `SEAL_NS` (research_window.py,
never a literal date) is dropped before use and counted (`rows_sealed`; the issuer events' builder key
`rows_available_on_or_after_2025_dropped`). The two stage files and the fundamental events are single files (no
year partitions), so they are opened whole and filtered by the reader, exactly as the 8-K fields read
`eight_k_items.parquet`. Tests: a seal mocked inside the role (sealed output equals the world without the post-seal
rows; unsealed they move it), and a fresh interpreter under the repository window (seal 2024-01-01) that drops every
synthetic row dated 2024+ as sealed and yields the same payload.

## 3. Build command (root runs it after the v8 freeze gate; not run by this lane)

On the 4-year role lo3, from root's fields v12 directory (74 fields per E-42), reusing everything; runbook R11 argv plus
`--price-source` (F-1's fields are in v12; its check needs the option even when every price field is reused), the
`--reuse` pins and the draft entry as the script. Variables as in the W0-2 runbook R10/R11.

```bash
F12DIR=build-equity/train-2020-2023-lo3-fields-v12
F12=$(sha $F12DIR/manifest.json)
F76=$("$PY" -c "import json,sys;n=[f['name'] for f in json.load(open(sys.argv[1],encoding='utf-8'))['fields']];print(','.join(n+['nt_first_126','earn_season_rank']))" $F12DIR/manifest.json)
"$PY" scripts/run_bounded_research.py --seconds 600 --max-rss-mib 2560 --min-free-mib 512 \
  --output build-equity/train-2020-2023-lo3-fields-v13-run \
  --bind atx-engine/tools/prepare_research_fields_draft.py --bind atx-engine/tools/research_fields_v9.py \
  --bind atx-engine/tools/prepare_research_fields.py --bind atx-engine/tools/research_fields_sec.py \
  --bind atx-engine/tools/research_fields_v8.py --bind atx-engine/tools/research_fields_price.py \
  --bind atx-engine/tools/research_fields_holdings.py --bind build-equity/train-2020-2023-lo3/manifest.json \
  --bind $F12DIR/manifest.json -- \
  "$PY" atx-engine/tools/prepare_research_fields_draft.py --role build-equity/train-2020-2023-lo3 --role-sha256 $LO3 \
  --output build-equity/train-2020-2023-lo3-fields-v13 --fields $F76 \
  --reuse $F12DIR --reuse-sha256 $F12 --reuse-hardlink \
  --finra $FINRA --tickerhistory $TH --finra-short-volume $SVRAW --price-source $TH \
  --identity-bridge $V1/export/identity-bridge-v2-pit --identity-bridge-sha256 $V2PIT \
  --fund-events $FE --fund-events-sha256 $FEV --fund-lag-sessions 1 \
  --sic-events $V1/fundamentals --sic-events-sha256 $SIC \
  --sec-stages $V1 --sec-identity-bridge $V1/export/identity-bridge-v2-pit --sec-identity-bridge-sha256 $V2PIT \
  --earnings-calendar-sha256 $EC --insider-sha256 $INS --sec-filings-sha256 $SECF \
  --thirteenf $V1/thirteenf --thirteenf-sha256 $F13 --ftd $V1/ftd --ftd-sha256 $FTD \
  --regsho-threshold $V1/regsho_threshold --regsho-threshold-sha256 $REGSHO \
  --security-master $V1/security_master --security-master-sha256 $SECM \
  --short-volume-ext $V1/short_volume_ext --short-volume-ext-sha256 $SVX \
  --max-rss-mib 2048 --max-seconds 580
F13M=$(sha build-equity/train-2020-2023-lo3-fields-v13/manifest.json)
```

Expected: `reuse.reused` = the 74 v12 names, `reuse.computed` = `["nt_first_126", "earn_season_rank"]`; names in
`F76` order; `seal.exclusive_end` 2024-01-01; no 2024-named source (R10 check); `source_checks.v9.nt_first_126.events
.rows_sealed` / `.filings.rows_sealed` and `source_checks.v9.earn_season_rank.fund_events.
rows_available_on_or_after_2025_dropped` are counts of dropped rows only. lo1: the same with the lo1 role, the issuer
bridge `build-equity/identity-bridge-r4-v2` / `$R4` and no `--sic-events` (R10 argv). Memory [est]: two link matrices of
nd x n x 5 B (about 42 MB each) plus the issuer events; `filings.parquet` is streamed (batches of 262,144 rows, four
columns), its size is unknown to this lane (no stage manifest was opened). Then K1: the two frozen `add-alpha` lines
(draft section 4) with `--fields build-equity/train-2020-2023-lo3-fields-v13` after the registry gets the two field
rows.

## 4. How root verifies (now, no build)

- Tests (synthetic): `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
  atx-engine/tools/test_research_fields_v9_nt.py atx-engine/tools/test_research_fields_v9_earn.py
  atx-engine/tools/test_prepare_research_fields_draft.py` -> 26 passed. Whole directory
  `atx-engine/tools` -> 279 passed (253 before + 26).
- Identity: `git diff --stat e9e1ee20 HEAD -- atx-engine/tools` lists only the five new files: the builder, every
  existing field module and every existing test are byte-identical. The builder's `code_sha256_lf` is `74df97f9...`,
  the R10/R11 fields-v9 builds' value. Producer fingerprints at head (= base, same bytes): builder `role 4f8767ff`,
  `finra a4b7744e`, `th 264eb1c9`, `lake 17c82861`, `issuer 9894e6d1`, `finra_sv 4090d3e0`; sec `af5fcf06`; price
  `3e415d7b / 945949d6 / 696ab31d / 9d870bf5 / ded50b81`; v8 `68c09181 / 15593c3b / 4b021ced`; holdings
  `cab3b9b4 / 9b2f42b6 / cc4cf935 / b1ceebb3 / 3cf03c85`. `test_prepare_research_fields_draft.py` pins: the plain
  builder lacks the draft names and module; the fields v9 lists (v71.json, base-lo1/lo3) hold none of them; registration
  appends exactly `FIELDS_V13_DRAFT` after every existing field with every existing spec object and formula id
  unchanged; every builder and module fingerprint is unchanged under registration **and under the promotion hook**
  (`import research_fields_v9 as _v9` / `FIELD_MODULES.append(_v9.bind(globals()))` appended to the builder source),
  while a module-level `ALL_FIELDS.update` would move the SEC fingerprint (the test's teeth). So the v8 builds' reuse
  counts cannot move.

## 5. Deviations and readings

- **Registration through a draft entry, not a builder hook.** "Off by default" is literal: `prepare_research_fields.py`
  is untouched, so no v8 output, manifest byte or fingerprint can move. Promotion (when v13 is real) is the two-line
  hook above; it would also need the tail assertions of `test_research_fields_v8.py:207` and
  `test_research_fields_v8_quarters.py:445` ("v8 fields are the last in ALL_FIELDS") widened.
- **Names.** Module `research_fields_v9.py` (platform-version convention of `research_fields_v8.py`, whose fields went
  into fields v11); draft list `FIELDS_V13_DRAFT` (next free fields version after v12).
- **F-L1.** "Usable" is the K8 rule exactly (the session after the first session whose 22:00 UTC mark follows
  `available_at`); the draft's "usable from the first session whose mark follows" read as that rule. "First" lookback
  includes original NT 20-F (any NT form of the CIK, as the draft says), strictly earlier `available_at`, the 365-day
  bound inclusive. Presence reads the same stage's `filings.parquet`: original forms of atx-db's `domestic` regime
  (10-K, 10-Q, 10-KT, 10-QT and the pre-2009 10-K405 / 10-KSB / 10-QSB / 10-KSB40 / 10-KT405), not only 10-K / 10-Q.
  The stage's `filer_regime.parquet` was not used: its segments merge future filings (`valid_to`), not point in time.
- **F-L2.** A = the anchor of the issuer row selected at t (the latest visible fiscal quarter), not "the latest quarter
  with a finite ni_q" (that would make U an already-announced quarter when the latest row lacks ni_q; A's own ni_q is
  never ranked). Added NaN when the selected row amends an older quarter than the latest visible one (A + 1 would then
  not be the next quarter); the house selection rule is otherwise unchanged. Out-of-domain values -> NaN (cannot occur).
- **Brief item "past returns around them".** Not used: the draft's definition is the seasonality of earnings levels
  (Chang et al. EarnRank) in an expected window; no return enters the field.
- **Task split.** Commit 1 holds an nt-only version of the two new files; commit 2 extends them; tests are commit 3
  (no TDD, owner directive).

## 6. Open risks

- `filings.parquet` is the largest file of the stage (all EDGAR forms 2009-2026, size unknown here): hashed and read
  in a stream; time [est] seconds to a minute; it is opened whole and its 2024+ rows dropped by the reader (as
  `eight_k_items.parquet`). If root prefers not to open it, the alternative is a presence rule on `events.parquet`
  alone (a different, weaker definition: a ruling).
- `nt_first_126` is a sparse flag (draft: well under 1% of names); the synthetic world says nothing about real
  coverage. NT forms outside the stage's event list (NT 11-K, NT 10-D, NT N-CEN) are not notices here.
- `earn_season_rank` ranks the just-announced quarter between an earnings release and its 10-Q / 10-K row (caveat);
  the candidate's 0..21-session window normally excludes that stretch (the next expected date is then ~60 sessions
  away), except for very late filers.
- The `is_primary` labelling caveat of `ea_days_to_expected` (existing, registered) carries into `earn_season`.
- `imported_code` reads the sibling sources from the same directory as the module; a checkout that mixes versions is
  caught (pin differs -> recomputed), never silently reused.
- Statistic counts of sealed rows appear in the manifests (as every SEC / issuer field already records them).

## Cross-lane edits

None. Only new files under `atx-engine/tools/` and the LIB3 draft doc `docs/plans/2026-10-01-v9-library-draft.md`
(status rows, section headers and field notes of C-3 / C-4, the count line, a "Built" note in section 3, K1 item 2;
the frozen `add-alpha` strings and DSL are unchanged).

## Hygiene and disclosures

- No data opened: nothing under `build-equity/` was read; no atx-db stage file or stage manifest was opened. File
  formats came from atx-db **source code** in the main checkout (read only): `C:/atx/atx-db/src/atx_db/alpha_panel/
  sec_filings.py` (events / filings / filer_regime schemas and SQL, `FORM_EVENTS`, `REGIME_FORMS`),
  `earnings_calendar.py` (the expected-date rule), `common.py` (stage manifest writer). These hold code constants such
  as the archive fetch date 2026-09-19, no statistic.
- Read for context: lane rules, the draft, task-LIB3-report, progress.md entries R7-a..c, PM3-5a, E-42, W0-n (and
  neighbouring lines), the W0-2 runbook R10/R11 argv, the integration-log R10/R11 rows (timings, peaks, 2020-2022
  coverage acceptance of fields v9: build statistics before 2024, no return or signal statistic), task-F-3 report argv.
- Synthetic fixtures: the nt world is dated 2021-2023 with rows after the role dated 2023-2025; the earn world is the
  v8 quarter world (2014-2021) plus synthetic 2024 rows for the repository-window test.

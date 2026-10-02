# task-PLAT report (tier1-v3 S1.1, S1.2, S1.3, S1.4, S0.3)

Lane PLAT. Branch `feat/tier1-v3-warehouse`. All paths below are under `C:/atx/atx-db`.

Package names: the plan's `atx_db/lake/` and `atx_db/parity/` shadowed the pre-existing modules `atx_db/lake.py`
(LakehouseExporter, used by `quality`, scripts and 5 test files) and `atx_db/parity.py`. Per the controller's
URGENT ruling the lake package is `atx_db/stagelake/` (CLI `python -m atx_db.stagelake ...`); for the same reason
I renamed `parity/` to `atx_db/parityscore/` (CLI `python -m atx_db.parityscore.scorecard`) in the same commit
8489f93b. **The parityscore ownership change needs controller acknowledgement.** `lake.py` / `parity.py` untouched.

## 1. What was built

| piece | files | notes |
|---|---|---|
| stage contract | `src/atx_db/stagelake/contract.py` | `Stage`/`Output` entry types, validation + topological order, manifest readers (files, legacy outputs, input bindings incl. legacy layouts, code SHAs), `bind_inputs()` helper for stage authors, `default_root()` |
| registry | `src/atx_db/stagelake/registry.py` | 29 seeded entries (every pre-sprint `data/alpha_panel/v1/**/*manifest*.json` + every `build.py` STAGES step, `BUILD_STEPS` map); `LAKE_STAGES` literals discovered in any `atx_db` module by AST parse (never imported), so lanes register without editing `stagelake/`; `load(strict=False)` for CLIs drops a broken declaration with a warning. Lanes had self-registered 6 more stages this way at report time (35 total). |
| root coverage | `src/atx_db/stagelake/coverage.py` | unregistered manifests / orphan Parquet / absent manifests, each with a ready-to-paste entry |
| verify | `src/atx_db/stagelake/verify.py`, `python -m atx_db.stagelake verify [--stage S]` | FAIL / STALE / WARN per stage (docs/LAKE.md); pyarrow + hashlib only |
| catalog | `src/atx_db/stagelake/catalog.py`, `python -m atx_db.stagelake catalog [--dump]` -> `data/catalog.duckdb` | views, `<view>_as_of(ts)` macros, `lake_cutoff(d)`, COMMENT ON, `lake_stages` / `lake_outputs` |
| orchestrator | `src/atx_db/stagelake/orchestrate.py`, `python -m atx_db.stagelake plan|run` | due = code / input binding / upstream; guard runner; binding sidecar `_lake/bindings/<stage>.json` |
| CLI, fixtures | `src/atx_db/stagelake/__main__.py`, `src/atx_db/stagelake/testing.py` | |
| parity catalog | `src/atx_db/parityscore/catalog.py` -> `src/atx_db/parityscore/catalog.csv` (370 rows) | 20 plan §1.1 domain rows, 161 design-spec items (26 IS, 30 BS, 15 CF, 20 supplemental/industry, 70 derived), 143 further seed items, 16 CRSP fields, 9 panel/ownership fields, 12 spec market/quality quantities, 4 lake-only items |
| scorecard | `src/atx_db/parityscore/scorecard.py` -> `docs/PARITY_SCORECARD.md` | numbers read verbatim from manifests / `metrics/coverage.parquet`, else `not measured` |
| docs | `docs/LAKE.md`, `docs/PARITY_SCORECARD.md` | |
| tests | `tests/test_lake_{registry,verify,catalog,orchestrate}.py`, `tests/test_parity_scorecard.py` | 34 tests, 50.9 s; 33 pass, 1 live-lake coverage test fails by design while other lanes have unregistered/unpublished stages (§4.3) |

Data written: `data/catalog.duckdb` + `data/catalog.duckdb.dump.txt` (canonical dump), `_lake/bindings/` only in
test fixtures. The live lake was never written by this lane (rebuild checks ran out of place, §4.1).

## 2. Done criteria

Stopped at controller OWNER STOP (2026-09-30 ~01:25 UTC). No job of mine is running or queued.

| task | criterion | status | measured |
|---|---|---|---|
| S1.1 | one entry per stage (name, module+args, schema, inputs, output globs, clock, staleness, vintage, guard cap, lane) | PASS | 29 seeded + 6 lane-registered (`LAKE_STAGES`) = 35 entries, all validate (DAG, topological order) |
| S1.1 | registration cheap (one data-only entry) | PASS | `LAKE_STAGES = [{...}]` literal in the stage module, AST-discovered; MKT (5) and TXT-classification (1) used it without touching `stagelake/` |
| S1.1 | unregistered-stage test tells the author what to add | PASS | `test_live_lake_every_manifest_and_parquet_file_is_registered_and_vice_versa` prints a ready-to-paste entry per gap; fixture tests cover both directions |
| S1.1 | every stage with a manifest is registered and vice versa | PASS for the 29 seeded stages; live test FAILS by design | live lake at report time: 10 unregistered manifests + 3 unregistered Parquet dirs + 4 registered-unpublished stages, all other lanes' in-flight work (§4.3) |
| S1.1 | 3 non-S0.1 stages rebuilt through normal CLI under guard, SHA-256 identical | **PARTIAL: 2 of 3** | corporate_actions 1/1 file identical; short_volume 9/9 partitions identical; ftd not completed (0.4 GiB cap hit, DuckDB OOM; no determinism finding) |
| S1.2 | `verify [--stage S]`: manifest completeness, file SHA, input binding, available_at present + max ≤ now, no .partial; FAIL vs STALE | PASS | 14 verify tests (one per failure kind + STALE + WARN); real run: 390 files / 8.79 GiB hashed in 74.7 s, peak 0.167 GiB |
| S1.2 | real FAIL/STALE list reported | PASS | FAIL 14, OK 8, PLANNED 3, WARN 10, STALE 0 (table below) |
| S1.3 | catalog.duckdb from scratch, view per output, `<stage>_as_of(ts)`, COMMENT ON from registry | PASS | 59 views, 51 `_as_of` macros + `lake_cutoff`, `lake_stages` / `lake_outputs` tables |
| S1.3 | rebuild < 10 min; two rebuilds byte-identical; read-only open works | PASS (run before the rename; not re-run after OWNER STOP) | 7.5 s and 5.34 s; both `d3a9e7a1…0574a` (798,720 bytes), dump `18f4cb3e…` identical; read-only 58/59 views at 200 MB (§4.4) |
| S1.4 | DAG; due iff code SHA or input-manifest SHA differs; `--dry-run` plan; `--run` through guard | PASS | 5 orchestrator tests, 12.4 s |
| S1.4 | two measured fixture cases | PASS | (a) new data in `src1` -> due exactly {mid, leaf1, leaf2}, `--run` executes those 3 in topological order, then nothing due; (b) code edit in `s_leaf3.py` -> due exactly {leaf3}; CRLF-only edit -> not due |
| S1.4 | one real `--dry-run` in report | PASS | due 27 of 35, 2.5 s (below) |
| S0.3 | parity `catalog.csv` with all columns, every §1.1 row and design-spec item | PASS | 370 rows (20 §1.1 domain rows, 161 design-spec items, 143 seed items, 16 CRSP, 9 panel/ownership, 12 market/quality, 4 lake-only) |
| S0.3 | scorecard renders measured coverage or `not measured`; fixture test; real render committed | PASS (v0) | 4 scorecard tests; `docs/PARITY_SCORECARD.md` 370 rows, 324 `not measured` (metrics/coverage.parquet unpublished) |

Tests: `tests/test_lake_{registry,verify,catalog,orchestrate}.py` + `tests/test_parity_scorecard.py`: 34 tests,
33 pass, 1 live-lake coverage test fails by design, 22.1 s total; ruff clean.

Commits: 61cdcb7e (S1.1), e6a9202b (S1.2), 01be882f (S1.3), 5fe21334 (S1.4), e6e6ef5f (S0.3), ba5ed50d (tolerant
CLI load), 8489f93b (rename to stagelake/parityscore; the file moves themselves were swept into 1819c5b2 by a
concurrent FUND commit), plus the final report/test commit.

### Resume instructions

1. **S1.1 third rebuild.** Either (a) ftd at a 0.6 GiB guard cap (needs controller OK, lane rule was ≤ 0.4):
   scratch root with no junctions, `ATX_ALPHA_PANEL_ROOT=<scratch> ATX_SHORTFLOW_DUCKDB_MEMORY=250MB
   python run_memory_guarded.py --job-gb 0.6 --receipt <new> -- python -m atx_db.alpha_panel.ftd build`, then compare
   `cusip_map.parquet` + `year=2013..2026/ftd.parquet` SHA-256 with the live files / `ftd/manifest.json`; or (b) a
   lighter non-S0.1 stage at 0.4 GiB (candidates not yet tried: `regsho_threshold`, `short_volume_ext`,
   `delisting`). Use junctions only for input dirs; remove them with `os.rmdir` before deleting the scratch root.
2. **S1.3 re-run after the rename** (guarded, 0.4 GiB): `python -m atx_db.stagelake catalog --dump` twice and
   compare SHA-256 (the view set grows as lanes publish; byte identity holds per lake state).
3. **S0.3 v1**: after S0.1 publishes `metrics/coverage.parquet`, rerun `python -m atx_db.parityscore.scorecard`
   and commit `docs/PARITY_SCORECARD.md`.
4. Re-run `python -m atx_db.stagelake verify --stage borrow_proxy` after the S0 borrow_proxy job publishes (its two
   `.partial` files were in-flight writes).

### S1.1 rebuilds (out of place, normal CLI, guard 0.4 GiB)

Method: `ATX_ALPHA_PANEL_ROOT` = scratch root; input stage dirs = NTFS junctions to the live lake; command = the
stage's own CLI under `run_memory_guarded.py --job-gb 0.4`; SHA-256 of every output compared with the live file
(hashed before the run) and the manifest's recorded SHA. Live lake never written.

| stage | command (env) | result | wall / guard run | guard peak | files, SHA-256 (live = rebuilt) |
|---|---|---|---|---|---|
| corporate_actions | `python -m atx_db.alpha_panel.corporate_actions` | **IDENTICAL** | 4.2 s | 0.202 GiB | `vendor_events.parquet` b57257f0a78ce6444a35060e4a0999b272fe163a603fa4abfc574a95435fad82 (= manifest) |
| short_volume | `python -m atx_db.alpha_panel.short_volume build` (`ATX_FINRA_DUCKDB_MEMORY=150MB`) | **IDENTICAL** (9/9) | 737.8 s (201 s queue + 534.6 s run) | 0.323 GiB | year=2018 14ed7898518deadca1060e93950070d4200f149aa470ae405359c2184a2961e9; 2019 3e67e9fd396d951f1f9c45121b79c70d4a764bf39909a5b6485af7b4796e070b; 2020 f862d0270c9a61a53eb38d6f90823ef1021a8bdc95dbc42d617d317e28e27b70; 2021 24c303033cb25a60444b80687e06facd16b63250b2d75be13e8a7a9393bbdc3f; 2022 845d3070aec182252586918e723b60a77208ab81cac72946eb8ad67c8509329b; 2023 93f909753bc41fe96f02157e74a30972075027945666de806fb471d0902d7d02; 2024 89fee39d47a824479ced75118ac66c6c40fe2acd24fa108de021cb85d3dc8063; 2025 bb85d635553bbd79a9ce5520af68a8875cc46ebd2908aa4aa730c4c2cfa3941d; 2026 a0f5fbf27ab55cee10781b6e01caf5b65ee09024d47dcde0faa4f2fed7952831 (legacy manifest records no SHA) |
| ftd | `python -m atx_db.alpha_panel.ftd build` (`ATX_SHORTFLOW_DUCKDB_MEMORY=250MB`) | **NOT COMPLETED** | 793 s (490 s queue + 302.6 s run) | 0.400 GiB, cap hit | `_duckdb.OutOfMemoryException: Out of Memory Error: Allocation failure` in `finra_fetch.sid0_repairs`; earlier: 150 MB `failed to pin block`, 250 MB `stopped_low_commit` (host) |

Other attempts (memory, not determinism): security_master (hard-coded 350 MB DuckDB, cap hit at 0.4),
earnings_calendar (`MemoryError` at 0.4, 2136 s), short_interest (DuckDB 150 MB OOM). Four earlier short_volume
attempts were stopped by the guard for host-wide low commit; their completed partitions (2018-2022) were already
identical.

### S1.2 real run

2026-09-30 01:17 UTC, `python -m atx_db.stagelake verify --json ...`, full SHA-256 of 390 files / 8.79 GiB in
74.7 s, peak commit 0.167 GiB (pyarrow + hashlib, no DuckDB: unguarded per C-1). Exit 1 (FAIL + coverage gaps).

`summary: FAIL 14, OK 8, PLANNED 3, WARN 10`; **STALE: none** (no stage's recorded binding differs from its input's
current manifest). No `sha_mismatch` / `size_mismatch` / `clock_future` anywhere. Findings: `input_unbound` 22 (WARN),
`legacy_clock` 10 (WARN), `incomplete_manifest` 8, `missing_manifest` 4, `unbound_file` 2, `partial_file` 2.

FAIL list (the controller's fix list):

| stage | FAIL finding | fix owner |
|---|---|---|
| identity, identity_names, identity_backfill | legacy manifest: no schema, status, code, files | ID (republish via `write_stage_manifest`) |
| prices | legacy manifest: no schema, status, code, files | MKT |
| short_interest | legacy manifest; `mapping_audit.parquet` has no recorded SHA | OWN |
| short_volume | legacy manifest: no schema, status, code, files | OWN |
| identity_table | `identity/multi_class_issuers.parquet` written but not bound by `link_table_manifest.json` | ID |
| export_fundamental_events, export_acceptance_bridge_pit_me | manifest has no `code` | FUND / S0 |
| borrow_proxy | `year=2020`, `year=2022` `borrow_proxy.parquet.partial` on disk (S0 borrow_proxy job was running at 0.8 GiB: in-flight write, re-verify after it publishes) | S0 |
| market_shares, market_liquidity, indexes, market_index | self-registered (`LAKE_STAGES`) but not yet published: publish or set `planned: True` | MKT |

WARN (not blocking): 22 `input_unbound` (legacy manifests record no binding for a registered input; e.g. panel does
not bind `identity/backfill_manifest.json`), 10 `legacy_clock` (`session_date`, `clock_utc`, `dissemination_date`,
`trade_date`, `accepted_utc`, `first_available_at`). An earlier run (00:10 UTC, 283 files / 4.68 GiB, 16.8 s, peak
0.164 GiB) gave FAIL 15 / OK 7 / PLANNED 4 / WARN 9, STALE 0. Report JSON: scratchpad `verify_final.json` (not
committed; rerun the command).

### S1.3 catalog

Guarded (0.4 GiB, peak 0.292 GiB, run 23.3 s), 2026-09-29 ~23:40 UTC, before the package rename (logic unchanged
by the rename; not re-run after OWNER STOP): two from-scratch builds 7.5 s and 5.34 s, both SHA-256
`d3a9e7a1aa5448d15c00f232884d67eedacde544173acf8a3b3f5cceb6d0574a` (798,720 bytes; DuckDB 1.5.5 stores the DB alias,
so identity holds when built under the same file name: `build()` builds in `_catalog_build/<name>` then
`os.replace`), canonical dump `18f4cb3e88d2f609171641a5b8e6cfa7426f8fec9854fed566ff615db89358bc` (843 lines) identical.
59 views, 51 `_as_of` macros, 12 registered outputs skipped (no files yet: borrow_proxy, characteristics,
classification x2, indexes x3, market x3, metrics x2). Read-only open (`memory_limit=200MB`, 1 thread): 58/59 views
answer `SELECT * LIMIT 1` (export_fundamental_events OOM, §4.4). Checks: `fundamentals_events_as_of(lake_cutoff(DATE
'2024-03-01'))` = 15,613 rows (latest event per cik; 356,340 events before the cutoff), `corporate_actions_as_of(
TIMESTAMP '2021-01-01')` = 74,175, `lake_cutoff(DATE '2024-03-01')` = 2024-02-29 22:00.

### S1.4 real `--dry-run`

2026-09-30 01:18 UTC, `python -m atx_db.stagelake plan --json ...`: 2.5 s, peak commit 0.028 GiB (no DuckDB, no
writes: unguarded per C-1).

```
due 27 of 35; fresh: ftd, sec_filings, insider, regsho_threshold, short_volume_ext, thirteenf, reference, prices_history
  1. identity             run   - code not recorded: identity_links.py
  2. prices               run   - code not recorded: prices.py
  3. short_interest       run   - code not recorded: short_interest.py
  4. identity_names       run   - code not recorded; input binding not recorded: identity, short_interest, prices
  5. identity_backfill    run   ...
  6. earnings_calendar    run   - input binding not recorded: identity_backfill; upstream due: identity_backfill, prices
  7. fundamentals         run   - code changed: fund_extract.py, fund_items.py, fundamentals.py (FUND lane v10 edits)
  8. identity_table ... 10. short_volume (code not recorded) 11. panel (own reason: identity_backfill unbound) ...
 14. classification       run   - upstream due: fundamentals
 17. export_acceptance_bridge_pit_me  manual
 19-21. export_identity_bridge_{all,pit,strict}  via identity_table
 23-26. market_shares, market_liquidity, indexes, market_index  (never built)
 27. metrics              run   - never built
```

Every "due" traces to a legacy manifest (no `code` / no bindings), a real code change (fundamentals v10), a
never-built stage, or propagation from those. The plan is truthful but not yet useful for incremental runs until
the legacy stages are republished once (by their lanes with `write_stage_manifest` + `bind_inputs`, or by one
orchestrated `run`, which writes the binding sidecar).

## 3. Sources

None landed. Disk: `data/catalog.duckdb` ~0.8 MB + dump ~0.1 MB. Scratch rebuild roots under the session temp dir
deleted after the checks (junctions removed first, never recursed).

## 4. Deviations and open issues

1. **Out-of-place rebuilds (S1.1).** Rebuilds ran through the stage's normal CLI under the guard with
   `ATX_ALPHA_PANEL_ROOT` = a scratch root whose input stage directories are NTFS junctions to the live lake, and
   outputs were hashed against the live files and their manifests. In-place rebuilds would have rewritten live
   manifests (new `git_head`), made every downstream binding STALE, and risked `os.replace` onto files other lanes'
   jobs had open. Manifests themselves differ by design (`built_at`, `git_head`); data files are compared.
2. **Guard ≤ 0.4 GiB limits which stages can be rebuilt**: security_master (hard-coded 350 MB DuckDB: cap hit),
   earnings_calendar (Python `MemoryError` at 0.4 GiB), short_interest (DuckDB 150 MB OOM) failed on memory, not on
   determinism. Several short_volume / ftd attempts were stopped by the guard with `stopped_low_commit` (host free
   commit 0.63-0.74 GiB while the job itself peaked 0.19-0.31 GiB): host pressure from other lanes, not the job.
   No stage was found non-deterministic.
3. **Registry test on the live lake fails now by design**, and its message prints the entry to add. At report
   time: unregistered manifests `classification_tnic/`, `events/`, `export/identity-bridge-v3-{all,pit,strict}/`,
   `identity/link_table_v3_manifest.json`, `options/`, `security_master/{cusip_history,listing_events}_manifest.json`,
   `stakes/`; Parquet without a stage under `notes/`, `nport/`, `thirteenf_filer_type/`; registered but unpublished
   `market_shares`, `market_liquidity`, `indexes`, `market_index`. Owners: ID, MKT, EVT, OWN, TXT, LIC, NOTES lanes
   (one `LAKE_STAGES` literal each). With the 29 seeded entries alone every pre-sprint manifest is registered and
   every seeded entry has its manifest.
4. **Catalog read-only check**: `export_fundamental_events` needs > 190 MB for `SELECT * LIMIT 1` because
   `fund_export` writes 262,144-row groups × 88 columns (lane rule: ≤ 32,768 for wide tables). Fix belongs to FUND
   (`fund_export.py`); readers can raise `memory_limit`.
5. **Panel binds 13 inputs but not `identity/backfill_manifest.json`** although it reads `links_combined.parquet`
   (identity_backfill): add it to `panel.INPUT_MANIFESTS` (S0 owner).
6. **identity_table** writes `identity/multi_class_issuers.parquet` without binding it in
   `link_table_manifest.json` (ID); **export manifests** (`export_fundamental_events`,
   `export_acceptance_bridge_pit_me`) record no `code` (FUND / S0).
7. `parityscore/catalog.csv` had to be `git add -f` (repo-wide `*.csv` ignore); `pyproject` package-data lists only
   `seeds/*.csv`, so a non-editable install would miss it (not my path).
8. `docs/PARITY_SCORECARD.md` is scorecard v0 without `metrics/coverage.parquet` (S0.1 metrics not published at
   render time): fundamentals items show the all-filing-events basis from the fundamentals manifest; rerun
   `python -m atx_db.parityscore.scorecard` after metrics publish to add the member_equity / linked-USD bases.
9. Plan §3 invariant 1 (`available_at` everywhere) is not met by 10 legacy outputs; `verify` WARNs (`legacy_clock`)
   rather than FAILs when the registry names the legacy clock, so the list stays visible without blocking.
10. `alpha_panel/build.py` is not removed or rewired (not my path); `stagelake run` is its registry-driven
    replacement.

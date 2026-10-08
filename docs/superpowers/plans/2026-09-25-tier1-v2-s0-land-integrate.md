# atx-db Tier-1 v2 — Sprint 0: Land, Review, Integrate

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax. **No TDD** (user directive): acceptance is a real-data or
> retained-file measurement recorded in the task report; tests only where the index §5.4 allows.

**Goal:** Turn the killed session-3 swarm's uncommitted work and review debt into reviewed commits on one consistent
HEAD, settle the production migration state, and pass one integration gate so Sprint 1 starts from a clean, known base.

**Architecture:** Each killed lane's work-in-progress is resumed in place by a fresh Opus implementer that owns exactly
that lane's files (resume rule: `git diff -- <owned files>` first, keep what is correct). Reviews of already-committed
tasks run in parallel with file-disjoint landings. One read-only guarded probe of the prod warehouse fixes the facts
the migration lane needs. The sprint ends with a pinned HEAD export and a collect-only plus focused-suite gate.

**Tech Stack:** Python, DuckDB 1.5.5, pytest (touched tests only), git.

**Spec:** `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` (read §3 constraints, §4 memory doctrine, §5
swarm rules). Nodes 0.1–0.15 of the index DAG.

## State update (2026-09-26): the killed lanes' WIP is committed

At the user's request all pending atx-db changes were committed as 11 per-lane **unreviewed WIP commits** on
`feat/tier1-parity` (base `da28fb60`). Wherever a task below says "`git diff -- <owned files>`", read the lane's WIP
commit instead (`git show <sha>`), then finish, test and commit on top of it. The WIP commits are not approved work:
each still goes through its task's review.

| Commit | Lane | Task |
|---|---|---|
| `f9743583` | 0327/0328 pin-by-swap (impl-a9f) | 0.2 |
| `52d57a99` | S1 standardization gaps (impl-p12) | 0.3 |
| `93080ef2` | P8 fix 1 + R1d clock change (impl-r1d) | 0.4 |
| `0069540e` | P9 fix 1 (impl-p3) | 0.5 |
| `de869e07` | RI2 stage (impl-ri1) | 0.6 |
| `f1dbf52c` | P1 fix 1 (impl-ri1; test file also carries impl-a8f's `currency_status` assert) | 0.6 |
| `0e8cb43d` | `currency_status` writer (impl-a8f) | 0.9 |
| `3c718845` | W2 lineage writers (impl-p4) | 0.9 |
| `a7e23ff3` | E1 adapters (impl-r2e) | 0.7 |
| `807ef7ef` | R2d fix 1 (impl-r2e) | 0.7 |
| `36c8059f` | P13 fix 1 (impl-c4) | 0.8 |

Known breakage at this HEAD until S0 lands: the 0327 checksum changed (`test_migrations.py` pin, templates); E1 pins
stale source versions; `test_statement_map_seed.py:44` count pin (331 vs 344); R2d Feb DDD assertion; P1 tests absent.

## Global Constraints

Every task implicitly includes the index §3 (safety, SEC user agent `atx-db/0.1 atx-research@example.com` only,
Write/Edit-only edits, SERIALIZED files, WAL rules, never open the prod warehouse without the HEAVY token), §4 (memory
doctrine) and §5 (ownership tokens, concurrency ≤ 8 agents, agent memory gate phys ≥ 1.8 / commit ≥ 3.6 GiB,
`OPENBLAS_NUM_THREADS=1`, commit hygiene, trailer `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`).

Paths are relative to `C:\atx\atx-db\` unless they start with `.superpowers/` or `docs/` (repo root `C:\atx`).
Session-3 reports and reviews for every lane are in `.superpowers/sdd/tier1-parity/claude-ctl/` (`<ID>-report.md`,
`<ID>-review.md`) and scratch logs in the old session scratchpad; `grep` the ledger
`.superpowers/sdd/tier1-parity/claude-ctl/progress.md` for `Ruling:` lines about your task id rather than reading it
whole.

## Review Focus

1. **Foreign hunks in owned files.** Several WIP files carry another lane's edits (e.g. `tests/test_market_owner_bridge.py`
   has impl-a8f's `currency_status` assert inside P1's file). A reasonable person expects every commit to contain only
   its task's work, with any shared file coordinated.
2. **Checksum drift of reviewed migrations.** Editing the reviewed 0327 body changes its checksum; templates, the
   `test_migrations.py` pin and any scratch DB built at 0327 become stale. Expected: one owner re-baselines all of them in
   the same commit and the 0327 delta gets its own review.
3. **Stale version pins between producer and consumer.** E1 pinned events v2 / factors v3 / ownership v1 while
   producers are at v3 / v4 / v2. Expected: every consumer of a bumped contract is updated in the same sprint and the
   integration gate proves it.
4. **Non-deterministic stage output.** RI2's real-data proof failed twice with "revision already held with other
   content (12,650 rows)". Expected: two runs over the same pinned inputs produce byte-identical evidence rows.
5. **PIT leakage in corporate actions.** P8 as committed stamps final labels at the ex-date clock. Expected: a pending
   row at the ex-date and resolutions only at their own decision clocks, visible through as-of readers.

---

### Task 1: [0.1] Production warehouse state probe (read-only, guarded)

**Lane:** OPS · **Tokens:** HEAVY (read-only) · **Depends on:** — · **Peak target:** 0.6 GiB

**Files:**
- Create: `C:\Users\natha\AppData\Local\Temp\claude\c--atx\<session>\scratchpad\probe_prod_state.py` (scratch, not repo)
- Create: `.superpowers/sdd/tier1-v2/receipts/prod-state-<UTC timestamp>.json`

**Interfaces:**
- Produces: a JSON receipt with keys `applied_migrations` (max version + full list), `registry_head`,
  `wal_present` (bool, from the filesystem before opening), `table_rows` (for `sec_company_facts`,
  `fundamental_points`, `equity_daily_bars`, `sec_submissions`, `fundamental_standardized`, `derived_metric_values`,
  `market_daily_metrics`, `equity_price_metrics`, `forward_returns_survivorship_safe`, `schema_contract`),
  `unit_inventory` (output of the existing A9 `ticker_history_unit_inventory` read-only function), `db_bytes`,
  `free_disk_bytes`. Consumed by 0.2 (which migrations are unapplied), 1.2 and 1.8.

- [ ] **Step 1:** Check `C:\atx\atx-db\data\warehouse.duckdb.wal`. If it exists, **stop** and report: a stale WAL needs the
  recovery procedure (PRODUCTION_RUNBOOK "migrate died between COMMIT and checkpoint") before any open.
- [ ] **Step 2:** Write the probe with the Write tool. It opens the warehouse with
  `duckdb.connect(path, read_only=True, config={"memory_limit": "384MB", "threads": 1})`, reads the applied-migration
  table (find its name with `grep -n "CREATE TABLE" src/atx_db/migrations/_runner.py`), counts rows for the listed
  tables, and calls `ticker_history_unit_inventory` (`grep -rn "def ticker_history_unit_inventory" src/atx_db`). Import
  only what it needs; set `os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")` before any import.
- [ ] **Step 3:** Run it under the guard exactly the way `.superpowers/sdd/tier1-parity/claude-ctl/launch-archive21.ps1`
  invokes `run_memory_guarded.py`, with `--job-gb 0.6`, from a git-archive export of HEAD (RX3 rule: heavy launches run
  from a pinned export — create `.superpowers/sdd/tier1-parity/exports/<sha8>` with `git archive`). Record the guard
  receipt path.
- [ ] **Step 4:** Write the JSON receipt and a 10-line summary to the task report. If any vendor-share row > 1e8 appears
  in the unit inventory (A9 ceiling), flag it: B0 would abort on it.

**Acceptance:** receipt exists; `applied_migrations.max` is stated (resolving the 0322-vs-0326 discrepancy);
`wal_present` is false; guard peak ≤ 0.6 GiB.

---

### Task 2: [0.2] MIG-A — register 0328, WAL-safe pin refresh, per-migration CHECKPOINT, contract scratch exclusion

**Lane:** MIG · **Tokens:** MIG · **Depends on:** 0.1 · **Peak target:** 0.4 GiB (template runs 345 MB, alone)

**Files (WIP from killed agent impl-a9f; start with `git diff`):**
- Modify: `src/atx_db/migrations/bodies_0327.py` (WIP +98/−2: `_replace_rows_by_swap`, `refresh_schema_contract_pin_by_swap`)
- Modify: `src/atx_db/migrations/bodies_0328.py` (WIP +13/−4)
- Modify: `src/atx_db/migrations/registry.py` (register 0328; head 327 → 328)
- Modify: `src/atx_db/migrations/__init__.py` if registration requires it
- Modify: `src/atx_db/migrations/_runner.py:333-347` (CHECKPOINT after each migration COMMIT)
- Modify: `src/atx_db/migrations/bodies_0323.py`, `bodies_0324.py`, `bodies_0325.py` — **only if 0.1 shows they are unapplied in prod**
- Modify: `src/atx_db/schema_contract.py:318-337` (`_fetch_live_tables`)
- Modify: `src/atx_db/schema.py` (`COMMON_FIELD_DESCRIPTIONS["rdq"]`; `_seed_field_catalog`)
- Modify: `src/atx_db/jobs.py:~1525` (corporate_actions dependency)
- Modify: `tests/test_migrations.py` (checksum pins)

**Interfaces:**
- Consumes: 0.1 receipt (`applied_migrations`).
- Produces: registry head 328; `refresh_schema_contract_pin_by_swap(conn) -> None` (public helper in `bodies_0327.py`,
  used by every later migration that re-pins the contract, including 0329+); runner guarantee "each migration is
  COMMITted then CHECKPOINTed before the next starts".

- [ ] **Step 1:** `git diff -- src/atx_db/migrations/ src/atx_db/schema.py`; read `MIG0328-report.md` and the A9 review for
  the WAL-replay ART hazard ("the next write invalidates the database").
- [ ] **Step 2:** Finish the swap helper so 0327 and 0328 re-pin `schema_contract` / `schema_contract_version` by
  create/copy/swap (drop and recreate their ART indexes inside the swap), never by in-place UPDATE/DELETE. If 0.1 shows
  0323–0325 unapplied, switch their `_refresh_schema_contract_v2_pin` calls to the swap helper too; if applied, leave
  their bodies byte-identical and record a ruling.
- [ ] **Step 3:** Register 0328 in `registry.py`. Add `CHECKPOINT` after each migration's COMMIT in `_runner.py`.
- [ ] **Step 4:** In `_fetch_live_tables`, exclude scratch tables so the contract stops certifying drift:
  ```python
  _SCRATCH_TABLE_PATTERNS = ("\\_ss\\_%", "%\\_\\_rebuild\\_%", "%\\_\\_swap", "%\\_\\_next", "%\\_bulk\\_stage")
  # WHERE NOT (table_name LIKE ANY pattern ESCAPE '\\')
  ```
- [ ] **Step 5:** `schema.py`: update the `rdq` description to the FP-rdq rule ("rdq is NULL when no 8-K Item 2.02 exists;
  rdq_basis/rdq_available_at/rdq_accession_number carry lineage") and make `_seed_field_catalog` insert only missing
  rows (never `INSERT OR REPLACE` descriptions a migration set).
- [ ] **Step 6:** `jobs.py:~1525`: map the corporate_actions job dependency to the table the stage actually reads
  (`equity_daily_bars`, per P8-review M3), not `tbltickerhistory_daily`.
- [ ] **Step 7:** Re-baseline checksum pins in `tests/test_migrations.py` and rebuild the conftest template once (~370 MB,
  run alone).
- [ ] **Step 8 (e2e, allowed):** WAL-replay drill on a scratch copy of a template at 0326: run the upgrade in a
  subprocess, terminate it right after the 0327 COMMIT log line (before its CHECKPOINT would matter, i.e. patch nothing —
  terminate on the log line), reopen with the bounded config, then perform one `schema_contract` write and `verify_schema`.
  Expected: reopen succeeds, write succeeds, `verify_schema == ()`. Then run the slow upgrade test `[326 → 328]` once.
- [ ] **Step 9:** Commit with pathspecs; report lists the new checksums of 0327/0328 and whether 0323–0325 changed.

**Acceptance:** registry head 328; drill passes; slow upgrade passes; `git grep -n "_refresh_schema_contract_v2_pin"`
shows no call site in any migration that is unapplied in prod. Review scope: the 0327 delta, 0328 registration, runner.

---

### Task 3: [0.3] Land S1 — standardization gaps from the FSDS benchmark

**Lane:** F · **Tokens:** STD · **Depends on:** 0.2 · **Peak target:** 0.4 GiB (template rebuild ~370 MB alone)

**Files (WIP of impl-p12):**
- Modify: `src/atx_db/_standardization_set_based.py` (+170/−29), `src/atx_db/standardization.py` (+106/−10),
  `src/atx_db/fsds_baseline.py` (+107/−43)
- Modify: `src/atx_db/seeds/standardization_rules.csv`, `statement_map.csv`, `concept_map.csv`, `fundamental_items.csv`
- Modify: `docs/measurements/FSDS_BENCHMARK.md`
- Modify: `tests/test_standardization.py`, `tests/test_standardization_composition_pit.py`
- Modify: `tests/test_statement_map_seed.py:44` (count pin 331 → 344; **now owned by STD**)

**Interfaces:**
- Produces: rule inputs as `{"input": "output"}` references; dependency levels with a cycle check at seed load; n-ary
  `difference`; `zero_fill_subtrahends`; level-by-level composition. Seeds: `PaymentsToAcquireProductiveAssets` → capex
  1305 fallback (priority 50, removed from 1306); capex multipliers consistently −1.0; derived `stockholders_equity` /
  `common_equity` = SE(output) − preferred; `equity_incl_NCI` = SE + NCI; `total_liabilities` = L&SE −
  equity_incl_NCI(output); revenue ← utility 1801 output.

- [ ] **Step 1:** `git diff` all owned files; read `S1-*` logs and `P12-report.md`.
- [ ] **Step 2:** Update the count pin in `test_statement_map_seed.py` and fix the failing assertion at
  `test_standardization.py:518` if still failing.
- [ ] **Step 3:** Digest proof: pre-existing rule outputs unchanged. Build a template at head 328 (after 0.2), run
  the WIP `S1-probe_digest.py` approach (rule outputs for all rules not touched by S1 are byte-identical between
  HEAD~ seeds and WIP seeds on the FSDS subset DB). Record differing rules — only the intended ones may differ.
- [ ] **Step 4:** Re-run the P12 FSDS 50-filer benchmark (`fsds_baseline.py`, 384MB/2 threads, ~251 MB measured).
- [ ] **Step 5:** Run the two touched test files once; commit; update `FSDS_BENCHMARK.md` with the new numbers.

**Acceptance:** FSDS cells mapped > 84.5% (target ≥ 90%); capex, operating income, gross profit and common equity
coverage reported separately against 62% / 58% / 52% / 217-of-250 baselines; digest proof lists only intended rules.

---

### Task 4: [0.4] Land P8 fix 1 — revisioned per-step corporate-action events (+ R1d clock change)

**Lane:** K · **Tokens:** — · **Depends on:** — · **Peak target:** 0.6 GiB (P8 build 221 s / 585 MB measured)

**Files (WIP of impl-r1d):**
- Modify: `src/atx_db/corporate_actions.py` (+301/−122), `src/atx_db/_split_epochs.py` (+70/−31),
  `src/atx_db/daily_adjustments.py` (+58), `src/atx_db/adjustment_factors.py` (+11/−4), `src/atx_db/asof/pricing.py`
  (+10/−4), `src/atx_db/corporate_action_metrics.py` (+5/−1), `scripts/build_corporate_action_events.py` (+7/−5)
- Modify: `tests/test_corporate_actions.py` (+56/−27)

**Interfaces:**
- Produces: `corporate_actions_asof_sql(cutoff)` and `corporate_actions_current_sql()`; per-step revision columns
  `supersedes_event_id`, `superseded_at`; pending states `split_candidate_pending`, `distribution_pending_classification`;
  `factor_series_sql` exported from `_split_epochs.py`; a stable **per-step event id** (0.8 uses it as the release
  natural key).

- [ ] **Step 1:** `git diff`; read `P8-review.md` (I1–I3, M1–M7) and the P8/R1d rulings.
- [ ] **Step 2:** Verify the WIP covers I1 (tie-out raises **before** the transaction commits), I2 (pending row at the
  ex-date clock, resolutions at their own decision clocks, XBRL corroboration never before the fact's clock), I3
  (table-wide factor-series set; other-series bars counted), M1 (`factor_noise` < 1e-5), M4 (refuse keep-legacy), M5 (DPS
  staged once; USD/share only), M6 (OTHER excluded from cumulative factors).
- [ ] **Step 3:** M2: steps with non-integer ratios or q > 10 at year-end on leveraged-ETF-like lines are labeled
  `split_ratio_implausible` (never applied as splits). Record a ruling.
- [ ] **Step 4:** Document in the report exactly how `_split_epochs.py` changes R1d's confirmation clocks (share evidence
  never known before the pre-step run; split-derived tolerance tied to A8 `SPLIT_RUN_TOLERANCE`/`SPLIT_FACTOR_MIN`;
  in-band matches take the family lag) — the reviewer must review this at R1d depth.
- [ ] **Step 5 (real data):** rebuild on the retained Parquet into a scratch DuckDB (384MB, 2 threads):
  `scripts/build_corporate_action_events.py` against `data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet`.
- [ ] **Step 6:** Run `tests/test_corporate_actions.py` once; commit.

**Acceptance (retained data):** tie-out ≤ 1e-6 on all 25,760 lines; an injected breach (one factor perturbed by 1e-3)
aborts with no rows committed; for the 1,933 late splits the as-of at ex+1 shows the pending row and the as-of at the
confirmation clock shows the split; 0 rows with `xbrl_dps` corroboration before the fact's own clock; counts of cash
dividends / splits / stock dividends / unclassified reported against 370,973 / 3,100 / 60 / 1,149. Review: P8 + R1d depth.

---

### Task 5: [0.5] Land P9 fix 1 — ownership identity and features

**Lane:** K · **Tokens:** — · **Depends on:** — · **Peak target:** 0.4 GiB

**Files (WIP of impl-p3):**
- Modify: `src/atx_db/ownership_identity.py` (+280/−97), `src/atx_db/research/ownership_features.py` (+287/−128)
- Modify: `tests/test_ownership_identity.py`, `tests/test_research_ownership_features.py`

**Interfaces:**
- Produces: `QUERY_VERSION = "research-ownership-features-v2"` (the exact string E1 pins in 0.7); `owner_basis` in
  {`linked_primary`, `unlinked_line`}; survivor-conditioning label and blocker.

- [ ] **Step 1:** `git diff`; read `P9-review.md` (I-1…I-5, M1–M11).
- [ ] **Step 2:** Confirm the WIP implements: current CUSIP needs ≥ 3 filers + dominant name share ≥ 0.8 + a current SEC
  title; common-title regex with non-common exclusion, CUSIP mod-10 check and placeholder rejection; dated TICKER
  evidence only (`primary_symbol` diagnostic only); FINRA revised rows get a modeled clock; `split_in_period` NULL
  including R1d exact splits; line-level SI including unlinked lines.
- [ ] **Step 3:** Run the two test files once; commit. List unaddressed M-items in the report with a proposed ruling each.

**Acceptance:** tests pass; report lists each I-finding → code location. (No real 13F/FINRA data exists yet; real-data
acceptance happens in 4.5.)

---

### Task 6: [0.6] Land RI2 (identity-reconstruction stage) and P1 fix 1 (rule-5 preferred lines)

**Lane:** K · **Tokens:** ACT · **Depends on:** — · **Peak target:** 0.5 GiB (RI1 stages 480–501 MB; scratch DB 256MB)

**Files (WIP of impl-ri1 and impl-a8f):**
- Modify: `src/atx_db/activation.py` (+66/−1; SERIALIZED, ACT), `src/atx_db/identity_reconstruction.py` (+357/−25)
- Create: `tests/test_identity_reconstruction_stage.py` (untracked, 147 lines)
- Modify: `tests/test_activation_ledger.py` (+1), `tests/test_listing_ingestion_pit.py` (+5/−4), `docs/PRODUCTION_RUNBOOK.md` (+28/−2)
- Modify: `src/atx_db/market_owner_bridge.py` (+172/−25), `tests/test_market_owner_bridge.py` (contains impl-a8f's
  `currency_status` assertion — keep it, attribute it in the commit message, and commit it with 0.9's writer landed or
  after it)

**Interfaces:**
- Produces: activation stage `identity_reconstruction` (before `market_daily`; run5 stage suffix 23 → 24) writing
  append-only, idempotent revisions to `security_identity_evidence`; bridge rule 5 with `non_common_line` class over every
  traded symbol (`line_symbol_history`), `overlapping_reconstruction` guard, never-visible counts, first-visible dedupe
  order, label `current_ticker_unverified+reconstructed_history`.

- [ ] **Step 1:** `git diff`; read `RI2-prove*.log`, `RI1-report.md`, `P1-review.md`.
- [ ] **Step 2 (determinism):** Run the RI1 build twice into two scratch DuckDBs (256MB, 2 threads) from the same pinned
  inputs (TickerHistory3.parquet sha `0ed96b26…`, companyfacts.zip, submissions.zip). Diff per-row content hashes of
  `security_identity_evidence`. Find the unordered step (candidate causes: `row_number()` without a total order,
  set/dict iteration order, float summation order) and make it deterministic; if code changed between the two failed
  proofs, bump `METHOD`. Acceptance: two consecutive runs produce identical revision content, and the writer accepts
  the second run as a no-op.
- [ ] **Step 3:** P1 tests: the report's promised tests are absent. Write only the complex ones: (a) a preferred line
  (NHPAP-like) is `non_common_line` and claims no CIK-days; (b) two reconstructions overlapping one CIK trigger the
  overlap guard; (c) first-visible dedupe never uses hindsight.
- [ ] **Step 4 (real data):** re-measure attrition on retained data with the default tier (high+medium at
  `tier_at(cutoff)`) and high-only.
- [ ] **Step 5:** Collect-only of the bridge tests from a HEAD export; run the touched tests once; two commits (RI2, P1).

**Acceptance:** determinism proven; 0 preferred/warrant/unit lines linked as common (NHPAP `TBLTICKERHISTORY-6197456`,
TECTP `-5947420` withheld); operating-company attrition ≤ 12.0% (high-only reported, ~27.5%); 111 never-visible lines
counted, not claimed.

---

### Task 7: [0.7] Land E1 (external research sources) + R2d fix 1 + the VA1 panel change

**Lane:** R · **Tokens:** FEAT, PANEL · **Depends on:** 0.5 · **Peak target:** 0.5 GiB

**Files (WIP of impl-r2e):**
- Modify: `src/atx_db/research/features.py` (+312/−16), `tests/test_research_features.py` (+179/−2)
- Modify: `src/atx_db/research/panel.py` (+125/−30), `tests/test_research_panel.py` (+7/−1)

**Interfaces:**
- Consumes: source contracts at HEAD — read them from `research/events.py` (~:144), `research/factor_returns.py`
  (~:172) and `research/ownership_features.py` (~:101, v2 after 0.5). Do not hard-code from memory.
- Produces: `write_derived_feature_version(conn, *, spec, rows) -> str` — a **public** writer replacing the private R2b
  helpers that P5 imports (P5 M6); source kinds `event`, `factor_exposure`, `ownership` with pinned sealed versions;
  panel `QUERY_VERSION` bumped (v7 → v8) with P2 natives reading **VA1-repaired** bars.

- [ ] **Step 1:** `git diff`; read `E1-report.md`, `R2d-review.md`, `VA1-report.md` (panel change routed to this lane).
- [ ] **Step 2:** Fix the adapter pins in `features.py` (~:767-769) to the current contracts.
- [ ] **Step 3:** Add the public writer and use it internally.
- [ ] **Step 4:** R2d: keep I1 (withhold `line_market_cap` on market_daily verdicts `adr_ratio_unresolved`,
  `dei_archive_conflict`), m1 (bar-count chunks scaled to the memory limit), m4 (share-bridge digest), m5 (invisible ADS
  basis → `adr_ratio_unknown`); flip the Feb DDD assertion to the post-b4345421 behavior (`split_pending_share_update`).
- [ ] **Step 5:** Apply the VA1 panel change (P2 natives read repaired bars via the `_vendor_artifact` helper) and bump
  `QUERY_VERSION`.
- [ ] **Step 6:** Run the two test files once; two commits (E1; R2d+VA1 panel).

**Acceptance:** E1 test passes against current contracts; `grep -n "research-earnings-events-v2\|factor-returns-v3\|ownership-features-v1" src/`
returns nothing; the panel test covering 2021-01-04 shows repaired natives.

---

### Task 8: [0.8] Land P13 fix 1 — release/API extension and 0328 eligibility columns

**Lane:** Q · **Tokens:** — · **Depends on:** 0.2, 0.4 · **Peak target:** 0.4 GiB

**Files (WIP of impl-c4):**
- Modify: `src/atx_db/publication.py` (+173/−36), `src/atx_db/api/catalog.py` (+18/−47)
- Modify: `tests/test_publication.py` (+69/−14; also the VA1 `forward_return_publication_v2` literals at ~276 and ~832),
  `tests/test_api_service.py` (+20/−12)

**Interfaces:**
- Consumes: P8 per-step event id (0.4) as the corporate-action natural key; 0328 columns present at head 328.
- Produces: CA events withheld until P8 is reviewed; `listing_status` capped at `candidate` (`eligibility_cap`);
  `market_daily` natural key includes `source`; `include_pending` only for publication; forward-label version allowlist;
  optional dataset schema change → withheld, not fatal; guarded DETACH; eligibility columns written only when present
  (`_present_columns()`).

- [ ] **Step 1:** `git diff`; read `P13-review.md` (I1–I3, m1–m8).
- [ ] **Step 2:** Update the two VA1 v2 literals to the v3 contract; set the CA natural key to the per-step id.
- [ ] **Step 3:** Run the two test files once; commit.

**Acceptance:** tests pass; a publication on the test template shows CA withheld, `listing_status` ≤ candidate and
`market_daily` keyed by source.

---

### Task 9: [0.9] Land W2 (0328 lineage-column writers) and the `currency_status` writer

**Lane:** F/K · **Tokens:** — · **Depends on:** 0.2 · **Peak target:** 0.3 GiB

**Files (WIP of impl-p4 and impl-a8f):**
- Modify: `src/atx_db/fundamental_statements.py` (+21/−10: `rdq_basis`, `rdq_available_at`, `rdq_accession_number`)
- Modify: `src/atx_db/reference_classifications.py` (+20/−4: `classification_basis`, `mapping_version`)
- Modify: `src/atx_db/market_daily.py` (+16/−7: `currency_status`)

- [ ] **Step 1:** `git diff`; confirm each writer emits the column only when it exists (pre-0328 warehouses still work).
- [ ] **Step 2:** Probe on a head-328 template: write one stage run for each writer and select the new columns.
- [ ] **Step 3:** Commit (three files). Tell 0.6's owner the `currency_status` assertion in
  `tests/test_market_owner_bridge.py` now has its writer.

**Acceptance:** probe shows non-NULL lineage columns on the template; no change to row counts.

---

### Task 10: [0.10] CB2 — catalog rows for P3/P4/P9 features, new source kinds, and research metadata columns

**Lane:** R · **Tokens:** CAT · **Depends on:** 0.7 · **Peak target:** 0.3 GiB (catalog economics test ~345 MB alone)

**Files:**
- Modify: `src/atx_db/seeds/research_anomaly_catalog.csv`, `src/atx_db/research/catalog.py:~123` (`SOURCE_KINDS`, validators)

**Interfaces:**
- Produces (consumed by 1.10, 1.12, 2.10, 3.9, 4.3):
  - rows: `ear_m1p1`, `runup_m21_m2`, `days_since_announcement`, `sue_ni_event` (mutually exclusive with `sue_ni`: keep
    one as primary, record which), `beta_mkt_252d`, `ivol_252d`, `io_ratio_13f`, `io_change_13f`, `breadth_change_13f`,
    `short_interest_ratio`, `days_to_cover_si` (exact definitions in `P3-report.md`, `P4-report.md`, `P9-report.md`,
    `E1-report.md`);
  - vocabulary: classes `ownership`, `event_timing`; caveat `io_above_one`; clock `finra_publication_modeled`;
    `SOURCE_KINDS += ("event", "factor_exposure", "ownership")`;
  - **new metadata columns for every row** (default shown): `population` (`all`; conditional features name their
    population, e.g. `rd_reporters`, `dividend_payers`, `inventory_holders`, `advertising_reporters`), `evidence_class`
    (`replication` for published anomalies/analogues with a pre-registered sign, `discovery` for
    `economic_conjecture` rows), `publication_year` (parsed from `reference`; NULL when not a published anomaly),
    `jkp_theme` (one of the 13 JKP themes or `none`), `wave` (`w0_existing` for existing rows).

- [ ] **Step 1:** Add the columns and validators; fill `publication_year` and `jkp_theme` for all rows (use the reference
  column; the JKP theme list is: accruals, debt_issuance, investment, low_leverage, low_risk, momentum, profit_growth,
  profitability, quality, seasonality, size, short_term_reversal, value).
- [ ] **Step 2:** Add the 11 rows.
- [ ] **Step 3:** Run the catalog test file once (alone, ~345 MB); commit. Record the new catalog digest in the report.

**Acceptance:** validator accepts all rows; 186 rows total (183 eligible unless a row is blocked with a reason); every
eligible row has `evidence_class`, `population`, `jkp_theme`.

---

### Task 11: [0.11] Review P5 fix 1 + R3d; finish the P5 store-path smoke

**Lane:** REV (+ P5 owner for fixes) · **Depends on:** 0.7 · **Peak target:** 0.4 GiB (P5 e2e 371 MB alone)

**Scope:** commits 64e4ed10, 753e4cf8 (P5) and da28fb60 (R3d). Files under review: `src/atx_db/research/composites.py`
and its tests.

- [ ] **Step 1 (implementer, P5 owner):** switch `composites.py` from private R2b helpers to
  `write_derived_feature_version` (P5 M6); run `run_build_forward` end to end on the fixture store (open_basis_inputs +
  run_evaluation + StoreSource; P5 M7); commit.
- [ ] **Step 2 (reviewer):** review 753e4cf8 + R3d + Step 1 against `P5-review.md` C1, I1–I4, M1–M11; confirm the null
  simulation (v2 selection passes 0.061 vs nominal 0.074) is reproducible from the committed script.
- [ ] **Step 3:** ≤ 2 fix rounds; record residuals (weights fit on the selection sample; in-memory
  `evaluate_composites` repeatability) as parked findings with rulings.

**Acceptance:** review approved or adjudicated; store-path smoke output in the report.

---

### Task 12: [0.12] Review R1e — split lookup via the bridge

**Lane:** REV · **Depends on:** 0.6 · **Scope:** f3830159

- [ ] **Step 1:** Review against R1e-report concerns: tier used is the segment tier at the ex-date cutoff (no
  `tier_at(frame clock)`), production breadth unmeasured (probe upper bound 5,123 → 8,316 owners, 32.7% → 53.1%), bridge
  built once per derived refresh, ADR owners get no epochs, multi-class disagreement hazard (123 of 137, upper bound).
- [ ] **Step 2:** Fix rounds by the R1e owner (≤ 2). A `tier_at(frame_clock)` API may be required: if so, it is a
  finding for 3.3.

**Acceptance:** approved or adjudicated with rulings.

---

### Task 13: [0.13] Review VA1 (vendor 2021-01-04 artifact repair) + A8 follow-up

**Lane:** REV · **Depends on:** 0.9 · **Scope:** 5272dfae (VA1, which also carries A8's `market_daily.py` follow-up
b4345421 content)

- [ ] **Step 1:** Review both tasks' logic in the one commit: VA1 neutralization in every adjusted-close return builder;
  A8 follow-up (split-derived runs wait for the prior run's availability; runs never precede their bars).
- [ ] **Step 2:** Open items to resolve or rule: ≥ 13 test lines pinning `forward_return_publication_v2` outside
  `test_publication.py` (`tests/test_fundamental_signal_evaluation.py` 112,125,237,379,408,420;
  `tests/test_historical_identity.py:825`; `tests/test_desk_question_pack.py` 534,549; `evaluation.py:122` docstring);
  `market_daily` has no version column; 16 lines on 2021-01-04 with exact-ratio / flat-price steps remain wrong by up to
  −90% (proposal: treat an exact-ratio step without a raw price follow-through as an artifact).
- [ ] **Step 3:** Fix rounds (≤ 2) by the owner of each file.

**Acceptance:** approved or adjudicated; v2 literals gone from the test suite.

---

### Task 14: [0.14] Review P14 — incremental TickerHistory3 partitions + rebase ledger

**Lane:** REV · **Depends on:** 0.2 · **Scope:** 8aef02ae, 251f1188 (`src/atx_db/ticker_history_incremental.py`)

- [ ] **Step 1:** Review against the P14 report residuals: bulk republish must be newer than every ledger row;
  partition gaps make the multiplier inexact; published `adj_close` is basis-relative (re-hashes inputs after a
  rebase); tables gated until 0328 (`RevisionsTableNotMigratedError`).
- [ ] **Step 2 (memory/WAL finding, fix round):** the rebase path does an in-place `UPDATE` of historical
  `adjusted_close` (`ticker_history_incremental.py:731-735`) and restatements do in-place DELETE+INSERT (`:704-709`) on a
  `DEFAULT now()` table. Change rebases to ledger-only (`equity_adjustment_rebases`) with the multiplier applied at read
  time (as `bars_asof_sql` already does); restatements append to `equity_daily_bar_revisions` plus a small keyed
  DELETE+INSERT followed by `CHECKPOINT`.

**Acceptance:** approved or adjudicated; no in-place UPDATE on `equity_daily_bars` remains in P14.

---

### Task 15: [0.15] Integration gate

**Lane:** I · **Depends on:** 0.1–0.14 · **Peak target:** 0.3 GiB

**Files:**
- Create: `.superpowers/sdd/tier1-v2/receipts/s0-integration-gate.md`

- [ ] **Step 1:** `git status --short -- atx-db/` shows no modified tracked files owned by atx-db lanes (ignore
  `atx-engine/*`, alpha-engine docs and other sessions' files).
- [ ] **Step 2:** Create a git-archive export of HEAD under `.superpowers/sdd/tier1-parity/exports/<sha8>`; from it run
  `pytest --collect-only -q` over the whole suite (records import errors across all 254 files).
- [ ] **Step 3:** Run, once each and alone, the test files touched in S0 (list them from `git diff --stat <S0 base>..HEAD`).
- [ ] **Step 4:** Version-pin audit: for each producer contract (`research-earnings-events-*`, `research-factor-returns-*`,
  `research-ownership-features-*`, `forward_return_publication_*`, panel `QUERY_VERSION`), grep consumers and confirm one
  version everywhere.
- [ ] **Step 5:** Write the receipt (HEAD sha, export path, collect-only counts, failures with owners). Failures route to
  owners; the gate passes when collect-only has 0 errors and touched tests pass.

**Acceptance:** gate receipt says PASS with the HEAD sha that Sprint 1 uses as base.

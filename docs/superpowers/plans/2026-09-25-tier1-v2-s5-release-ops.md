# atx-db Tier-1 v2 — Sprint 5: Quality Gates, Reproducible Release, Daily Refresh, Documentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan
> task-by-task. Steps use checkbox (`- [ ]`) syntax. **No TDD** (user directive): acceptance is a real-data
> measurement; the whole-branch review (5.6) runs the full non-slow suite once, in bounded batches.

**Goal:** Make quality failures block publication, make every release reproducible from its manifest, keep the SEC side
current with an unattended incremental refresh, document the product the way a tier-1 buyer expects, pass one
whole-branch review, and publish the first honest `candidate` release.

**Architecture:** Publish-bound loads fail before commit on critical/error checks; the release gate reads critical DQCs
that now include survivorship, tie-out, calendar, PIT-clock and uniqueness checks. The manifest pins code, engine,
schema and every consumed input. The nightly supervisor diffs the SEC bulk archives by member CRC, rebuilds only dirty
buckets through the batch stages, and runs each stage as its own guarded subprocess under a single-writer lock.

**Tech Stack:** Python, DuckDB 1.5.5, batch runner (1.3), publication (`publication.py`), Windows Task Scheduler (only
with user gate U10).

**Spec:** `docs/superpowers/plans/2026-09-25-tier1-v2-00-dag-index.md` (§2 exit criteria, §3, §4, §5; rulings R-8,
R-10). Nodes 5.1–5.7.

## Global Constraints

Every task includes the index §3, §4 (M1–M8) and §5. Migration number in this sprint: **0332** (quality policy). The
SEC user agent is exactly `atx-db/0.1 atx-research@example.com`. Paths are relative to `C:\atx\atx-db\` unless they
start with `.superpowers/` or `docs/`. Commit trailer `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.

## Review Focus

1. **A release that says `eligible` on incomplete evidence.** Expected: any unmeasured gate, any critical DQC failure,
   a dirty tree, or reconstructed-only identity keeps the release at `candidate` with the reasons listed.
2. **Loads that "succeed" after a failed check.** Expected: publish-bound loads raise before COMMIT on critical/error
   checks; `Dataset.run` reports failure.
3. **Non-reproducible manifests.** Expected: two publishes of one snapshot are byte-identical apart from `created_at`
   and replay reproduces every Parquet sha256.
4. **Daily refresh that drifts from a full rebuild.** Expected: for the affected CIKs, the incremental result equals a
   full rebuild; a kill mid-cycle recovers on the next cycle.
5. **Personal data leaks.** Expected: no email other than the approved placeholder anywhere in `src/`, `scripts/`,
   generated docs or published manifests (old dataset `params_json` may hold old contact data — never dump it).

---

### Task 1: [5.1] Quality-gate policy and the real DQC/SLO run (migration 0332)

**Lane:** Q · **Tokens:** MIG (0332), HEAVY (the run) · **Depends on:** 3.6 · **Peak target:** ≤ 0.8 GiB per check slice

**Files:**
- Modify: `src/atx_db/dataset.py`, `src/atx_db/warehouse.py` (`quality_check`), `src/atx_db/quality/*`,
  `src/atx_db/provider_coverage.py`, `src/atx_db/publication.py:~1053` (`_critical_dqc_gate`)
- Create: `src/atx_db/migrations/bodies_0332.py`; modify `registry.py` (MIG)

**Interfaces:**
- `Dataset.run(..., raise_on: tuple[str, ...] = ("critical", "error"))` for publish-bound loads: checks run on the
  staged `__next` data before the swap; a failure aborts the swap and marks the run failed.
- Critical checks (0332, via the swap helper): P8 `factor_tie_out` (≤ 1e-6), `delisting_events_without_terminal_return`,
  `cik_less_unknown_performance_policy_rows`, owner-bridge linkage floor (operating-company attrition ≤ 12.5%),
  calendar reconciliation (0 missing sessions), non-NULL `available_at` on every `is_pit_column` table, uniqueness of the
  natural key on each of the 13 tables whose PK 0329 dropped, forward-label expected-minus-published anchors = 0.
- Release row-delta thresholds (per dataset, vs the previous release) as warnings.
- Every public schema has an SLO row so `refresh_provider_coverage` cannot raise.

- [ ] **Step 1:** Implement; register 0332.
- [ ] **Step 2 (drill on a prod copy):** delete one terminal row → load fails and release is `candidate`; perturb one
  tie-out line over tolerance → same; clean copy → all criticals pass.
- [ ] **Step 3 (real, OPS with HEAVY):** run every registered quality check (≥ 379) and every SLO snapshot on prod as
  bounded slices; write `.superpowers/sdd/tier1-v2/receipts/quality-<UTC>.json`.
- [ ] **Step 4:** Commit.

**Acceptance:** drills behave as stated; every registered check has a result; every registered SLO has a snapshot;
failures listed with owners.

---

### Task 2: [5.2] Reproducible release manifest and a golden schema manifest

**Lane:** Q · **Tokens:** MIG (`schema_contract.py`) · **Depends on:** 0.8, 1.2 · **Peak target:** 0.4 GiB

**Files:**
- Modify: `src/atx_db/publication.py:~2131-2150`, `src/atx_db/schema_contract.py:861-901`
- Create: `tests/data/golden_schema_manifest.json` (generated from a clean bootstrap at head 0332)

**Interfaces:** manifest adds `code_git_sha`, `code_dirty` (publishing refuses when true), `package_version`,
`duckdb_version`, `applied_migration_max` + per-migration checksums, `schema_contract_sha256`,
`research_manifest_sha` (4.6), `information_cutoff` (`2026-09-20T22:00:00Z`), and `consumed_receipts` (per stage, the
`source_file_id`s the build actually read) instead of every historical receipt. `verify_schema` compares live against
the golden manifest (drift detection, not certification).

- [ ] **Step 1:** Implement; generate the golden manifest from a clean bootstrap.
- [ ] **Step 2 (real):** two publishes from one warehouse snapshot on a copy → manifests identical apart from
  `created_at`; re-export from the manifest reproduces every Parquet sha256.
- [ ] **Step 3:** Commit.

**Acceptance:** byte-identity and sha reproduction hold; a dirty tree publish is refused.

---

### Task 3: [5.3] Daily refresh — SEC archive CRC diff, dirty-set propagation, supervisor

**Lane:** OPS/M · **Tokens:** ACT, HEAVY (cycles) · **Depends on:** 2.7, 3.6, 1.3 · **Peak target:** ≤ 0.8 GiB per step

**Files:**
- Modify: `src/atx_db/activation.py:824-848` (`_download_archive`: versioned path
  `data/raw/sec/<archive>/<UTC date>.zip` + receipt instead of "skip if exists")
- Create: `src/atx_db/refresh_delta.py`, `scripts/nightly_refresh.py`, `docs/operations/DAILY_REFRESH.md`

**Interfaces:**
- `changed_members(prev_members: Path, new_zip: Path) -> list[str]` — CRC32/size diff against the stored
  `members.parquet` (1.4/1.6).
- Delta apply for changed CIKs, choosing by measurement (record a ruling): (a) a small migration makes the ingest tables'
  load timestamps explicit (no `DEFAULT now()`), so a keyed DELETE+INSERT per changed CIK is WAL-safe, followed by
  `CHECKPOINT`; or (b) dirty-bucket `__next` rebuild + swap. Downstream: rebuild only dirty buckets of the 2.1/2.4 stages,
  labels and derived rows for affected securities; price side (P14 partitions, 3.1 incremental, market_daily for the new
  date) only when U4/RX11 provides a feed.
- `scripts/nightly_refresh.py`: single-writer file lock; each stage a guarded subprocess with its own `--job-gb`;
  status JSON under `.superpowers/sdd/tier1-v2/receipts/refresh/`; automatic `recover_stale_run` on start; incremental lake
  export of changed partitions.
- Maintenance: monthly `COPY FROM DATABASE` compaction to a fresh file then file swap (documented; dry run on a copy).
- Scheduler registration only after U10.

- [ ] **Step 1:** Implement; measure (a) vs (b) on one real delta (a second archive download of companyfacts.zip and
  submissions.zip with the approved user agent through `sec_http`).
- [ ] **Step 2 (real):** run 5 cycles (manually triggered if U10 is not granted); kill one cycle mid-stage.
- [ ] **Step 3:** For 50 affected CIKs, compare incremental results with a full rebuild of those CIKs.
- [ ] **Step 4:** Commit; write `DAILY_REFRESH.md`.

**Acceptance:** each cycle < 60 min of admitted time at ≤ 0.8 GiB per step; the killed cycle recovers automatically; 50/50
CIKs equal the full rebuild.

---

### Task 4: [5.4] Buyer documentation and coverage report

**Lane:** Q · **Tokens:** — · **Depends on:** 2.8, 3.7, 4.7 · **Peak target:** 0.3 GiB

**Files:**
- Create under `docs/methodology/`: `CLOCKS.md` (from 1.11, extended), `IDENTITY.md`, `SURVIVORSHIP_AND_DELISTING.md`,
  `CORPORATE_ACTIONS.md`, `FUNDAMENTALS.md`, `VINTAGES_AND_RESTATEMENTS.md`, `RETURNS_AND_LABELS.md`, `UNIVERSE.md`,
  `RESEARCH_METHODOLOGY.md`, `LIMITATIONS.md`, `DATA_SOURCES_AND_LICENSING.md`
- Create: `docs/COVERAGE_REPORT_<release_id>.md`, `docs/KNOWN_ISSUES.md`, `docs/CHANGELOG_DATA.md`, `docs/VERSIONING.md`

- [ ] **Step 1:** Write each page with the measured numbers from receipts (attrition, terminal policy counts, FSDS
  agreement, item gate, restatement counts, calendar reconciliation, VA1 bias, P8 counts, strict-basis viability).
- [ ] **Step 2:** `LIMITATIONS.md` states: prices from 2012-03-26 (TickerHistory3); XBRL fundamentals complete from about
  mid-2011 (phase-in 2009–2011); IFRS filers excluded from US-GAAP standardization (862 CIKs flagged); modeled vendor
  delivery clock (22:00 UTC vs vendor T+1 delivery); no analyst estimates, borrow, options or forward calendars; identity
  reconstructed, not strict-verified (unless X.6 is done).
- [ ] **Step 3:** `DATA_SOURCES_AND_LICENSING.md` lists every source and its terms as known; for SpiderRock
  TickerHistory3 it states that redistribution rights are unverified and the release is internal-only until the user
  confirms the terms (do not invent license terms).
- [ ] **Step 4:** Coverage report by year × item × JKP size group, universe counts by year, and feature coverage.
- [ ] **Step 5:** Commit.

**Acceptance:** every number cites a receipt, manifest or ledger cell.

---

### Task 5: [5.5] Data dictionary, public API snapshot, AF3 minors, security sweep

**Lane:** Q · **Tokens:** — (AF3 items dispatched to their owners) · **Depends on:** 5.1, 5.2, 4.6 · **Peak:** 0.4 GiB

**Files:**
- Modify: `docs/DATA_DICTIONARY.md` (via `scripts/generate_data_dictionary.py`, adding fallback chains and coverage per
  step), `tests/data/public_api_snapshot.json`, `src/atx_db/api/auth.py:~87` (static API key default `datasets=[]`)

- [ ] **Step 1:** Regenerate the dictionary on a clean export at head 0332 (new tables: `build_runs`, `build_batches`,
  `equity_bar_unit_corrections`, `fundamental_restatement_events`, identity tables, `security_identity_evidence`,
  `equity_adjustment_rebases`, `equity_daily_bar_revisions`, `foreign_filer_reason_intervals`; new columns `shares_clock`,
  `currency_status`, `eligibility_reason`, `rdq_basis`; labels `vendor_shares_zero`, `bar_price_invalid`).
- [ ] **Step 2:** Refresh the public API snapshot once all lanes are quiet (new modules from session 3 and this plan).
- [ ] **Step 3:** AF3 minors bundle — dispatch to owners, one small commit each: C4 N1, N2, M3, M5; P11 R1; P6 R1; A8
  NEW-2, NEW-3; C1 N1, N2, M4, M5, M8, M9; R3b N2–N6 (texts in the session-3 ledger/reviews).
- [ ] **Step 4 (security sweep):** `git grep -nE "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[a-z]{2,}" -- src scripts docs` must show
  only `atx-research@example.com` and documentation examples; confirm `atx-db/build/` is absent (deleted 2026-09-26 under
  U8; it is git-ignored and regenerated by packaging builds — re-check it holds no stale email).
- [ ] **Step 5:** Commit.

**Acceptance:** `tests/test_data_dictionary.py` and the API snapshot test pass on a clean export; sweep clean except U8.

---

### Task 6: [5.6] Whole-branch review and one full non-slow suite run

**Lane:** REV · **Tokens:** — · **Depends on:** 5.3, 5.4, 5.5 · **Peak target:** 0.4 GiB (suite in ≤ 300 MB batches)

- [ ] **Step 1:** Review package from the Sprint-0 base to HEAD plus the deferred AF2 commits (4bcf7f5e … 40248226);
  reviewer on Opus 5.5 with the index as spec: PIT, survivorship, memory doctrine, reproducibility, security.
- [ ] **Step 2:** Run the full non-slow suite once from a pinned export, one test file per process in sequence (each
  ≤ 300 MB; template rebuild alone), recording pass/fail per file.
- [ ] **Step 3:** One fix dispatch per owner for findings; one scoped re-review; adjudicate residuals with rulings.

**Acceptance:** review approved or adjudicated; suite results recorded (all files, not just the 57 that ever ran).

---

### Task 7: [5.7] Candidate release run and release notes

**Lane:** OPS · **Tokens:** HEAVY · **Depends on:** 5.6 · **Peak target:** ≤ 1.0 GiB per slice

**Files:**
- Create: `docs/releases/<release_id>.md`, receipts under `.superpowers/sdd/tier1-v2/receipts/release/`

- [ ] **Step 1:** Un-withhold corporate-action events if P8 is approved (0.4) and its criticals pass.
- [ ] **Step 2:** `publish-release` for: standardized fundamentals + vintage views, restatement events, derived metrics,
  market daily, price metrics, labels v4, identity links and names history, delisting events and terminals, corporate
  actions, universe (candidate cap), research characteristics (4.6). Release diff vs the previous release: bucketed
  FULL OUTER JOIN by key hash above ~30M rows.
- [ ] **Step 3:** Reproducibility check (5.2) on this release.
- [ ] **Step 4:** Release notes: contents, counts, gates and their results, `candidate` reasons, limitations, what changed.

**Acceptance:** release published with status `candidate` and an honest reason list (exit code 3 expected);
reproducibility holds; index §2 exit criteria checked off with evidence links in the notes.

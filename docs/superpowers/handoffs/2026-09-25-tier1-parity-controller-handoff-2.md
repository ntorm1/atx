# Tier1-parity controller handoff #2 — 2026-09-25 ~19:00 UTC

Supersedes `2026-09-25-tier1-parity-controller-handoff.md` (session 1, Codex era). Branch `feat/tier1-parity`,
HEAD `01a5834e`. Session start was `7080a478`; **50 commits** landed this session. Nothing pushed, nothing merged.

## 0. Read these first (in order)

1. Ledger (source of truth, append-only): `.superpowers/sdd/tier1-parity/claude-ctl/progress.md` — the "Session 2"
   section has every dispatch, result, ruling, fix round and parked finding. Trust it and `git log` over memory.
2. Plan: `claude-ctl/plan-s2.md` — §0 verification ledger, §1 coordination rules, §2 rulings RX1–RX5, §3 Phase A
   tasks A1–A10, §4 Phase B (run5), §5 Phase C (superseded ordering), **§8 Research lane R (user priority)**.
3. Contracts every subagent gets: `claude-ctl/global-rules.md`, `claude-ctl/implementer-contract.md`,
   `claude-ctl/reviewer-contract.md`. Per-task `claude-ctl/<TASK>-report.md` / `<TASK>-review.md`; review diffs in
   `claude-ctl/review-pkg/`.

## 1. Standing user directives (binding)

- Restart the swarm to make atx-db a tier-1 data provider. **All agent work on Claude subagents** (Agent tool,
  `general-purpose`, `model: opus` = Opus 5.5). Codex retired. Controller preserves its own context (delegate, don't
  read big files).
- **No TDD.** Build real features first; tests only where they prove economic / PIT / integrity / operational
  behavior. Prioritize real progress.
- **User priority (mid-session):** "high quality, massive cross-sectional, statistically significant standardized
  metrics that have predictive power over forward returns over medium-long horizons. Then build forward from that."
  → Research lane R is the priority; release/SLO/refresh work sits behind it.
- **Edit only with Write/Edit tools** — no Python/pathlib/sed/Set-Content/heredoc edits of code or docs (shell only
  for tests, probes, git). In contract rule 9 and global-rules.
- Process rules in force: one scoped review per task, **max 2 fix rounds** (session ruling), then breaker/adjudicate
  and park with ruling. Parallel implementers allowed only on file-disjoint ownership; explicit-pathspec commits; never
  stash/reset/clean/checkout/push; leave `stash@{0..2}` (pre-existing, Sep 19/20 + Aug 4), `docs/plans/2026-09-24-alpha-engine-*`,
  `atx-engine/*`, `C:\atx-wt\*` alone (another session's work — `atx-engine/src/book/replay.cpp` is modified by it).

## 2. State at stop

- **All subagents stopped** by the controller at the user's request (~18:55Z). Orphaned pytest/probe processes killed.
- **archive21 auto-launch watcher stopped.** No warehouse writer running; `activation-companyfacts-archive21-launch.json`
  does NOT exist → archive21 was never launched. Warehouse untouched this session except OPS-A's closure of archive19/20
  ledgers (see §4).
- **Uncommitted WIP in the working tree** (from lanes stopped mid-edit — verify before keeping; do not blindly commit):

| Lane (agent) | Files (uncommitted) | What it was doing | Next action |
|---|---|---|---|
| Catalog fix round R1a+R1b (impl-r1b) | `research/catalog.py`, `seeds/research_anomaly_catalog.csv`, `seeds/derived_metric_definitions.csv`, `seeds/standardization_rules.csv`, `docs/research/ANOMALY_CATALOG.md`, `tests/test_research_catalog.py`, `tests/test_research_metric_economics.py` | Combined fix round per rv reviews: add `hypothesis_family`/`domain`/`caveat_code` columns; resolve contradicting hypotheses (two_sided or drop); SG&A composite (J3, likely the standardization_rules.csv change); consistent PP&E basis (J2); missing≠zero debt/dividends (J4); block eps_diluted_growth_yoy until split guard; debt/assets_to_market → mixed; EOL-normalized catalog hash. Was mid "doc hunks" | Fresh implementer: read `R1a-review.md` + `R1b-review.md` + ledger "combined catalog fix round" ruling, `git diff` those files, finish, run catalog + economics tests (economics test ~345 MB, run alone), commit, then scoped re-review |
| R1d fix round 1 (impl-a4) | `_derived_pit.py`, `_split_epochs.py`, `derived_metrics.py`, `tests/test_split_epochs.py`, `tests/test_derived_quarter_chains.py`, `tests/test_core_metric_breadth.py` (shared) | Split guard fixes: C1 share-count corroboration (special dividends/spin-offs ≠ splits), I1 source/load boundaries = coverage breaks, I2 flat factor + split signature → unknown, m5, m3, m2 set-based scan (was restructuring `_stage` into chunked inserts) | Fresh implementer with `R1d-review.md` + `R1d-report.md`; finish; test; commit; scoped re-review (rv probes in scratchpad `rv-a4-probe/test_rv_r1d_probe.py`) |
| R3b fix round 1 (impl-r3b) | `research/evaluation.py`, `scripts/research_evaluate.py`, `tests/test_research_evaluation.py` | I1 full expected family + not_produced/partial_family_subset blockers; I2 valid-cohort + primary-line ranking; I3 `research_eval_series` table; NYSE size buckets from PIT venue; use only size_status-verified size (R2b concern #1); accept expected_sign 0 | Fresh implementer with `R3b-review.md` + controller additions in ledger; finish; commit; re-review |
| AF2 test-suite health (impl-af2) | `ticker_history_quality.py`, `tests/test_activation_stages_b.py`, `tests/test_alias_mining.py`, `tests/test_backfill.py` | Many AF2 items already committed (see §3); was editing the backfill test to pass the retained dataset explicitly | Read `AF2-brief.md` (+ items 15/16 and M3/M11/N3 added via messages — see ledger), check which items remain, finish, one chunked full non-slow run, report, review |
| RI1 delisted-line owner reconstruction (impl-ri1) | `src/atx_db/identity_reconstruction.py` (untracked), `tests/test_identity_reconstruction.py` (untracked) | New module: confidence-scored historical line→CIK links (TickerHistory + submissions formerNames + companyfacts dei:TradingSymbol + name matching), output shaped to 0327 `security_identity_evidence`; was "reviewing module once for correctness". No report written yet | Fresh implementer: read module + brief in ledger dispatch; measure coverage/precision on retained files; commit; review; then bridge hook (after A8 review) |
| R2d (impl-r2a) | none yet | Panel reads A8's filing-matched share relation for `line_market_cap` (hook spec in `R2a-report.md` "Fix round 2") | Dispatch fresh after A8 review |

- `tests/data/public_api_snapshot.json`: needs `research`, `market_owner_bridge`, `identity_reconstruction` (RI1)
  entries — `test_module_boundaries::test_public_api_snapshot_matches_pinned_fixture` fails until then (`_split_epochs`
  is private by design).

## 3. Task status (commits; review verdicts in claude-ctl/*-review.md)

**Complete (review clean or parked with ruling):**
- L1 desk SQL pack `c69ef10e`.
- A1 activation ladder + pinned-snapshot policy `12bfc755, 9230c4e5` (universal post-cutoff receipt guard).
- A2 PIT universe builder `01f7f67c, 36feab47` (strict `us_listed_v1` + labeled `us_listed_reconstructed_v1`).
- A3 delisting events/terminals `3003b8d9, 8ae547fa, 44a095f6` (1 parked: N4 → fixed in AF1).
- A4 quarterly margin/EPS-growth/accel metrics `ddf0cdad, 602fc6c6, a297f93d`.
- A5 owner bridge `54a80811, 94a26940, d9c5acab` (multi-line = >1 common class).
- A6 period-own fiscal labels `4ebfd784, 128b9699`.
- A7 basic/diluted GAAP EPS release evidence `551f8933, 1266f6a1, ea493262`.
- AF1 cross-task follow-ups `f78208ff, 18176e9f, 07c1fa01, 4167fbd9, 1328779e, a901d4f0, 133fc21d`.
- R1c quarter-chain proofs `51eae63f, 71fcca6a`.
- R2a monthly PIT research panel `9b143636, ea5a4b33, 4526fb18` (absorbed R2c set-based slim lineage proofs).
- R3a stats core + monthly labels `d6d7302c, 1728c0a6` (1 parked: 0186 monitoring view → fixed in A9 item 4).

**Committed, review NOT finished (reviewers stopped):**
- A8 market-cap basis + vendor share units + share availability clock `b10175b6, 01a5834e` — review package
  `review-pkg/A8-2commits.diff`; acceptance (i)–(vi) in ledger; rv-a5 was mid-review.
- A9 migration 0327 (registered, head 327) `78697856, ee498df5` — `review-pkg/A9-2commits.diff`; rv-a9 was mid-review.
  **Must be reviewed before B0 applies it** (runs once on ~13 GB warehouse).
- R2b standardized feature store `83b50e06` — `review-pkg/R2b-83b50e06.diff`; rv-r2a was mid-review.
- R1a anomaly catalog `89d029e9` + R1b 43 anomaly defs `ef9b7d87` — reviewed (Changes needed); fixes are the
  uncommitted catalog fix round above.
- R1d split guard `34743f05` — reviewed (1 Critical C1, 2 Important); fixes are uncommitted WIP above.
- R3b evaluation harness `fc5c5987` — reviewed (3 Important); fixes are uncommitted WIP above.
- AF2 partial: `4bcf7f5e, 84b11085, b82507e0, d3fda8a4, ef676dc0, 4fe3aa76, 5921444e, 74790837, bb458e20`
  (stale fixtures, ticker-history publisher temporaries, A7 V1 discontinued-ops heading, listing_metrics receipt clock,
  pandas-path labels/anchors, desk-q5 accepts v2, conftest template fingerprint covers imported registry, 0314 bootstrap
  to slow lane). Unreviewed.
- OPS-A ops commits: `da06924b, d8e9c936, 4909b6e2, 80878e3e`.

**Not started:** A10 run5 preflight (after A8/A9 reviews; regenerates DATA_DICTIONARY incl. rv-a6's replacement label
text in `A6-review.md` "Re-review 1"; removes personal-email UAs in 4 modules; runs the >300 MB scale tests deferred all
session), R1e (split lookup via bridge API `single_class_lines` — after R1d fix + A8 review), R2d, R4 qualification
ledger (RX7 frozen policy/holdout), R5 end-to-end rehearsal + heavy research runs RR1–RR5, FQ2/CF1 versioned size fix
(fq2_v3), Phase C items behind R. Phase B (run5) blocked on heavy chain (§4).

## 4. Heavy chain (warehouse) — blocked on host RAM

- Frontier: CompanyFacts archive chain. archive19 (e27c8a4e) and archive20 (4c69bc32) closed failed with receipts;
  totals unchanged 47,941,000 facts = points, 9,462 lineage CIKs; **9,558 members left** starting position 10,832.
  `rows=0` counter is correct (counts replay rewrites only).
- **archive21 is ready, not launched:** `claude-ctl/launch-archive21.ps1` runs the pinned export
  `.superpowers/sdd/tier1-parity/exports/c69ef10e/atx-db` (src == 7080a478, the chain's code), resume from 4c69bc32,
  `--cache-dir` stays relative by design (resume compares literal cache paths). Launch detached:
  `Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','C:\atx\.superpowers\sdd\tier1-parity\claude-ctl\launch-archive21.ps1'`.
- **Blocker:** the guard (`run_memory_guarded.py`) refuses launch unless min(phys, commit free) ≥ job_gb 1.5 + 2 =
  3.5 GiB and stops the job at phys < 1.5 / commit < 3 GiB. Host baseline leaves ~2.0–2.7 GiB physical free (VS Code
  ~3.7–4.3 GB, claude processes ~2–2.6 GB, Chrome ~1.4 GB, Windows Update). archive20 was guard-stopped at 11:23Z when
  agent pytest load dipped commit to 2.9 GiB. Controller ruled **never lower guard floors**. Options: user frees RAM
  (close Chrome / extra VS Code windows / pause agents), or run heavy work with no agent swarm active.
- Auto-launcher (stopped): `claude-ctl/autolaunch-archive21.ps1 -MaxHours 12` (timestamped receipts; observer floor
  3.6 phys / 6 commit sustained 120 s; checks no writer; launches detached). Log `claude-ctl/autolaunch-archive21.log`.
- After archive21 terminal: inspect with `claude-ctl/inspect_companyfacts_terminal.py`; then **submissions resume**
  (reconcile §1 command, from 04cf947d…, batch 50, no sec_bulk_download) and **CVX earnings_release_facts** — pin CVX to
  an export that contains A7 (`ea493262`) but NOT the 0327 registration: e.g. `78697856` (0327 body unregistered).
- **B0** (apply 0327 with governed backup): export must be ≥ `b10175b6`/`01a5834e` (A8 loader writes `shares_unit` so
  0327's median-rule correction skips new runs) and include all reviewed Phase A. 0327 cost: est_actual/market_daily
  rebuilds trivial (0 rows), shares UPDATE ~20 s / +250 MB over 32M bars.
- **Disk (RX4 granted by user after stop: "clear whatever you need to make space, keep prod db"):** pruned with the
  project's registry-aware `migration_admin.enforce_backup_retention(keep_latest=1)` → 9 registered backups deleted, plus
  2 unregistered older backups (20260924-224104, 20260920-170328) removed. Kept: `data/warehouse.duckdb` (11.99 GB, prod)
  and newest registered rollback `warehouse.duckdb.pre-migrate.20260924-225023.bak` (12 GB). C: free 36.3 → **122.8 GB**.
  Kept deliberately: raw vendor sources `data/staging/broad-bars/` (TickerHistory3.parquet 3.4 GB, 10y TSV 10.3 GB),
  `data/cache/` (pinned snapshot sources). Regenerable if more space is ever needed: `atx-db/.pytest_cache`
  (6.5 GB stale schema templates), session scratchpad (~4.5 GB).
- **B3 mandatory pre-step:** `--only symbol_directory --force` then STOP post-checks (0 latest rows as_of_date
  2026-09-20; latest as_of_date 2026-09-18; max(available_at) within cutoff day). Any ladder running `security_master`
  needs `--as-of-date` ≥ 2026-09-20 (retained company_tickers.json mtime).
- Heavy research runs (R5 RR1–RR5) only after run5 B3, with no warehouse writer (research.duckdb attaches read-only);
  RR3 pre-step: real 10K-state lineage sample (commands in `R2a-report.md`). Projections: RR3 14–29 GB / 14–28 h;
  R2b 1–2.2 h, 6.5–14 GB per basis; RR4 2.5–5 h.

## 5. Key discoveries / traps (append to durable docs as appropriate)

- **TickerHistory3 vendor `shares` are in THOUSANDS** (AAPL 15,204,137 vs 15.2B real) and runs start at the DEI cover
  as-of date (~2 weeks before the filing is public). Fixed going forward by A8 loader; existing rows corrected by 0327
  item 5; share availability clock by A8 (DEI-matched → filing acceptance, else labeled 90-day modeled lag; 20-F median
  lag 99 d → residual look-ahead flagged for review).
- **DuckDB 1.5.5 WAL replay fails** on ALTER TABLE of a table with a `DEFAULT now()` column ("GetDefaultDatabase with no
  default database set"). 0327 uses create/copy/swap and has a WAL-replay test; older ALTER migrations are exposed only
  between COMMIT and CHECKPOINT (pre-migrate backup recovers).
- **conftest template-cache poisoning** (fingerprint from disk vs registry imported at collection) — fixed `74790837`.
  Template rebuild after any migration/seed change peaks ~364–383 MB (accepted exception to the 300 MB rule).
- **Inference:** Newey-West (lag h−1) over-rejects badly under overlapping monthly h-month returns at T≈160 (16% at 5%;
  |t|≥3 ~4% vs 0.27%). Significance must use R3a's **EWC fixed-b Student-t** (`robust_p_value`, `z_equivalent`,
  `hlz_pass` takes z/p only) + studentized bootstrap; DSR/PSR need `horizon_periods=h` and `n_trials` = whole family;
  equal-weight decile spreads over-reject (5–9%) → supporting evidence only. FQ2/CF1 legacy 21-session tests ~11% size.
- **Power:** prices start 2012-03 → ~160 monthly observations, ~13 independent years at 12 m.
- **Survivorship:** owner bridge links only via current SEC ticker map → most delisted lines unlinked → no fundamentals
  (R2a counts `owner_link_attrition`); nearly all historical delistings CIK-less `unknown` + Shumway. RI1 + C9/C10
  historical identity evidence is the real fix.
- **Split handling:** `_split_epochs.py` infers splits from adjusted/raw close ratio jumps; review showed special
  dividends/spin-offs ≥20% misread as splits without share corroboration (C1, fix in WIP). Bars are keyed by price line,
  fundamentals by owner id → R1e must route split lookup through the bridge or cross-filing per-share comparisons stay
  unproven for most issuers.
- **Host/process:** subagents share one Windows host; keep each pytest ≤ ~300 MB and one per agent; large synthetic
  probes starve the heavy slot. Use task-prefixed scratch paths (a scratchpad `head/` collision happened). A transient
  "Not logged in" API auth error killed one agent mid-round (resume worked).
- Edit tool on mixed-EOL files normalizes touched lines (activation.py, reported_eps_core.py, registry.py,
  `__init__.py` changed EOL); accepted, no functional effect.

## 6. Rulings made this session (verbatim list in the ledger — `grep -n '^Ruling:' claude-ctl/progress.md`)

Governance: Claude opus subagents; no TDD; 1 review + ≤2 fix rounds; detached heavy launches; parallel file-disjoint
implementers; heavy slot priority (≤300 MB/test, one pytest per agent); never lower guard floors; RX1 (labeled
reconstructed basis for research, strict for certification) approved and extended to the R lane; RX2 (Shumway only for
unexplained cessations) approved; RX3 (pinned-export heavy launches) approved; **RX4 (prune backups) NOT granted**; RX5
(≤500 SEC doc fetches) approved; RX6 (separate research.duckdb, warehouse read-only) approved; RX7 (freeze
qualification policy + holdout before real data) approved; re-prioritized to research lane R per user; accept CRLF→LF
normalization. (RX4 was later granted by the user and executed — see §4 Disk.)
Data/economics: pinned directory rows' available_at = original receipt time; universal post-cutoff guard; B3
symbol_directory pre-step mandatory; merger window [−365 d, +30 d] with target-side forms only; delist_date = first
session after last trade, observed DLSTDT normalized at load; halt-gap terminals stitched at first session after last
trade (shared effective-terminal fragment, calculation_version v2); multi-line = >1 common-equity class; NULL multiples
for unresolved multi-class; vendor shares ×1000 + filing-matched share clock; est_actual PK includes period_start;
per-share QoQ incomparable without split basis; formula-derived split sensitivity; flows-exactly-4q rule until split
guard, then NO fallback when bars are missing; one-sided opening-balance tolerance; split requires share corroboration;
piotroski_f_cash_issuance variant; SUE share-basis-free (NI seasonal diff / prior-8 sd); catalog adds
hypothesis_family/domain/caveat_code, class-count targets soft; significance on EWC; R4 gates on IC family and requires
same-sign in small and large NYSE size buckets (PIT venue); fundamentals on one primary line per issuer; identity-free
market features for all eligible members; unverified vendor-share size excluded from size logic until R2d; SLOs no
weaker than existing analogues.
Parked: A3 M7 legacy `refresh_delisting_events` (jobs.py, C12 must retire it); A3 N4 (fixed in AF1); R3a N1 (fixed in
A9). Deferred minors per task are in the ledger (`grep 'minor (deferred)'`).

## 7. Recommended next steps (in order)

1. Decide on host RAM for the heavy chain (user): free ~1.5 GB physical, or schedule heavy runs with the swarm paused.
2. Finish the 5 WIP lanes (§2 table) with fresh implementers (Opus), each followed by its scoped re-review; finish the
   stopped reviews of A8, A9, R2b (re-dispatch reviewers with the same package paths).
3. Refresh `public_api_snapshot.json`; commit AF2 remainder; one chunked full non-slow gate run.
4. Heavy: archive21 → inspect → submissions resume → CVX (export `78697856`) → A10 preflight → B0 (0327, reviewed) →
   B1–B3 run5 → B4 acceptance.
5. Research: R1e, R2d, RI1 hook into bridge, R4 (frozen policy/holdout, hashed), R5 fixture rehearsal, then RR1–RR5 on
   run5 outputs → first qualified-signal catalog (`docs/research/QUALIFIED_SIGNALS.md`).
6. Then Phase C behind R (desk runner C1/C5, release eligibility C4, coverage/SLO/DQC, incremental refresh C12, FQ2 v3).

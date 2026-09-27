# Task T6 — PIT research-field producer aligned to existing role axes (Python)

Owner worktree: `C:/atx-wt/pool-8`, branch `feat/mega-alpha-fields-20260926` (base `bb5bc25b`).
New files only: `atx-engine/tools/prepare_research_fields.py` and a small synthetic test
`atx-engine/tools/test_prepare_research_fields.py` (or the repo's existing Python test location for
`prepare_recent_research.py` — find where its tests live and mirror). Do not edit existing files.

## Why
The 48-alpha price/volume blend is too weak for the net Sharpe >= 1 objective. A read-only inventory
found non-price point-in-time (PIT) sources keyed by our instrument namespace (`spiderrock.securityID`).
They must become extra DSL panel fields aligned EXACTLY to the existing role payload axes, so that the
C++ runner (a later task) can load them next to close/raw_close/volume without re-projecting prices.

## Existing role payloads (read-only inputs)
Role dirs in `C:/atx-wt/pool-2/build-equity/`: `recent-fast-train-2020-2022-v1/` and
`recent-fast-validation-2023-2024-v1/`, each with `manifest.json`, `sessions.i64` (session
timestamps, see manifest), `ids.u64` (securityIDs), date-major `close.f64`, `raw_close.f64`,
`volume.f64`, `present.u8`, `member.u8`. Producer of these: `atx-engine/tools/prepare_recent_research.py`
(read it: clocks = modeled session + 22h mark, + 23h decision; manifest conventions; hashing).

## Sources (from the inventory; verify each claim before relying on it)
1. FINRA short interest, as-of tables: `C:/atx/data/finra_short_interest/asof/si_shares.csv` and
   `si_dtc.csv` (read-only; separately owned checkout — never write there). Keyed by securityID;
   `available_at` = official dissemination date (`dissemination_schedule.csv`). Existing C++ join
   semantics: strict `available_at < session` (`atx-impl/src/asof_field.hpp:17-23`, loader
   `build_asof_column` :64-67) — replicate exactly. Staleness cap 45 calendar days -> NaN.
   Caveat to record: files before 2021-06 are later republications (vintage risk).
2. TickerHistory3 extra columns in `C:/Users/natha/Downloads/TickerHistory3.parquet` (3.45 GB,
   262 row groups, rows mix all dates; stream only needed columns with pyarrow row-group iteration and
   filter to role ids and role session dates; keep RAM < ~700 MB). Columns of interest: ATM implied
   vol term structure `atmCenI_*`, historical vol `atmCenH_*`, `earnFlag` (use only 0/1, i.e. at/after
   the event; -1 presumes a known future date — exclude), `nEarnCnt_*` only if it counts PAST events
   (verify semantics from docs `docs/superpowers/specs/2026-06-16-orats-history-loader-design.md`
   and exclude any forward-looking `*EMove`/`*D1` columns), and `shares`. Read the schema from the footer
   first and pick exact names. Vendor end-of-day row for date d is treated as known at the d 22:00 UTC
   mark (same as close). Record: IV often null; no vintage proof.
3. Monthly size/security type: `C:/atx/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23/
   spine_monthly/year=*/part-0.parquet` (`me_line`, `size_grp`, formation date) and `line_types`
   (`security_type`); `line_id = TBLTICKERHISTORY-<securityID>`. As-of by formation session (strict <
   decision session). Pin by hashing the files you read (a job may be writing the lake; if files change
   while reading, fail).
Everything available on or after 2025-01-01 must be excluded (the roles end 2024, but assert it).

## Output
`build-equity/<role>-fields-v1/` (exclusive new dir; refuse if exists): one date-major little-endian
f64 file per field, shape = role dates x role ids, NaN where not visible/absent; plus `manifest.json`
(schema `atx.research-role-fields/v1`): role manifest sha256, role sessions/ids sha256 (must equal the
role's), field list with name, source path(s) + sha256, clock rule, staleness rule, units,
coverage stats (per-field fraction of member cells finite, per-year), and per-file sha256/bytes.
Proposed field names (DSL identifiers, no dots): `si_shares`, `si_dtc`, `iv_atm_<tenor>` for 1-3 tenors
available, `hv_<tenor>`, `earn_recent` (0/1), `shares_out`, `mktcap_lagged` (from spine me_line),
`is_common` (1/0 from security_type). Keep to fields that exist; justify drops.
CLI: `--role DIR --role-sha256 SHA --output DIR [--fields a,b,...] [--max-rss-mib N]`.

## Constraints
- Python 3.12 with pyarrow/numpy/duckdb as available (`C:/Program Files/Python312/python.exe` has them;
  check). Deterministic output bytes. Stream; never materialize the full 32M-row table.
- Do NOT run it on the real sources yourself beyond schema/footer reads and tiny row-group samples
  (<= a few MB): root alone runs real data. Write a synthetic-fixture test (tiny role + tiny CSV/parquet)
  covering: strict available_at < session, 45-day staleness, 2025 exclusion assert, axis alignment,
  exclusive output, manifest hashes. Running that synthetic test yourself is fine.
- Not TDD: implement first, then the fixture. No subagents. Commit in pool-8 (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
Full report to `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T6-report.md` (exact
source columns chosen and semantics evidence, clocks, expected runtime/RAM, the exact command lines root
should run for TRAIN and validation roles, commit SHA). Return only: status, commit SHA, one-line
summary, concerns.

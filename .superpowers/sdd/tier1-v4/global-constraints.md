## Global Constraints

- **GC1 PIT.** Every published row carries `available_at` (UTC). Daily: visible at session d only if
  `available_at < 22:00 UTC of session d-1`. Monthly (stamped at month-end session m): uses only rows with
  `available_at < 22:00 UTC of m` and prices ≤ close of m. No backdating; a missing clock gets a documented
  conservative floor. Vintages are new rows, never overwritten. Delisted lines included.
- **GC2 Holdout (D6, owner clarification R21).** No return evaluation (IC, factor correlation, portfolio/backtest
  assessment, replication) or complete forward-label window on any date ≥ 2023-01-01. Historical return-based
  predictors and independently source-qualified individual delisting-return values may be constructed later,
  with all original PIT/source requirements. Value/coverage comparisons are allowed in any year. See
  `task-21-cutoff-scope-ruling.md`; this does not authorize later evaluation or forward labels.
- **GC3 Universe and window.** Score window 2020-01-01 → latest session. Report coverage per year 2020-2025 on panel
  `member_equity` and, once Task 2 lands, on `univ_all_common` and `univ_r3000p`. Lookback history only where a
  measure needs it (fundamentals from 2015; prices from 2018; `prices_history` for factor replication windows).
- **GC4 Host.** Every data job runs under `../.superpowers/sdd/tier1-parity/run_memory_guarded.py`, cap ≤ 1.0 GiB
  (prefer 0.6), DuckDB via `atx_db.alpha_panel.common.connect` (file-backed spill, threads ≤ 2), one heavy job per
  lane at a time. Pytest: offline fixtures, < 60 s per file. C: free ≥ 40 GB before any landing > 1 GB.
- **GC5 Files.** Never edit or stage any path in `.superpowers/sdd/tier1-v4/foreign-dirty-files.txt` (another
  session's uncommitted work). New behaviour goes into new modules under `atx-db/src/atx_db/alpha_panel/`. Stage
  conventions per `.superpowers/sdd/tier1-v3/LANE-RULES.md` (`stage_dir`, `connect`, atomic `copy_to_parquet`,
  `write_stage_manifest` with `input_manifests_sha256`). Do NOT edit shared registries (`atx_db/stagelake/*.py`,
  `DATA_DICTIONARY.md`, `PARITY_SCORECARD.md`): lanes run concurrently in one tree and a shared file would be swept
  into another lane's commit. Instead write a stage-registration stub `atx-db/docs/datasets/<stage>.registry.json`
  (stage name, path, keys, schema id, as-of macro SQL); Task 17 folds every stub into `stagelake`.
- **GC6 Commits.** `git add <paths> && git commit -m "<msg>" -- <paths>` (explicit pathspec), message ends with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never `git add -A`, never commit data. Report file
  committed with `git add -f`.
- **GC7 Sources.** SEC via `atx_db.sec_http` (≤ 5 req/s, existing user agent); other hosts ≤ 1 req/s. Raw landings in
  `data/raw/<source>/` with `receipts.jsonl`. Free sources only (D3).
- **GC8 Naming and docs.** Compustat mnemonics for standardized items, WRDS WFRS names for ratios, JKP names for
  characteristics. Each new stage gets `atx-db/docs/datasets/<stage>.md`: columns (name, type, unit, definition,
  source, clock, staleness), keys, PIT rule, measured coverage table, known limits.
- **GC9 Evidence.** Every gate is reported PASS/FAIL with the measured number and the exact command. A FAIL that
  cannot be closed inside the task is reported, not hidden; the stage still publishes with `gate_status` in its
  manifest.


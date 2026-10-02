# tier1-v3 lane rules (read before any work)

Plan: `C:/atx/docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md` (read §3 invariants and your sprint).
Rulings and facts: `C:/atx/.superpowers/sdd/tier1-v3/CARRY.md` (read all of it).
Owner focus: US equity long/short alpha; the score window is the last 6 years (2020-09 → today). Build what a
characteristic in that window needs; history before 2019 only when a lookback needs it (fundamentals 5-year lags).

## Environment

- Repo `C:/atx`, branch `feat/tier1-v3-warehouse`. Never switch branches, create worktrees, stash, reset or rebase.
- Work from `C:/atx/atx-db`. Python `.venv/Scripts/python.exe`; set `PYTHONPATH=C:/atx/atx-db/src` and
  `OPENBLAS_NUM_THREADS=1`. Bash tool available (Git Bash).
- Stage lake root `data/alpha_panel/v1` (git-ignored). Stage code lives in `src/atx_db/alpha_panel/` unless your
  brief gives another package. Follow the `alpha_panel.common` conventions: `stage_dir`, `connect` (DuckDB, memory
  ≤ 60% of your guard cap, threads ≤ 2), `copy_to_parquet` (atomic `.partial` → rename; row groups ≤ 32768 for
  wide tables), `write_stage_manifest` (schema id, code SHA, per-file SHA-256; add `input_manifests_sha256` for
  every input stage manifest you read). Look at an existing stage such as `ftd.py` or `insider.py` first.

## Memory guard (mandatory for every data job)

```bash
cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 -- \
    .venv/Scripts/python.exe -m atx_db.alpha_panel.<module> <args>
```

- Cap ≤ 1.0 GiB per job; prefer 0.3-0.6. Run at most one of your own heavy jobs at a time. The guard queues when the
  2 GiB machine budget is full: that is normal, wait for it. Returncode 3221225477 = native crash; investigate.
- Pytest runs need no guard but must be fast (< 60 s per file) and offline (fixtures only, no network).

## Invariants (plan §3)

- Every stage row carries `available_at` (UTC). Visible at session d only if `available_at < 22:00 UTC of d-1`.
  No backdating; a missing clock gets a documented conservative floor.
- Vintages are new rows, never overwritten. Re-posted sources carry `vintage_risk`.
- Delisted lines included. Issuer joins carry `link_tier`.
- Keys: TickerHistory3 `security_id` (with the sid0-bracket repair already in the prices stage) and CIK.
- D6 holdout: never compute a return-based statistic on 2023+ data (factor/return validations end 2022-12-31).

## Sources

- SEC: use `atx_db.sec_http` (limiter ≤ 5 req/s, approved user agent). Other hosts: polite, sequential, ≤ 1 req/s
  unless the host documents more. Reuse existing user-agent constants; do not invent contact strings.
- Raw landings go to `data/raw/<source>/` as served, with a `receipts.jsonl` (url, bytes, sha256, http_status,
  fetched_at). Downloads must be resumable.
- Disk: keep C: free ≥ 40 GB (check `df -h /c` before any landing > 1 GB). Parse zips then delete them, keep the
  receipt. Record for each new source: url, bytes landed, history span, cadence, terms note, rate limit — in your
  report under "Sources" (the controller compiles `docs/SOURCES.md`).

## Code and commits

- Create and edit files with Write/Edit. Match the surrounding style (dense docstrings, type hints, no chatter).
- Edit only the paths your brief owns. If you need a change elsewhere, say so in your report instead.
- Do not touch `panel.py`, `borrow_proxy.py`, `metrics.py`, `export_impl.py` or the stages `panel/`, `borrow_proxy/`,
  `metrics/`, `export/` unless your brief says so (the controller runs the S0.1 chain on them).
- Tests: fixture-based pytest in `atx-db/tests/test_<area>_*.py`; every rule gets a test.
- Commit with explicit pathspecs only: `git add <file> ...` then `git commit -m "<msg>"`, message ending with the line
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never `git add -A` / `git add .`; never commit data.
  Other lanes commit concurrently: if `index.lock` exists, wait a few seconds and retry; never delete it unless it is
  older than 2 minutes.

## Report

Write `C:/atx/.superpowers/sdd/tier1-v3/task-<ID>-report.md` (and `git add -f` it in your last commit):
1. what was built (modules, stages, rows, bytes);
2. each done criterion from the brief: PASS/FAIL with the measured number and the command that measured it;
3. sources landed (see above); disk used;
4. deviations and open issues.
Your final message to the controller: the done-criteria table, commit SHAs, open issues. Terse.

## Commit rule update (2026-09-30 00:40Z)

The git index is shared by all lanes: a bare `git commit` sweeps other lanes' staged files into your commit
(it happened: 1819c5b2). Always commit with an explicit path list after `--`, which commits only those paths:
`git add <paths> && git commit -m "<msg>" -- <paths>`.

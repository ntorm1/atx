# Task W0-2 (tool part) report: compare_window_overlap.py

Lane R1, worktree pool-10, branch `feat/platform-v8-r1-20260929`. Scope: only the overlap tool and its test. Root runs
brief steps 1-6 (bridge, roles, plan-only, fields, u pass, ledger).

## What was built

- `atx-impl/tools/compare_window_overlap.py` (new). CLI:
  `--kind {field,signal,daily_ic} --old PATH --new PATH --out report.json [--before YYYY-MM-DD] [--per-key]
  [--old-role DIR --new-role DIR] [--old-run DIR --new-run DIR] [--no-verify]`.
  - Alignment is by (session, instrument id), never by position: `np.intersect1d` of the two roles' `sessions.i64`
    and `ids.u64` (both verified against their role manifest). Old-only / new-only sessions and instruments are
    counted in `alignment`.
  - Cell rule: equal = identical IEEE bits or NaN on both sides (any NaN payload). Bit patterns first, then the
    absolute difference for unequal cells with no NaN side (`max_abs_diff`, `"inf"` if an infinity is involved).
    A finite old value with no aligned new cell is `old_cells_missing_in_new`. `bit_identical` = no unequal cell and
    no missing old value. New-only instruments/sessions (the grown union) are only counted (`new_only_finite_cells`).
  - `--per-key`: one row per field / candidate / id: `cells_compared, unequal_cells, nan_mismatch_cells,
    max_abs_diff, first_diff {session, session_ns, instrument_id}` (daily_ic: `{session, horizon, column}`),
    `old_cells_missing_in_new, new_only_finite_cells, bit_identical`. Without it: totals + `differing_keys` only.
  - `--before D`: sessions strictly before D. Refused if D is after the seal.
  - Seal: `seal()` imports `atx-engine/tools/research_window.py` (`SEAL_NS`, task W0-1) through a guarded import;
    until that module is merged the documented fallback is the literal 2024-01-01 (`SEAL_FALLBACK_NS`), and the
    report's `seal.source` says which one was used. Any role session or daily-IC row on or after the seal is refused
    (exit 2) before a payload is opened.
  - Memory: one key at a time; payloads are read-only `np.memmap`s read in blocks of 64 sessions (64 x names x 8 B per
    side), unmapped before the next key. Payload SHA-256s are verified against the fields manifest / sidecar pins
    (a second streamed read; `--no-verify` skips it).
  - Output: `atx.window-overlap/v1` JSON written via `.partial` + rename, never overwritten; stdout one summary line
    (`bit_identical, max_abs_diff, unequal_cells, cells_compared, old_cells_missing_in_new, differing_keys`).
    Exit 0 compared, 2 refused.
- Kinds:
  - `field`: two `atx.research-role-fields/v1` dirs; role from the manifest's `role.path` (override `--old-role` /
    `--new-role`), which must carry the manifest SHA-256 and sessions/ids SHA-256s the fields manifest recorded. One
    key per field name of the old manifest; a field absent from the new one is a `missing_in_new` row;
    `new_only_keys` lists the rest.
  - `signal`: **mapping chosen** (also written into every report as `mapping`): each cache root is walked (depth <= 4,
    `ic<v>_<16hex>` IC-result directories skipped) for sidecars of schema `atx.dsl-candidate-signal/v1|v2` whose
    `role_manifest_sha256` is that side's role (`--old-role` / `--new-role`, required, because a sidecar names its
    role only by SHA). The ids compared are the old run's or every old-cache id on the old role. With `--old-run` /
    `--new-run` (IC run directories) a side's entry is the one whose `payload_sha256` that run's
    `summary.json` `roles[train].candidate_cache.entries[]` lists for the id; without them a sole entry is taken and
    several entries of one id pair only through exactly one equal `dsl_sha256`, else the id is reported `ambiguous`
    and not compared. Deviation from the dispatch note: `train_candidates.jsonl` does not name the cache entry (it
    holds the IC summaries only, `strategy_ic_runner.cpp:342-464`); `summary.json` `candidate_cache.entries` does, so
    that is what `--*-run` reads. A summary with any non-`train` role is refused.
  - `daily_ic`: two `train_daily_ic.csv` files or run directories; header checked; keyed (id, horizon, session_ns);
    columns pearson, rank_ic, oriented_rank_ic (empty = NaN); `decision_index` is ignored (it may differ).
- `atx-impl/tools/test_compare_window_overlap.py` (new, 14 tests, synthetic roles only, all sessions in 2020-2021):
  identical overlap with instrument-union growth and interleaved columns; changed cell found with the right session
  and id (earliest first, new-only session ignored); NaN handling (different NaN payloads equal, NaN vs value unequal
  and not in max_abs_diff); old value without a new cell counted missing; `--before`; seal refusal (2024-01-02 role
  session, `--before` past the seal); wrong role refused and no overwrite; signal matched by candidate id across
  different `fp_` directories with another role's entry and an IC-result record ignored; `--*-run` resolving an
  ambiguous id; corrupt payload refused unless `--no-verify`; daily_ic aligned by (id, date) with a shifted decision
  index, `--before`, sealed row refused; seal value 2024-01-01 from the module or the fallback.

## How root verifies

- `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_compare_window_overlap.py`
  -> `14 passed` (run here). No C++; no build target.
- R13 calls (the runbook's lines need the role/run arguments and `--before`; field overlap over the whole overlap):

```bash
"$PY" atx-impl/tools/compare_window_overlap.py --kind field --per-key \
  --old build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 --new build-equity/train-2020-2023-lo1-fields-v9 \
  --out build-equity/w0-2-overlap-field-lo1.json
OLDROLE=$("$PY" -c "import json;print(json.load(open('build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9/manifest.json'))['role']['path'])")
"$PY" atx-impl/tools/compare_window_overlap.py --kind signal --per-key --before 2022-09-30 \
  --old build-equity/mega-candidate-cache-v71 --new build-equity/mega-candidate-cache-v8-lo1 \
  --old-role "$OLDROLE" --new-role build-equity/train-2020-2023-lo1 \
  --old-run build-equity/mega-v71-train-u-1 --new-run build-equity/w0-2-v71-u-lo1-1 \
  --out build-equity/w0-2-overlap-signal-lo1.json
"$PY" atx-impl/tools/compare_window_overlap.py --kind daily_ic --per-key --before 2022-09-30 \
  --old build-equity/mega-v71-train-u-1 --new build-equity/w0-2-v71-u-lo1-1 \
  --out build-equity/w0-2-overlap-daily-ic-lo1.json
```

  Ruling W0-a reads `totals.bit_identical`, `totals.max_abs_diff` (< 1e-9 or not) and `differing_keys`; the per-key
  rows name the first differing session and instrument. Expected by construction (runbook R13): `me_company`,
  `sv_ratio126` and every candidate reading them may differ; `regsho_threshold_days63` differs (BLOCKER 1).

## Deviations

- `--old-role/--new-role` are required for `--kind signal`, and `--old-run/--new-run` are optional (recommended:
  without them an id with several entries on the old role, e.g. field-payload variants from v7.0 and v7.1, is
  reported `ambiguous` rather than guessed).
- The run summary, not `train_candidates.jsonl`, names the entries (see above).

## Cross-lane edits

None.

## Open risks

- Seal fallback: until W0-1 merges, the literal 2024-01-01 is used (report `seal.source` says so). After the merge
  the guarded import picks `research_window.SEAL_NS` with no edit; root may delete `SEAL_FALLBACK_NS` then.
- Run time is I/O bound: field kind reads each payload twice with verification (~7.6 GB for 63 fields at 3y+4y
  shapes); `--no-verify` halves it.

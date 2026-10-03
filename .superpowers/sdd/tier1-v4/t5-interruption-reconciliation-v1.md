# Task5 interrupted fund reconciliation

2026-10-03, bounded operational follow-up. No builder retry, HTTP, data mutation, source cleanup, or foreign-path changes occurred. The original exec session 51062 returns `Unknown process id`; its exit status is unavailable. Root observed guard 18584 and child 18760 absent. The inspection guard independently reclaimed their stale slot with `job_exists=false`, `pid_match=false`. The original running receipt/logs remain untouched; termination cause is not established.

Inspection command (cwd `C:/atx/atx-db`):

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.4 --wait-minutes 0 --receipt ../.superpowers/sdd/tier1-v4/logs/t5-interruption-inspect-v1.receipt.json --stdout ../.superpowers/sdd/tier1-v4/logs/t5-interruption-inspect-v1.out.log --stderr ../.superpowers/sdd/tier1-v4/logs/t5-interruption-inspect-v1.err.log --quiet -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t5-interruption-inspect.py
```

Completed exit 0, 3.0 seconds, peak 0.0439 GiB under 0.4 GiB cap. The slot is released. Evidence: `logs/t5-interruption-inspection-v1.json` (all code SHA256 pins and work-file SHA256/size/footer-row inventory); `logs/t5-interruption-inspect-v1.receipt.json` (resource/admission/exit evidence). No data process remains owned by this agent.

Immutable source manifest SHA256: `04a6a697922e22a34738ef5993fe0b4564e542f2dbc44737790b8c33289006fe`. Approved extraction plan SHA256: `490dcb2f4632367dfc128b5ea8929a4e415ac7ed662e86f6b83910d18c46a447`. Source and interrupted target have exact one-to-one approved receipt/data membership: 85 data batches, 598,483,687 data bytes. Each actual source and target payload hash/size matches its receipt and source manifest; all copied receipt hashes and plan match. This certifies extract identity only, not economic/source qualification or acceptance gates.

Current code byte pins include fundamentals `080059f939105ae9296d234cf3018ac2bc927e799f2eed79fd0a2eeafa861870`, fund_depth `d14100b9daf3b9f0122d0847ef3f5e80a64f5df498aaf1cc683a1334b90482bd`, wrapper `575d1e18e22fd4cf880fa84f8b105892f98e1c0e2e018eef253d5a72ed6a6408`; remaining module pins are in the inspection JSON. Task5 implementation commits remain `7c929c5c`, `fbee8d54`, `9221b5d0`, `5ca0e6db`.

State: no stage-root files, manifest, finalized events/history/catalog, or part receipts. Prepare produced sub_clock (433,717 rows), class_shares (158,811), pos_sums (70,727), and pre_flags quarter parts through 2013q4. `pre_flags/2014q1.parquet.partial` is zero bytes. Existing readable Parquet footers do not prove full payload correctness or completion. No prepare receipt or `prepare done` line exists.

Concrete engineering defect: inherited `fundamentals.cached_copy` accepts a cache on SQL plus FSDS size/mtime fingerprint and readable footer row count; the sidecar does not bind cache payload SHA256. `_fingerprint` itself binds FSDS metadata rather than source bytes. This is an unresolved operational-certification gap, separate from unmeasured data gates. This follow-up makes no production cache-format change. The conservative workaround is recomputation rather than treating existing prep caches as certified.

Accepted recovery plan, to be executed by root later under the unchanged guard, after scheduling Task2:

1. Verify the inspected code pins and source manifest/plan pins still match, no producer is live, stage-root manifest/Parquet remain absent, and forensic destination is absent. Resolve/check both absolute paths are children of `C:/atx/atx-db/data/alpha_panel/v1/fundamentals_v11`.
2. In a bounded guarded helper, rename **only** `C:/atx/atx-db/data/alpha_panel/v1/fundamentals_v11/_work` to `C:/atx/atx-db/data/alpha_panel/v1/fundamentals_v11/_interrupted_work-r2-20261003T211353Z`. No deletion, no overwritten destination, no old-source/v10 mutation. Preserve original receipt/logs. This retains forensic/rollback evidence and avoids copying unbounded payloads.
3. Launch the wrapper below as a root-owned sustained guarded job. It revalidates every approved source extract hash/size and plan before recreating `_work/cf` via hardlinks; preparation starts fresh. Do **not** use `--skip-prepare`. No fixture is claimed for cache strengthening because that alternative was not implemented.

```powershell
# cwd C:/atx/atx-db; root owns this session through completion
$env:PYTHONPATH='C:/atx/atx-db/src'
$env:OPENBLAS_NUM_THREADS='1'
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/logs/t5-fresh-r3-fund.receipt.json --stdout ../.superpowers/sdd/tier1-v4/logs/t5-fresh-r3-fund.out.log --stderr ../.superpowers/sdd/tier1-v4/logs/t5-fresh-r3-fund.err.log --quiet -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t5-publish.py fund
```

The future paths above must be confirmed unused before launch. No automatic fund-check/std/ratios follows. Output physical integrity checks and measured acceptance gates remain pending. GC3 requires Task2 final universe; Task4 remains OPEN. A command exit 0 alone will not establish acceptance.

# DG1 - implementation report

Root acceptance: one independent review clean. Fourteen focused disk checks
passed with six repaired SA1 checks in `archive-disk-focused3`, native peak
0.584323883GiB under 1GiB. The real controller used the data path/3GiB floor.
Actual Windows preflight `disk-guard-native-refusal1` returned
`refused_low_disk` before child launch under an intentionally impossible floor.
No production disk pressure was induced. Ready to commit.

Status: candidate complete; root's independent review and focused runtime checks
are pending. No Python, pytest, database/archive opens or heavy workloads were
run by the implementer. Scoped native Ruff and `git diff --check` were used;
Ruff's import-order finding was repaired; the final scoped Ruff and diff checks
passed. No commit yet.

`run_memory_guarded.py` now accepts paired `--disk-path` and
`--min-free-disk-gb` options, rejecting missing partners and zero, negative or
non-finite thresholds before launching a child. With neither option, previous
memory behavior is unchanged. The target is resolved with `strict=True` before
`shutil.disk_usage`, so a missing target cannot silently select an ancestor's
volume. Receipts contain only `path`, `free_gb` and `min_free_gb` for disk evidence;
unavailable measurements record null free space and no operating-system error
text.

Preflight returns 78 with `refused_low_disk` or
`refused_disk_measurement_error`. During the existing one-second guard loop,
disk failure records `stopped_low_disk` or `stopped_disk_measurement_error` and
calls the same existing `TerminateJobObject(job, 137)` path used for memory
emergencies. It targets only the supervisor's already-owned Windows job and
descendants. Equal-to-floor free space is allowed; strictly less is refused or
stopped. Native cap creation/verification, preflight memory margin, physical and
commit hard stops, process launch and existing reporting cadence are unchanged.

The focused test module imports the controller without invoking its Windows
entry point. It checks the resolved measured path, exact floor boundary,
measurement failure, missing target and paired positive finite option policy.
Root should run the single module without workers in its guarded resource slot:

```powershell
.venv\Scripts\python.exe -m pytest tests/test_operator_disk_guard.py -n 0 -o addopts= -q
```

Upcoming archive18 must explicitly add `--disk-path C:\atx\atx-db\data
--min-free-disk-gb 3` to the controller arguments, before its `--` child-command
separator. No production launch was performed by this task. One-second polling
cannot reserve disk or prevent a burst from crossing the floor between samples;
the floor is operational protection, not capacity or completion evidence.

No backup, source or stash action occurred. `stash@{0}` is untouched.

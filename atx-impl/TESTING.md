# atx-impl DB Testing

Use the targeted dev runner for normal local work:

```powershell
python atx-impl\scripts\db_dev_tests.py
```

It selects tests from changed `atx-impl/db` and `atx-impl/scripts` files and runs
with `-n 0` by default, which keeps the workstation responsive. Useful modes:

```powershell
python atx-impl\scripts\db_dev_tests.py --smoke
python atx-impl\scripts\db_dev_tests.py --list
python atx-impl\scripts\db_dev_tests.py --full --workers 4
python atx-impl\scripts\db_dev_tests.py --kill-stale
```

The full suite remains the release/sprint-close gate, but do not use it as the
default edit-compile loop. `pytest -n auto` can spawn many DuckDB template-copy
workers and saturate Windows IO/CPU. The project pytest default is intentionally
bounded at `-n 4`; use `--workers auto` only for a dedicated full-suite run.

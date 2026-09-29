# tier1-v3 progress

One line per task event (UTC).

- 2026-09-29 22:25Z session start: owner goal = implement v3 plan, alpha focus on last 6 years, reclaim disk.
- 22:31Z S0.1 Reg SHO build PASS (NYSE combined landing complete, 2179 receipts to 2026-09-25): 40 s, 0.62 GiB peak.
- 22:33Z S0.1 panel assemble 2018-2021 crashed 0xC0000005 (shared DuckDB temp dir with the parallel 2022-2026 run);
  fix in `common.connect` (per-pid spill dir); 2018-2021 to rerun after 2022-2026.
- 22:50Z S0.5 rulings D1-D8 recorded in CARRY.md. Warehouse catalog tables archived (49 tables, 4.2 MB).
- 22:55Z disk batch 1: +49 GB free (29 → 78 GB).

### Task T33a: delisting-data feasibility (explorer, read-only)

**Lane:** EXP · **Model:** Opus 5.5 · **Depends on:** T29 · **Peak:** 0.6 GiB (DuckDB read-only ≤ 384 MB, ≤ 2 threads)

**Files:** Create `.superpowers/sdd/mega-alpha-20260926/delisting-feasibility.md`. Read-only: `C:/atx/atx-db/data/warehouse.duckdb` (`sec_submissions`: form, items, filing date, CIK), `build-equity/identity-bridge-r4-v1`, TRAIN role member ends, `exchange_listings` (0 venue rows per tier1 status — confirm).

- [ ] **Step 1:** For every TRAIN member instrument whose presence ends before 2022-12-31 (from the role's `present`), find via the bridge the CIK and list filings within [−60, +30] calendar days of the last present session: 8-K items (1.03, 2.01, 3.01), Form 25, 15-12G/15-15D, 10-K/10-Q continuation. Classify: `mna` (2.01 or acquirer-named 8-K), `performance` (3.01 / 25 / 1.03), `unknown`.
- [ ] **Step 2:** Report counts and shares by class for TRAIN (and, only as a count with no returns, VAL), venue availability, and the last-close vs any OTC continuation in tickerhistory.
- [ ] **Step 3:** Recommend GO/NO-GO: GO if ≥ 80% of TRAIN terminations are classifiable (`mna`+`performance`); else NO-GO (park lane, raise U3).
- [ ] **Step 4:** Commit doc: `docs(mega-alpha): delisting-return feasibility (T33a)`.

**Acceptance:** counts table + GO/NO-GO with the exact SQL used; no locks taken (open DuckDB `read_only=True`; abort if the file is locked, never wait).

---


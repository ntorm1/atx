# T25 — fundamentals fields QA / PIT audit script (TRAIN only; root runs)
Spec: T18 report (task-T18-report.md) §7 "T25 details", plus:
- Inputs (args, defaults under C:/atx-wt/pool-2/build-equity/): TRAIN fields-v5 recent-fast-train-2020-2022-v2-fields-v5
  (manifest 4e02b7db...), role recent-fast-train-2020-2022-v2, identity-bridge-r4-v1, fundamental-events-v1 (manifest
  519ecc1a...), and the raw sources the events came from (paths are in the events manifest/run.json; read-only).
  VAL fields-v5 manifest may be read for METADATA ONLY (field definitions identity TRAIN vs VAL) — never VAL payloads.
- Checks (each prints PASS/FLAG with numbers): (1) per field x year coverage on member cells and common-stock member
  cells; NaN reasons (no link / no event / stale / domain); (2) 50 random (id, date) cells per core item (be, at,
  cfo_ttm, ni_ttm, sale_ttm, shrs_q) reconciled to raw CompanyFacts facts + FSDS accepted_utc: value equal (tol 1e-9
  rel) and clock <= decision mark minus 1 session; (3) restatement vintage counts, FC1 fallback counts; (4) SPLIT
  CONTAMINATION: for TRAIN role names with a split in the window (raw_close/close factor change), compare shrs_q vs
  shrs_q_lag4 and shares_out vs delay(shares_out,252) around the split — report share of |log ratio| > log(1.5)
  explained by the split factor; (5) ratio sanity distributions (be/me_company, gp_ttm/at, cfo_ttm/at, sue, noa/at):
  quantiles, share of |x| > 10, sign of me_company; (6) FF12/FF49 group sizes per day among finite-fundamental
  members (min, share of groups < 5); (7) TRAIN/VAL field-definition identity (manifest text/blob only).
- Output --output DIR: audit.json + audit.md. Bounded <= 180 s / <= 1536 MiB per invocation, split into --checks groups.
- Deliver .superpowers/sdd/mega-alpha-20260926/studies/audit_fund_fields.py (+ synthetic pytest), git add -f.
- Never compute return/IC/performance statistics of any signal (this is a data audit, selection-neutral).

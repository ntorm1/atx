# Task 4 report

Status: DONE_WITH_CONCERNS. Agent: Codex, GPT-6 family; finer runtime model identity is not independently exposed. Reconciled WIP commit fd9d67b7. Implementation commits: e49b2b0a and 5fd191bb. Documentation/report commit is returned separately.

Published schema atx.alpha-panel.ratios/v2, rule wfrs-pit-v2. The suite contains 66 primary ratio columns, 3 distinctly named extensions and 4 composite scores. Issuer state has 306939 rows across 14134 CIKs, available from 2015-01-02 16:50:00 through 2026-09-19 22:00:00 UTC. Stage files use 342529081 bytes.

|year|monthly rows|first session|last session|
|---|---|---|---|
|2018|56960|2018-01-31|2018-12-31|
|2019|59513|2019-01-31|2019-12-31|
|2020|60220|2020-01-31|2020-12-31|
|2021|68968|2021-01-29|2021-12-31|
|2022|75847|2022-01-31|2022-12-30|
|2023|72299|2023-01-31|2023-12-29|
|2024|69350|2024-01-31|2024-12-31|
|2025|68127|2025-01-31|2025-12-31|
|2026|45628|2026-01-30|2026-08-31|

Coverage/score window: 2020 onward; 2018-2019 materialization is retained as lookback. Latest complete input-calendar month is August 2026. No return statistics were computed.

## Acceptance gates

|criterion|status|measured|command|
|---|---|---|---|
|Coverage: member_equity|FAIL|Primary 29/66; extensions 3/3; composites 0/4 meet 90% in every year 2020-2025|validate|
|Coverage: univ_all_common|FAIL|Primary 28/66; extensions 3/3; composites 0/4 meet 90% in every year 2020-2025|validate|
|Coverage: univ_r3000p|FAIL|Primary 19/66; extensions 2/3; composites 0/4 meet 90% in every year 2020-2025|validate|
|Same-date Spearman|FAIL|Minimum date rho: bm 0.9682, ep 0.9833, sp 0.9875, gpa 0.9619; required >=0.99|validate|
|p1/p99 ranges|FAIL|5 failing/unsupported columns: pe_exi, pe_inc, peg_trailing, divyield, ptpm|validate|
|Source recomputation|FAIL|20 issuers x 2 dates x 36 formulas = 1,440 cells: {"MISMATCH": 2, "absent_in_filing": 292, "absent_in_ratios": 7, "both_absent": 142, "match": 997}|handcheck --n 20|

Acceptance: 0 PASS / 6 FAIL. No gate was waived. Extensions cannot improve the primary-66 acceptance count. Range envelopes are indicative diagnostics, without complete source-backed literature certification. All quantiles, correlations and yearly eligible counts are retained in ratios.gates.json and ratios.coverage.json.

PTPM range measurement: {"n": 153034, "p1": -19.3497, "p50": 0.0893, "p99": 2.2911, "pass": false, "range": [-200, 2], "source": "margin"}

## Verification and exact commands

Offline fixture command from C:/atx: `atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts="" atx-db/tests/test_alpha_panel_ratios.py -q`, with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1. Result: **21 passed in 10.40s**. Owned-path git diff --check passed.

Tests cover monthly strict cutoffs, the prior-session daily cutoff, successive same-accession clocks, annual comparative restatements reported in later 10-Q filings, missing prior-period averages, composites, split/class/cover-date adverse cases, explicit unsupported primary P/E nulls, and custom-stage macros.

Real monthly diagnostics: strict fundamental-clock violations, missing row clocks and duplicate monthly keys = [0, 0, 0]. This is an informational PIT PASS, separate from the six acceptance gates. Manifest module SHA matches current code: True. Input standardized-stage hash matches the build manifest: True.

Sequential real commands from C:/atx/atx-db, using the environment above:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 1.0 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-build.receipt.json --stdout ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-build.out.log --stderr ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-build.err.log --quiet -- .venv/Scripts/python.exe -m atx_db.alpha_panel.ratios build --buckets 32
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-validate.receipt.json --stdout ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-validate.out.log --stderr ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-validate.err.log --quiet -- .venv/Scripts/python.exe -m atx_db.alpha_panel.ratios validate
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-handcheck.receipt.json --stdout ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-handcheck.out.log --stderr ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-handcheck.err.log --quiet -- .venv/Scripts/python.exe -m atx_db.alpha_panel.ratios handcheck --n 20
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-audit.receipt.json --stdout ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-audit.out.log --stderr ../.superpowers/sdd/tier1-v4/logs/t4-postcheck-audit.err.log --quiet -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t4-final-audit.py
```

Each command used the memory guard at <=1 GiB. Receipts contain elapsed time and peak memory. Final build completed in 84.0s. Superseded intermediate measurements are not acceptance evidence.

## Independent source-check scope and causal exceptions

Checked 36 formulas (33 primary, 3 extensions, no composites): ps, pcf, pe_issuer_ni, pe_issuer_ib, bm, ptb, divyield_paid, dpr, npm, opmad, gpm, roe, at_turn, accrual, debt_at, curr_ratio, de_ratio, fcf_ocf, rd_sale, gprof, sale_equity, sale_nwc, curr_debt, invt_act, rect_act, cash_lt, cash_ratio, quick_ratio, ocf_lct, ptpm, efftax, cfm, opmbd, roa, intcov, intcov_ratio.

Unverified 37 formulas (33 primary, 4 composites): capei, evm, pe_exi, pe_inc, peg_trailing, divyield, roce, aftret_eq, aftret_invcapx, aftret_equity, pretret_noa, pretret_earnat, equity_invcap, debt_invcap, totdebt_invcap, capital_ratio, int_debt, int_totdebt, debt_ebitda, short_debt, lt_debt, profit_lct, cash_debt, lt_ppent, dltt_be, debt_assets, debt_capital, cash_conversion, inv_turn, pay_turn, rect_turn, sale_invcap, adv_sale, piotroski_f, altman_z, ohlson_o, beneish_m. The sample does not certify the entire suite.

Every exception has CIK, accession, datadate, session date, filing-derived values and state primitive comparisons in t4-hand-attribution.json. Source-chain absence is unverified, rather than proof that a primitive does not exist. Missing prior balances remain NULL.

Expanded source checks found a local cash extractor omission: CIK 1802665, accession 0001558370-25-001441, period 2024-12-31 has $453.001m cash plus $14.185m reported ShortTermInvestments = $467.186m standardized CHE. The independent extractor now includes that actual component. Both cash-ratio discrepancies were eliminated without adjusting published ratios or assuming a zero.

Two ROE numerical failures remain. CIK 1280263 / 0001564590-21-016988 / 2021-01-31 uses prior-year TXDITC=0, while that filing reports $1.288m DeferredIncomeTaxLiabilitiesNet at 2020-01-31. CIK 1818201 / 0001193125-22-061328 / 2021-12-31 uses prior-year TXDITC=0, while the filing comparative is $322.348m at 2020-12-31. Current-period TXDITC matches both filings. State values and original source facts are retained in t4-source-cases.json. Task 5 must reconcile prior catalog/default/history concepts and clocks; no ratio-specific correction factor or balancing plug was applied.

Other source-chain failures include preferred stock/deferred tax, issuer-custom operating income, dividends, D&A, interest, R&D and capex. Some standardized values are source-stage explicit zeros or derived values without a matching narrow inline-XBRL chain. Task 5 must audit the keyed source defaults/concepts; these are not generically classified as upstream corruption.

## Naming, primitive needs and downstream contract

The WRDS manual appendix governs named counterparts. pe_exi/pe_inc remain diluted-EPS definitions and currently publish NULL for missing certified security-class/ADR/EPS/split/date alignment. PEG_trailing inherits that missing prerequisite. Task 5 supplies EPS histories; the market-data producer supplies class/ADR/share-basis alignment. No NI/IB-scaled EPS is invented.

Valid cap/earnings products are separately named pe_issuer_ni and pe_issuer_ib. Paid TTM dividend yield is divyield_paid. Primary divyield is NULL because an indicated dividend rate is unavailable; Task 13 distributions must supply that primitive. The gold registry must retain these distinct identities.

Fixed local formula/PIT defects: preserved later annual comparative vintages and successive clocks; removed source liability A-E plugs, derived missing-zero assumptions and current-only average substitution; replaced observed-missingness structural inference with explicit sector applicability; corrected NI profitability numerators, ROE SEQ+TXDITC equity, debt_at vs debt_assets, invested-capital denominator, EBIT decomposition and cash-conversion balance definitions. Same-date comparisons preserve actual timing/market-source disagreements instead of normalizing them to pass.

ratios.requirements.json maps formulas, sources, clock, staleness, sign/zero/history/fiscal policies, roles, downstream names and producer needs. Task 5 owns the complete profitability/growth primitive matrix and recovery; Tasks 7/16 consume only explicit formulas. Task 3 remains authoritative schema v2/cstd-pit-v3, including available_at in physical vintage identity. Its 5 PASS / 3 FAIL source gates are unchanged; balance/Q4 route to Task 5 and rdq to Task 9.

## Provenance, sources and files

Standardized input stage: compustat_std. Manifest SHA256: 9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325. Rebuild chain: compustat_std build --fund-stage fundamentals_v11 --stage <standardized>; ratios build --std-stage <standardized>. Raw fundamentals is not a direct ratios input.

Universe validation SHA256: 33a15585807fa242fc4e7eb38ec3c1902bf043ba442f57b2e2a8f45dbad7071d. Primitive-audit universe SHA256: 33a15585807fa242fc4e7eb38ec3c1902bf043ba442f57b2e2a8f45dbad7071d. Refresh coverage after concurrent universe refinement.

Retained SEC 10-K primary documents: 400953830 bytes across 80 retained files, including initial-run duplicates. URLs follow https://www.sec.gov/Archives/edgar/data/<cik>/<accession>/<primary>. receipts.jsonl retains URL, bytes, SHA256, HTTP status and retrieval clock. Sample periods span 2020-2025; cadence annual/as-needed amendments; public SEC terms; sec_http rate <=5 requests/s. No landing exceeded 1 GB. Originals are under data/raw/sec_ratios_handcheck; initial-run duplicates were moved into its initial-build subdirectory. Existing standardized fundamentals, prices and shares were reused.

Owned files: ratios.py, test_alpha_panel_ratios.py, ratios.md, ratios.registry.json, ratios.requirements.json, ratios.coverage.json, ratios.gates.json, Task 4 report and audit artifacts. Foreign-dirty files, shared registries and other task files were untouched. No child agents. Explicit-pathspec commits retain the required GC6 trailer; actual agent identity is stated above.

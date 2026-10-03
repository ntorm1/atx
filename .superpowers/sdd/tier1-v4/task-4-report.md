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

Coverage snapshot update: Task 2 subsequently published an intermediate universe and will publish its final universe after the remaining status audit. The recorded 33a15585807fa242fc4e7eb38ec3c1902bf043ba442f57b2e2a8f45dbad7071d coverage snapshot is therefore historical, not current universe coverage. GC3 refresh is pending the final Task 2 hash; the controller will coordinate one coverage refresh. No full ratio rebuild or relabeling of the earlier measurements was performed.


## Fix round 1 ? review-T4-r0

R1 addresses I1/I2/I3 locally; I4 remains open. 26 focused offline tests PASS. Missing retained history/FY0 is now in the coverage denominator and separately exposed in `ratio_history_missing`; only genuine zero denominators and explicit sector applicability exclude cells. Empty structural-mask mappings produce valid SQL. Observed missing-input wording no longer calls absence structural.

Identity links and latest dated FINRA classification require `available_at < cutoff`, using strict prior-calendar-session22UTC daily and month-end22UTC monthly. Same-session newly known links cannot select earlier fundamentals. No all-time modal FINRA classification participates in eligibility. Missing dated FINRA evidence remains unknown, governed by the visible common-link contract; this is not independent historical class certification.

Quarter pairing requires fiscal-quarter progression, fiscal-year consistency and unchanged fiscal calendar as well as elapsed-day tolerance. Annual pairing requires fiscal-year progression, unchanged calendar, and preceding end dates350?380days apart. Tests retain raw annual rows while excluding a335day comparative and a fiscal-calendar transition from annual history pairing. Rejected annual history makes annual/fallback ratios NULL; missing history remains eligible. The standardized producer does not expose period start/duration: this end-date screen is **not source-backed full-period duration certification**. Task5 must publish dated period start/end or a verified full-period/comparability flag. Missing certification remains an explicit consumer qualification, not an equivalence claim.

Ohlson still uses nominal assets/OCF proxy; complete formula/composite source-model certification remains unverified. No hard gate is waived. Main independent arithmetic covers33primary+3extensions; two additional existing-primitive formulas (`aftret_equity`, `profit_lct`) are checked separately by t4-r1-extra-sourcecheck.py.31primary and4composites remain unverified. Both-absent cells never certify formulas. No broader source discovery was performed.

The guarded R1 rebuild/validate/handcheck/extra-sourcecheck commands use `run_memory_guarded.py --job-gb`1.0/.8/.6/.6 respectively, receipt/stdout/stderr paths `logs/t4-r1-{build,validate,handcheck,extra-sourcecheck}.*`, with child commands `-m atx_db.alpha_panel.ratios build --buckets32`, `validate`, `handcheck --n20`, and `../.superpowers/sdd/tier1-v4/t4-r1-extra-sourcecheck.py`. Working directory C:/atx/atx-db; PYTHONPATH=C:/atx/atx-db/src; OPENBLAS_NUM_THREADS=1. Offline command from C:/atx: `atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' atx-db/tests/test_alpha_panel_ratios.py -q`;26passed in19.44s.

GC3 remains pending. R1 validation overlapped Task2 partial output publication: its all-common/R3000 measurements are **provisional/invalid for final acceptance**, regardless of recorded manifest hash. The controller must refresh against completed final output-integrity hashes. Earlier33a15585 measurements remain historical; neither snapshot is relabeled latest.

Exact latest evidence follows (t4-r1-evidence.json); previous sections describe the pre-review snapshot:


The four previously mismatching issuer-date cash cases were local independent-extraction defects: add actually reported MarketableSecuritiesCurrent or use explicitly reported CashCashEquivalentsAndShortTermInvestments. Retained t4-r1-source-cases.json records exact facts (no source-value fitting); the final module fixes these chains and fixtures cover priority/no double counting. Final build preserves source-hash lineage; main sourcecheck was rerun. Coverage/range/correlation formulas were unchanged by this sourcecheck-only correction, so their prior R1 measurements remain applicable with the stated universe invalidity.

Task5 source/history gaps and Task13 indicated dividends remain open. Primary coverage is24/66 member; extensions2/3; composites0/4. Correlations remain below.99 and unsupported/range failures remain open. Provisional all-common/R3000 counts19/66 and11/66 are not valid final acceptance evidence. Preliminary attribution execution from C:/atx failed on a relative artifact path (exit1); successful attribution-2 used C:/atx/atx-db.

```json
{
  "code_matches_manifest": true,
  "input_matches_manifest": true,
  "input_sha256": "9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325",
  "coverage_snapshot_status": {
    "is_latest_universe": false,
    "gc3": "PENDING controller final publication and output-integrity verification",
    "overlap_coverage": "provisional/invalid for final acceptance: Task2 partial publication overlapped validation",
    "measured_manifest_sha256": "d30ef0408b1501bea38fc7e733da7e99df4d4bc9156e3f23b3d65ba39a46031e",
    "manifest_hash_alone_certifies_output_integrity": false
  },
  "gates": {
    "coverage": "FAIL",
    "coverage_univ_all_common": "FAIL",
    "coverage_univ_r3000p": "FAIL",
    "handcheck": "FAIL",
    "ranges": "FAIL",
    "spearman": "FAIL"
  },
  "coverage_roles": {
    "coverage": {
      "composites": {
        "fails": [
          "piotroski_f",
          "altman_z",
          "ohlson_o",
          "beneish_m"
        ],
        "n_pass": 0,
        "total": 4
      },
      "extensions": {
        "fails": [
          "divyield_paid"
        ],
        "n_pass": 2,
        "total": 3
      },
      "primary": {
        "fails": [
          "capei",
          "bm",
          "evm",
          "pe_exi",
          "pe_inc",
          "dpr",
          "peg_trailing",
          "divyield",
          "opmbd",
          "opmad",
          "gpm",
          "ptpm",
          "cfm",
          "roa",
          "roe",
          "roce",
          "efftax",
          "aftret_invcapx",
          "pretret_noa",
          "pretret_earnat",
          "gprof",
          "int_debt",
          "int_totdebt",
          "invt_act",
          "rect_act",
          "debt_ebitda",
          "curr_debt",
          "profit_lct",
          "ocf_lct",
          "debt_capital",
          "intcov",
          "intcov_ratio",
          "cash_ratio",
          "quick_ratio",
          "curr_ratio",
          "cash_conversion",
          "inv_turn",
          "pay_turn",
          "rect_turn",
          "sale_invcap",
          "sale_nwc",
          "adv_sale"
        ],
        "n_pass": 24,
        "total": 66
      }
    },
    "coverage_univ_all_common": {
      "composites": {
        "fails": [
          "piotroski_f",
          "altman_z",
          "ohlson_o",
          "beneish_m"
        ],
        "n_pass": 0,
        "total": 4
      },
      "extensions": {
        "fails": [
          "divyield_paid"
        ],
        "n_pass": 2,
        "total": 3
      },
      "primary": {
        "fails": [
          "capei",
          "bm",
          "evm",
          "pe_exi",
          "pe_inc",
          "ps",
          "dpr",
          "peg_trailing",
          "divyield",
          "opmbd",
          "opmad",
          "gpm",
          "ptpm",
          "cfm",
          "roa",
          "roe",
          "roce",
          "efftax",
          "aftret_invcapx",
          "pretret_noa",
          "pretret_earnat",
          "gprof",
          "int_debt",
          "int_totdebt",
          "invt_act",
          "rect_act",
          "debt_ebitda",
          "curr_debt",
          "profit_lct",
          "ocf_lct",
          "fcf_ocf",
          "dltt_be",
          "debt_capital",
          "intcov",
          "intcov_ratio",
          "cash_ratio",
          "quick_ratio",
          "curr_ratio",
          "cash_conversion",
          "at_turn",
          "inv_turn",
          "pay_turn",
          "rect_turn",
          "sale_invcap",
          "sale_nwc",
          "rd_sale",
          "adv_sale"
        ],
        "n_pass": 19,
        "total": 66
      }
    },
    "coverage_univ_r3000p": {
      "composites": {
        "fails": [
          "piotroski_f",
          "altman_z",
          "ohlson_o",
          "beneish_m"
        ],
        "n_pass": 0,
        "total": 4
      },
      "extensions": {
        "fails": [
          "divyield_paid"
        ],
        "n_pass": 2,
        "total": 3
      },
      "primary": {
        "fails": [
          "capei",
          "bm",
          "evm",
          "pe_exi",
          "pe_inc",
          "ps",
          "pcf",
          "dpr",
          "peg_trailing",
          "divyield",
          "npm",
          "opmbd",
          "opmad",
          "gpm",
          "ptpm",
          "cfm",
          "roa",
          "roe",
          "roce",
          "efftax",
          "aftret_invcapx",
          "pretret_noa",
          "pretret_earnat",
          "gprof",
          "equity_invcap",
          "debt_invcap",
          "totdebt_invcap",
          "int_debt",
          "int_totdebt",
          "invt_act",
          "rect_act",
          "debt_ebitda",
          "curr_debt",
          "profit_lct",
          "ocf_lct",
          "cash_debt",
          "fcf_ocf",
          "lt_ppent",
          "dltt_be",
          "debt_capital",
          "intcov",
          "intcov_ratio",
          "cash_ratio",
          "quick_ratio",
          "curr_ratio",
          "cash_conversion",
          "at_turn",
          "inv_turn",
          "pay_turn",
          "rect_turn",
          "sale_equity",
          "sale_invcap",
          "sale_nwc",
          "rd_sale",
          "adv_sale"
        ],
        "n_pass": 11,
        "total": 66
      }
    }
  },
  "handcheck": {
    "issuers": 20,
    "pass": false,
    "rows": 40,
    "rule": "``n`` seeded linked member issuers (USD 10-K filers, issuer-primary line, no split between the 10-K and the\n    date), two month-end dates each: the first month-end on which each of the issuer's earliest and latest 10-Ks\n    2021-2025 is the state's lag-0 row (fiscal-year TTM path). For each, fetch the 10-K's primary document\n    (``sec_http``, <= 5 req/s), read its inline XBRL facts and recompute ``HAND_RATIOS`` from the filing's values\n    (``hand_items``) and the row's market inputs (me, prc, eps_adj); match = relative difference <= 1e-6.",
    "summary": {
      "absent_in_filing": 297,
      "absent_in_ratios": 1,
      "both_absent": 143,
      "match": 999
    }
  },
  "supplementary_counts": {
    "match": 59,
    "source_absent": 15,
    "both_absent": 6
  },
  "one_sided_evidence": "t4-r1-hand-attribution.json: keyed source primitive absence vs retained state; narrow extraction absence does not establish upstream corruption",
  "receipt_summary": {
    "build": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.6231,
      "run_seconds": 146.1
    },
    "validate": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.5433,
      "run_seconds": 19.0
    },
    "handcheck": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.2848,
      "run_seconds": 25.0
    },
    "extra-sourcecheck": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.082,
      "run_seconds": 1.0
    },
    "final-build": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.6282,
      "run_seconds": 135.1
    },
    "final-handcheck": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.2876,
      "run_seconds": 29.0
    },
    "attribution-2": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.1756,
      "run_seconds": 5.0
    },
    "source-cases": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.1577,
      "run_seconds": 2.0
    },
    "final-attribution": {
      "status": "completed",
      "returncode": 0,
      "cap_hit": false,
      "native_peak_job_memory_gb": 0.1753,
      "run_seconds": 5.0
    }
  }
}
```

Remaining one-sided ROE case: CIK1817358, accession0001817358-22-000039, FY2022-01-29, session2022-03-31. Filing prior TXDITC138.703m is present; eligible prior annual-state TXDITC is NULL. This is an unavailable retained/comparable annual-history path, not a demonstrated arithmetic defect or proof that the issuer primitive is absent. Task5 must reconcile source-history/period certification; no current-only average or tax plug is introduced.297 narrow-chain source absences remain uncertified. Final main arithmetic:999matches,143both-absent,297source-absent,1ratio-absent,0numerical mismatches; supplementary80cells:59matches,6both-absent,15source-absent.


## Fix round 2 ? retained I4, R10 and released R9

No ratio expression or PIT consumer was changed: I1/I2/I3 remain closed. Corrected the independent source auditor's debt extraction: current LTD alone is not total current debt, and total LongTermDebt is not noncurrent debt. Use explicitly reported DebtCurrent, or add actually reported current maturities and short borrowing; derive noncurrent debt from explicitly reported total minus current only when both exist. Missing components stay NULL. Removed a finite-lived-only intangible fallback that could omit indefinite-lived assets. These are source-audit corrections, not numerical fitting. The final ratio module/input hashes still match the published build; no formula rebuild was needed.

27 offline covering tests passed in14.43s. One new fixture checks independent ROCE/common equity/invested capital/Altman arithmetic and NULLs when NCI/source fields are absent. The exploratory fixture originally miscomputed average capital employed62.5; corrected expected value65 (current75, prior55), then the full focused file passed.

Every one of66 primary,3 extensions,4 composites now has a per-formula status, exact source fields/concept chains, missing-field counts, producer obligations and qualification in ratios.requirements.json. Retained40 issuer-date filings yielded2,920 classified cells:1,337 matches,1,002 source-absent,363 both-absent,11 numerical differences,7 ratio/history absences,200 contract-blocked cells (five primary formulas).57 primary have at least one positive numerical match; only 2 primary match all 40 cells. Neither partial matches, both-absent cells, nor documented-score arithmetic constitute full formula/source-model certification.

No positive source check exists for CAPEI, EVM, pe_exi/inc, PEG, divyield, ROCE, totdebt_invcap, debt_capital and Piotroski (nine primary plus one composite). The matrix states actual missing preferred/NCI/debt/interest/5-year NI history or class EPS/indicated-rate prerequisites; it does not call every unresolved formula generically unverified. Altman/Ohlson/Beneish have positive implemented-formula checks but source-model equivalence remains qualified; Piotroski lacks FY-2assets/issuance and comparable histories. Ohlson remains nominal-assets/OCF proxy; source GNP deflator/funds-from-operations identity is required for exact source-model equivalence.

All11 numerical differences independently reproduce the published arithmetic from exact retained state primitives. Causal evidence t4-r2-attribution.json isolates: CIK7084 FY2024 DLTT7.600b source versus7.580b state (six cells; debt net/gross/lease basis); CIK1415404 priorDLTT2.389168b versus2.389733b, priorNI-74.252m versus-62.917m, FY2023COGS9.510427b versus16.180753b and DLTT19.717266b versus0; CIK40545 priorNI9.482b versus9.483b; CIK1768224 RE-143.800m versus-143.816m. Seven one-sided cells follow missing comparable prior annual state history despite reported comparative values. These are precise Task5 source/basis/history reconciliation obligations, not proof every extraction value is the exact standardized economic primitive, and not a local ratio arithmetic failure. No zero plug/current-only average was introduced.

### Same-date characteristic comparison

R10: no published characteristics files exist, so the binding published-char_monthly comparison is BLOCKED/FAIL. Reconstructed char_registry/panel values are diagnostic proxies only.163,231 member month-end cells differ in issuer2,737, fundamental clock6,282, period6,184, reporting-currency label3,953, issuer cap144,881, shares147,879, and raw close0. Reporting currency label differences are not evidence of a money-unit error: both contracts publish converted USD values.

Proxy minimum per-date rho: bm0.980024,ep0.982803,sp0.988577,gpa0.961708. Paired counts/numerator differences: bm135,760/52,726; ep107,017/4,634; sp140,933/6,615; gpa103,346/11,659. Conditional cohorts in evidence are attribution diagnostics, never substitutes for the binding gate. Five keyed largest-relative-difference issuer samples per formula retain both currencies, cap/share/close inputs, issuer ids, clocks, dates, book/tax/preferred primitives and earnings/sales/gross-profit/assets.

Examples: CIK77159/session2022-02-28 has identical raw close33.759998 and fundamental clock, but ratios shares15,000 versus panel21,088,000 (issuer caps0.5064m versus711.9308m). This is an inherited market-share basis discrepancy, not established local ratio multiplication error. CIK1868778/session2024-07-31 has ratio filing at20:18that session (June quarter), while panel uses May2clock (March quarter); this is the correct monthly-versus-prior-session visibility distinction. CIK1815442/session2025-05-30 has panel GP0 versus ratio sale58.885m minusCOGS47.072m on identical date/clock; Task7 must reconcile source-derived GP/defaults. No unsafe foreign clocks or chain outputs were copied or edited.

Task7 recomparison contract is machine-readable: publish raw char_monthly on the same finalized universe, identical issuer/common line cap, USD money/FX provenance, strictly monthend22UTC identity/classification/fund clocks, same standardized accession/vintage/history/freshness/null/sign eligibility. BM uses actualSEQ+TXDITC-PSTK; EP is inverse issuer-NI extension (not diluted P/E); SP=SALE/ME; GPA=(SALE-COGS)/AT. Retain all stated lineage fields before comparing same-key raw values and testing rho>=.99.

### PTPM tails and unsupported range criterion

[WRDS Research Team manual2016, pp2-3](https://people.tamu.edu/~cschulman/ECMT461/Termp/WRDS_Industry_Financial_Ratio_Manual.pdf) defines raw firm-level ratios without truncation/smoothing; its1%outlier treatment applies to industry aggregation. It supplies no fixed firm-level PTPM[-200,2] band. Existing numeric envelopes are indicative diagnostics, not source-backed literature-range acceptance evidence. R10 keeps the binding literature gate UNVERIFIABLE/FAIL pending named reference publication/cohort/formula/universe/timing/treatment/threshold. No licensed WRDS acquisition, invented range or clipping occurred.

Measured PTPM147,952 cells: p1-17.28,median0.0922614,p992.4072077. Eight distinct issuer-period tail cases trace standardized annual amounts, exact catalog event and retained original CompanyFacts concepts/units/periods/accessions. CIK1080709 FY2021 pretax-616.433m and sales54k, CIK1668243 FY2019 pretax-105.146m and sales18k, CIK1326190 FY2022 pretax-84.910m and sales-68k are directly source-supported extremes. CIK1296774 FY2020 CNYpretax402.764770m /CNYsales625,488 equals the converted USD ratio643.920859; same-currency FX cancels, so this is not a ratio currency defect.

Source issues stay separate: CIK30625 FY2018/FY2020 source Revenues0 precedes explicit contractual revenue3.832666b/3.728134b, and annual restatement denominators become-1000/13000. Task5/standardization must repair total-sales concept selection/history while preserving source clocks; ratio arithmetic correctly consumes the published inputs. CIK1253986 financial sales185k versus reported InterestIncomeOperating466.087m requires financial-revenue/pretax scope certification. CIK1895618 tiny revenue256 is reported, but bounded PIcorroboration is NI rather than a certified pretax source. Do not label these unresolved PI/basis cases fully valid extremes.

### Final GC3 coverage refresh and HTTP accounting

Controller R9 released finalTask2manifest90e9ca70cec64838a96b5f7ffc0de88a28e1faf529b48b480c56f4b9d85d0c0e. Exactly one guarded final coverage refresh verified every 9 output file SHA/size before and after, with unchanged standardized input identity. Current primary passes remain24/66member,19/66all-common,11/66R3000; extensions2/3each and composites0/4each. This replaces R1 overlap-era provisional evidence. Stable final output is not universe parity: Task2French10/60FAIL remains. Hard gates remain0/6PASS; source, coverage, missing published comparator and unsupported literature reference are still open.

R2 makes zero new SEC HTTP attempts/retries/redirects; every source audit reuses retained filings/CompanyFacts. One web-tool read of the public TAMU-hosted WRDS manual is outside the SEC request budget; its backend HTTP-hop count is not exposed. Historical Task4 has seven recorded guarded handcheck executions and280 successful primary-document receipts. Exact all-attempt count is unrecoverable because ATX_SEC_RATE_LOG was not enabled; primary receipts omit headers/index/retries/redirects. A conservative upper bound for the recorded execution inventory is1,127 attempts, sum ceil(5*(child run_seconds+1))+1 per job under enforced shared5/sceiling, covering headers, optional index,6-attempt retry policy and redirects. This upper bound depends on complete guarded execution inventory; retained metadata cannot prove absence of an unrecorded historical invocation. Budget coordinator should reserve the upper bound, never280successes as total attempts. Exact receipts/inventory: t4-r2-http-budget.json and data/raw/sec_ratios_handcheck/{receipts.jsonl,initial-build/receipts.jsonl}.

### Actual guarded commands and results

Working directoryC:/atx/atx-db; PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1. Each command below uses `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb <cap> --wait-minutes60 --receipt ../.superpowers/sdd/tier1-v4/logs/<name>.receipt.json --stdout .../<name>.out.log --stderr .../<name>.err.log --quiet -- .venv/Scripts/python.exe <child>`. Receipts are copied to receipts/. All successful named checks stayed below1GiB; no external return statistics or foreign chain was executed.

Exploratory schema check found zero published characteristic files. Comparability-2 failed because glob included *.stats.parquet; numeric-only batchglob corrected in comparability-3. Sourcecheck initial failed because historical records omit security_id; use their issuer-primary published row explicitly. Corrected debt/intangible extraction and finalized per-field metadata in sourcecheck-final. No failed exploratory run is acceptance evidence.

|receipt name|cap GiB|child command|exit|peak GiB|seconds|
|---|---|---|---|---|---|
|t4-r2-attribution-final.receipt.json|0.6|../.superpowers/sdd/tier1-v4/t4-r2-attribution.py|0|0.2213|2.0|
|t4-r2-attribution.receipt.json|0.6|../.superpowers/sdd/tier1-v4/t4-r2-attribution.py|0|0.221|3.0|
|t4-r2-comparability-2.receipt.json|0.8|../.superpowers/sdd/tier1-v4/t4-r2-comparability.py|1|0.4687|11.0|
|t4-r2-comparability-3.receipt.json|0.8|../.superpowers/sdd/tier1-v4/t4-r2-comparability.py|0|0.467|9.0|
|t4-r2-comparability.receipt.json|0.8|../.superpowers/sdd/tier1-v4/t4-r2-comparability.py|0|0.4674|14.0|
|t4-r2-final-coverage.receipt.json|0.8|../.superpowers/sdd/tier1-v4/t4-r2-final-coverage.py|0|0.5313|16.0|
|t4-r2-http-budget.receipt.json|0.3|../.superpowers/sdd/tier1-v4/t4-r2-http-budget.py|0|0.0349|1.0|
|t4-r2-report.receipt.json|0.4|../.superpowers/sdd/tier1-v4/t4-r2-report.py|None|0.0135|None|
|t4-r2-schema.receipt.json|0.3|../.superpowers/sdd/tier1-v4/t4-r2-schema.py|0|0.0395|1.0|
|t4-r2-sourcecheck-2.receipt.json|0.8|../.superpowers/sdd/tier1-v4/t4-r2-sourcecheck.py|0|0.1469|4.0|
|t4-r2-sourcecheck-3.receipt.json|0.8|../.superpowers/sdd/tier1-v4/t4-r2-sourcecheck.py|0|0.147|8.1|
|t4-r2-sourcecheck-final.receipt.json|0.8|../.superpowers/sdd/tier1-v4/t4-r2-sourcecheck.py|0|0.1453|4.0|
|t4-r2-sourcecheck.receipt.json|0.8|../.superpowers/sdd/tier1-v4/t4-r2-sourcecheck.py|1|0.0701|1.0|
|t4-r2-tail-provenance-final.receipt.json|0.6|../.superpowers/sdd/tier1-v4/t4-r2-tail-provenance.py|0|0.0493|1.0|
|t4-r2-tail-provenance.receipt.json|0.6|../.superpowers/sdd/tier1-v4/t4-r2-tail-provenance.py|0|0.0492|1.0|

Formula-specific statuses and missing fields (counts are independent source arithmetic cells, not a suite certification):

|formula|role|status|matches|mismatches|missing source fields|
|---|---|---|---|---|---|
|capei|primary|blocked_contract|0|0|ni_fy0, ni_fy1, ni_fy2, ni_fy3, ni_fy4, source_full_year_comparability|
|bm|primary|partially_checked|2|0|txditc, pstk, seq|
|evm|primary|blocked_source_fields|0|0|dlc, che, oiadp, dp, pstk, mib, dltt|
|pe_issuer_ib|extension|checked_sample|40|0||
|pe_issuer_ni|extension|checked_sample|40|0||
|pe_exi|primary|blocked_contract|0|0|security_class_diluted_eps_continuing, adr_ratio, eps_share_basis, split_available_at|
|pe_inc|primary|blocked_contract|0|0|security_class_diluted_eps_total, adr_ratio, eps_share_basis, split_available_at|
|ps|primary|partially_checked|37|0|sale|
|pcf|primary|checked_sample|40|0||
|dpr|primary|partially_checked|23|0|dvc|
|peg_trailing|primary|blocked_contract|0|0|security_class_diluted_eps_fy0, security_class_diluted_eps_fy3, source_full_year_comparability, split_available_at|
|divyield_paid|extension|partially_checked|23|0|dvc|
|ptb|primary|partially_checked|3|0|pstk, seq|
|divyield|primary|blocked_contract|0|0|indicated_annual_dividend_rate, dividend_announcement_available_at, security_class_currency_basis|
|npm|primary|partially_checked|37|0|sale|
|opmbd|primary|partially_checked|20|0|oiadp, dp, sale|
|opmad|primary|partially_checked|29|0|oiadp, sale|
|gpm|primary|partially_checked|26|0|cogs, sale|
|ptpm|primary|partially_checked|37|0|pi, sale|
|cfm|primary|partially_checked|23|0|dp, sale|
|roa|primary|partially_checked|21|0|oiadp, dp|
|roe|primary|partially_checked|17|0|txditc, txditc_p, seq, seq_p|
|roce|primary|blocked_source_fields|0|0|xint, dlc, dlc_p, pstk, pstk_p, seq, seq_p, txt, dltt, dltt_p|
|efftax|primary|partially_checked|36|0|txt, pi|
|aftret_eq|primary|partially_checked|3|0|pstk, pstk_p, seq, seq_p|
|aftret_invcapx|primary|partially_checked|1|0|xint, pstk, seq, mib, dltt|
|aftret_equity|primary|partially_checked|38|0|seq, seq_p|
|pretret_noa|primary|partially_checked|27|0|oiadp, ppent, ppent_p, act, lct, act_p, lct_p|
|pretret_earnat|primary|partially_checked|27|0|oiadp, ppent, ppent_p, act, act_p|
|gprof|primary|partially_checked|26|0|cogs, sale|
|equity_invcap|primary|partially_checked|3|0|pstk, seq, mib, dltt|
|debt_invcap|primary|partially_checked|3|0|pstk, seq, mib, dltt|
|totdebt_invcap|primary|blocked_source_fields|0|0|dlc, pstk, seq, mib, dltt|
|capital_ratio|primary|partially_checked|3|0|pstk, seq, dltt|
|int_debt|primary|numeric_mismatch|12|2|xint, dltt, dltt_p|
|int_totdebt|primary|numeric_mismatch|4|1|xint, dlc, dlc_p, dltt, dltt_p|
|cash_lt|primary|partially_checked|24|0|che, lt|
|invt_act|primary|partially_checked|25|0|invt, act|
|rect_act|primary|partially_checked|28|0|rect, act|
|debt_at|primary|partially_checked|30|0|lt|
|debt_ebitda|primary|partially_checked|3|0|dlc, oiadp, dp, dltt|
|short_debt|primary|numeric_mismatch|6|1|dlc, dltt|
|curr_debt|primary|partially_checked|24|0|lt, lct|
|lt_debt|primary|partially_checked|17|0|lt, dltt|
|profit_lct|primary|partially_checked|21|0|oiadp, dp, lct|
|ocf_lct|primary|partially_checked|34|0|lct|
|cash_debt|primary|numeric_mismatch|6|1|dlc, dltt|
|fcf_ocf|primary|partially_checked|38|0|capx|
|lt_ppent|primary|partially_checked|9|0|lt, intan|
|dltt_be|primary|partially_checked|2|0|txditc, pstk, seq, dltt|
|de_ratio|primary|partially_checked|30|0|lt, seq|
|debt_assets|primary|numeric_mismatch|6|1|dlc, dltt|
|debt_capital|primary|blocked_source_fields|0|0|ap, dlc, pstk, seq, dltt|
|intcov|primary|partially_checked|15|0|xint|
|intcov_ratio|primary|partially_checked|14|0|xint, txt|
|cash_ratio|primary|partially_checked|32|0|che, lct|
|quick_ratio|primary|partially_checked|25|0|invt, act, lct|
|curr_ratio|primary|partially_checked|34|0|act, lct|
|cash_conversion|primary|partially_checked|19|0|ap, rect, cogs, invt, sale|
|at_turn|primary|partially_checked|37|0|sale|
|inv_turn|primary|partially_checked|22|0|cogs, invt, invt_p|
|pay_turn|primary|partially_checked|21|0|ap, ap_p, cogs, invt, invt_p|
|rect_turn|primary|partially_checked|28|0|rect, rect_p, sale|
|sale_equity|primary|partially_checked|35|0|seq, sale|
|sale_invcap|primary|partially_checked|3|0|pstk, seq, mib, dltt, sale|
|sale_nwc|primary|partially_checked|33|0|sale, act, lct|
|accrual|primary|checked_sample|40|0||
|rd_sale|primary|partially_checked|21|0|xrd, sale|
|adv_sale|primary|partially_checked|7|0|xad, sale|
|piotroski_f|composite|blocked_source_fields|0|0|at_p2, sstk, cogs, cogs_p, ib_p, dltt, dltt_p, act, lct, act_p, lct_p, sale, sale_p|
|altman_z|composite|numeric_mismatch|20|1|oiadp, lt, act, lct, sale|
|ohlson_o|composite|numeric_mismatch|17|2|lt, act, lct|
|beneish_m|composite|numeric_mismatch|10|2|dp, dp_p, rect, rect_p, cogs, cogs_p, ppent, ppent_p, xsga, xsga_p, dltt, dltt_p, sale, sale_p, act, act_p, lct, lct_p|

Composite source-model certification remains blocked separately from numerical formula arithmetic. Piotroski needs source FY0/FY1/FY2 period certification, beginning assets and common equity issuance-event/share-count identity. Altman needs certified EBIT rather than assumed OIADP equivalence, retained earnings and population applicability. Ohlson needs the GNP price-level deflator, source funds-from-operations identity and comparable prior NI; nominal assets/OCF remain proxies. Beneish needs original total-accruals definition plus certified comparable GP/debt/SG&A/D&A histories; IB-minus-OCF is not assumed equivalent. Exact fields/owners are composite_source_model_certification in requirements JSON.

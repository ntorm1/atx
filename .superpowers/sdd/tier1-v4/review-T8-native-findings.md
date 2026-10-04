# Task8 initial independent native review — 4738a7b4

### Spec Compliance — Needs fixes

The native implementation retains the full 120 holding months (2013–2022) and original 153 characteristics, full JKP denominator and independently declared OSAP overlap. It uses Task7's economic tercile kernel and the R20 nonmicro reference population, preserves already direction-signed JKP returns, and converts native OSAP percent returns once with separately pinned mapping/weight metadata. Native label reconstruction and complete pre-2023 windows remain separate from R21 historical predictor construction. Frozen external binding identity, upstream generation admission, source/code identity checks, monthly native replay and public full-generation replay are present.

Missing: final-publication restart recovery (I1). No extra scope or economic convention change found in this package.

Cannot verify from the diff: real source-semantic qualification, full historical source/clock availability, actual 120-month economic outcomes, and the original statistical gates. Author's actual 0 PASS / 4 FAIL/unmeasured assessment remains unresolved. Synthetic tests and this engineering review do not certify those gates or authorize an actual run.

### Strengths

Independent native benchmark replay is stronger than trusting derived return rows or caller-supplied correlation tables. Missing or untestable JKP characteristics retain the 153 denominator; missing admitted OSAP members block its median gate. Statistics retain actual calendar positions for HAC lags and expose pair counts and undefined t-statistics. Public validation reconstructs all months and the report rather than accepting refreshed hashes alone. Published metadata explicitly retains NOT_PRODUCTION_READY and source_quality_certified=false.

### Issues

#### Important I1 — Final immutable publication cannot recover from interruption

`atx-db/src/atx_db/alpha_panel/char_replication.py:188`, `:212`, `:215`, `:217`.

There are unrecoverable boundaries after completing all 120 receipts. If the immutable report is written and execution stops before the manifest, retry replays the monthly evidence and then tries to create the existing report again, raising FileExistsError. If the manifest is written and execution stops before public validation or pointer publication, retry fails immediately with `immutable completed Task8 generation`. The validate command can verify this complete generation but does not finish either publication pointer. Interruption between the two pointer writes can likewise leave `current.json` and the required `validation/char_replication.json` disagreeing with no CLI recovery path.

This breaks the advertised bounded restart/publication deliverable even when all source identities and completed monthly receipts remain unchanged. It requires controller deletion/reconstruction or a different generation rather than safe continuation of authenticated retained evidence.

Fix: under the writer lock, authenticate and compare existing final artifacts against independently reconstructed expected contents; recover a sealed complete generation through full public validation; then complete a recoverable publication transaction for both pointers. Preserve immutable artifacts and reject differing contents. Add interruptions after report creation, manifest creation, and each pointer write, including restoration of an existing prior published generation.

Evidence: two newly targeted synthetic publication tests injected an exception immediately after each final immutable write. In both cases all 120 receipts survived and `current.json` remained absent. Restored `build` raised FileExistsError for the report boundary, and `immutable completed Task8 generation` for the manifest boundary. The latter generation still passed `validate_generation`, demonstrating valid retained evidence without publication recovery. Tests used the existing explicitly isolated synthetic publication fixture; no actual source or return inputs were read.

#### Minor M1 — Source prerequisite is not represented in the machine-readable gate inventory

`atx-db/src/atx_db/alpha_panel/char_replication.py:77`–`:82`, `:214`.

The report describes four unresolved gates, including source admission, but generated `gates` and manifest `gate_status` contain only the three statistical gates. The false source-quality flag and readiness field prevent a production claim, so this does not itself certify sources; nevertheless a gate-only reader cannot reconcile the declared 0/4 assessment or see the prerequisite's explicit FAIL/pending reason. Include the independently determined source prerequisite as a separate machine-readable gate, without allowing caller assertions or statistical success to certify it.

### Validation

Only the two new interruption repros were run; unchanged author suites were not rerun. Command, from `C:/atx/atx-db`: `OPENBLAS_NUM_THREADS=1`, `PYTHONPATH=C:/atx/atx-db/src`, `python -m pytest <temporary test_final_interruption.py> -q -n0 -o addopts='' -p no:cacheprovider`. Result: **2 passed in 12.80s**, asserting the defective restart behavior. DuckDB used 128MB and one thread through the synthetic fixture. No real-data/hash/HTTP jobs, producer edits, commits, or subagents; no live job remains.

### Assessment

Spec: Needs fixes. Quality: Needs fixes. The economic and independent replay structure is substantial, but finalization interruption prevents reliable operational publication. This is an engineering checkpoint verdict only; real source admission and all actual original gates remain open.

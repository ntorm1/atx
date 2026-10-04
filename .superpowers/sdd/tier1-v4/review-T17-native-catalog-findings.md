# Task17 native/catalog checkpoint review

Reviewed checkpoint: `9e05e42df748b10c97d2804ef16d895354f3716d`, isolated `review-T17-native-catalog.diff`. Prior approved R23 runtime `6bab983f` and documentation `d847a7ce` are unchanged and were not re-reviewed. Task17 whole-task repair counter remains **1/5**; frozen Task16 remains **5/5**.

**Spec verdict: NEEDS CHANGES / original Task17 incomplete.** The four fixed readers are substantial implementation, but required all-family catalog/native serving integration and actual acceptance remain incomplete. The author openly reports this; this review does not interpret diagnostic NULLs, extraction receipts, or fixture PASS as original-task completion.

**Quality verdict: NEEDS CHANGES.** One catalog verification bypass was reproduced with an authorized isolated synthetic fixture. Recovery has a separate static stale-state overwrite defect. The source replay also materializes complete populations in Python despite its bounded resource contract.

## Numbered findings

1. **Important — `atx-db/src/atx_db/alpha_panel/tier1_catalog.py:324`–337: catalog verification trusts a mutable companion proof and checks names rather than serving definitions.** The independently supplied spec pins source snapshot/code, but does not independently pin the catalog/proof pair. A replacement view can retain its name and macro while returning invented cells. Updating the adjacent proof's `catalog_identity` makes `verify_catalog` accept those bytes without validating SQL definitions, schemas, clocks, or source-equivalent results. Authorized repro: original as-of value `20.0`; replaced view value `999.0`; source snapshot unchanged; before and after both returned `PASS_INTEGRITY_ONLY`. Bind independently admitted output/proof identities and verify the expected source-bound view/macro definitions (including schema/PIT semantics); presence alone does not meet the advertised catalog validation contract.

2. **Important — `atx-db/src/atx_db/alpha_panel/tier1_native_readers.py:149`–152: recovery overwrites an unrelated current generation.** With retained intent A→B, no pointer transaction, and a valid independently activated current C, the branch calls `_publish_pointer(B)` merely because current differs from B. It never requires actual current to equal intent.previous A. The existing native helper records whatever current exists as previous, so recovery silently replaces C and rewrites rollback history. Validation of B and authentication of A do not authorize overwriting C. Require current/previous/alias state to match the precise interrupted transition and reject unrelated valid states. This is a static finding, not an additional fixture run.

3. **Important (spec delivery gap) — `.superpowers/sdd/tier1-v4/task-17-report.md:244`, `atx-db/src/atx_db/alpha_panel/tier1_catalog.py:20`–60: all-family native serving implementation remains incomplete.** The report correctly identifies missing fixed Task12 terminal and Task13 distribution public-validator bindings, original factor/risk multi-product base-view mappings, daily characteristics/factors and ownership/short public-reader integration, and golden text/TNIC field mapping. Declaring stubs or generic per-file views does not implement those native schema/view/macro contracts. These are implementation dependencies, distinct from unavailable actual bytes or source-economic proofs. Finish them within controller-authorized ownership or record explicit downstream owners/interfaces; do not close full Task17 on the four readers alone.

4. **Important — `atx-db/src/atx_db/alpha_panel/private_tier1_earnings.py:119`, `:182`, `:360`–363: public replay is not bounded by the consumer resource contract.** It fetches the complete filing result into Python, retains all reconstructed release rows, groups the entire release population, materializes another complete feature list, then creates a complete Arrow table before writing. File-backed DuckDB's128MB limit does not bound these Python/Arrow allocations. This prevents claiming bounded full-population native replay under the <=1GiB guard on arbitrary supported inventories. Stream issuer groups/batches through unchanged native kernels and bounded writers, retaining complete population checks. No actual population allocation or memory test was run; this finding concerns the explicit unbounded algorithm.

## Checked invariants and retained constraints

Read the requested brief, global constraints, owner coverage, R21 cutoff, R23 financial runtime, Task14 cap ruling, foreign ownership list, and final report continuation. Reviewed the isolated diff in component passes, including all native reader/catalog code, focused fixtures, docs/stubs and the completeness JSON. Named outside-diff inspection was limited to the existing pointer helper relevant to finding2. The completeness matrix contains264 records, original primitive/formula/history/sign/applicability/clock fields, six yearly three-population NULL coverage cells per record, `FAIL_PENDING` admission and false completeness/source certification. Its ownership chain explicitly requires expected accessions→retained originals→full parse→dated links→PIT aggregates→gold.

Accounting paths delegate original standardized/ratio kernels, replay all32 units and retained continuation payloads, bind all85 extraction dependencies and the qualified immutable final financial leaf, retain original gates/handchecks, and use separate independently pinned R23 financial256MB proof rather than promoting the128MB consumer connection. Native alias/pointer identity, complete manifest/product inventory and before/after source/code pins are checked. The new consumer contract requires file-backed128MB, one thread/UTC, guard and disk floor; actual resource safety is not established by fixture PASS (finding4).

Task9 paths reconstruct original parsing/classification/calendar/features/reaction logic, preserve immutable parse-phase lineage, native cache units and source/code pins, use independent standardized financial input for validation, and retain precision/recall/audit gates as pending when unmeasured. Public status remains diagnostic and `source_quality_certified=False`. No source-quality PASS was invented here.

The existing registry/catalog/verify entry points are wired to new declarations and optional strict native contracts; source/preflight byte inventory and quiescence checks exist. The serving verifier nonetheless has finding1 and required family mappings remain unfinished under finding3. The existing CLI legacy path is retained. No complete actual catalog was built or accepted by this review.

R12 remains0/2 segment gates; I4–I7,1934 revenue-bearing misses,1235 missing-revenue filings,35194 unclassified annual filings and483 nonprobability positive disclosures are not promoted. Original long-item collision checks remain36 groups/72 rows,21 unequal-value groups and six ambiguous tax_dtl choices, without arbitrary deduplication. R21 construction versus evaluation distinction remains: predictor history may exist beyond2022, but return/correlation/IC/portfolio evidence must use wholly pre2023 complete windows. Gold/consumer acceptance remains pending.

## Authorized synthetic evidence

Parent authorized only the narrow catalog view/proof tamper repro after the prior reviewer released the serialized lane. It used the author's isolated `test_tier1_catalog.world` fixture, a temporary synthetic source and file-backed DuckDB128MB/one thread/UTC. Only fixture declarations/financial admission guard were substituted; catalog build/verification, source snapshots, actual view mutation and proof handling were exercised. No actual sources, actual return rows, HTTP, cleanup, suite rerun, index or git operations occurred. Fixture is retained at `C:/Users/natha/AppData/Local/Temp/atx-t17-review-catalog-3rtmjf7r`.

Exact command: PowerShell here-string below piped to `atx-db/.venv/Scripts/python.exe -`, with `PYTHONPATH=C:/atx/atx-db/src` and `OPENBLAS_NUM_THREADS=1`.

```python
import importlib.util,tempfile
from pathlib import Path
import duckdb
from atx_db.alpha_panel import tier1_catalog as T,private_tier1_admission as A,common as C
location=Path('atx-db/tests/test_tier1_catalog.py').resolve()
loader=importlib.util.spec_from_file_location('review_fixture',location)
fixture=importlib.util.module_from_spec(loader);loader.loader.exec_module(fixture)
root=Path(tempfile.mkdtemp(prefix='atx-t17-review-catalog-'));source,stage=fixture.world(root)
T.declarations=lambda:[stage];A.guard=lambda con:None;T.T.admit=lambda name,spec,con:None
before,_,_=T.preflight(root)
contract={'snapshot_sha256':A.digest(before),'code_pins':T.code_pins(),'admissions':{name:{} for name in T.T.NAMES}}
output=root/'catalog.duckdb'
T.build(root,output,contract['snapshot_sha256'],contract['code_pins'],contract['admissions'])
print('BEFORE',T.verify_catalog(root,output,contract))
with duckdb.connect(str(output),config={'memory_limit':'128MB','threads':'1'}) as con:
 con.execute("SET TimeZone='UTC'")
 print('ORIGINAL_ROWS',con.execute("SELECT value FROM fixture_as_of(TIMESTAMP '2021-02-02')").fetchall())
 con.execute("CREATE OR REPLACE VIEW fixture AS SELECT 1::BIGINT security_id,999::DOUBLE AS value,TIMESTAMP '2021-01-01' available_at")
proof=output.with_suffix('.proof.json');record=C.read_json(proof)
record['catalog_identity']=A.pin(output);C.write_json_atomic(proof,record)
print('SOURCE_SNAPSHOT_UNCHANGED',T.preflight(root)[0]==before)
print('AFTER',T.verify_catalog(root,output,contract))
with duckdb.connect(str(output),read_only=True,config={'memory_limit':'128MB','threads':'1'}) as con:
 print('WRONG_ROWS_ACCEPTED',con.execute("SELECT value FROM fixture_as_of(TIMESTAMP '2021-02-02')").fetchall())
print('RETAINED_SYNTHETIC_FIXTURE',str(root))
```

Result: exit0,6.68seconds. BEFORE/AFTER each `{'status':'PASS_INTEGRITY_ONLY','source_quality_certified':False}`; ORIGINAL_ROWS `[(20.0,)]`; SOURCE_SNAPSHOT_UNCHANGED `True`; WRONG_ROWS_ACCEPTED `[(999.0,)]`. Initial setup attempt failed on reserved SQL alias before meaningful verification and was corrected; it is not a product failure. Test lane was immediately released to root/Task15 after the decisive run. Author's unchanged17-test42.03s PASS and8-module/4-JSON static PASS were acknowledged, not rerun.

## Precise remaining admission dependencies

The accounting and earnings source plans require reviewed independent execution/code/source snapshots, actual complete85-to-financial admission, source-qualified immutable final leaf and all native economic/manual gates. Complete13F bronze/cohorts, native daily/factor/ownership/short publications, Task12/13 action/cash/terminal clocks and validators, guidance actual seven gates/manual population, notes original context/revenue proof and whole-key repair, full text corpus/historical SIC source clocks and actual yearly three-universe coverage remain required. Extraction receipt admission is not financial/native-gate admission; stopped partitions are not completed cohorts; engineering fixture PASS is not actual source qualification. Original full SHA/size/quiescent catalog verification, clean-source parity refresh and final golden/consumer seals remain unperformed. No NULL-only outcome closes these requirements.

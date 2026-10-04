# Task17 initial R23 runtime integration checkpoint

Status: DONE_WITH_CONCERNS for the R23 engineering prerequisite only; full Task17
is INCOMPLETE. Initial checkpoint repair count0; current whole Task17 repair
count1/5 (see Fix round1 below), with no component counter reset.
Task16 remains at its exhausted5/5 cap. Independent scoped re-review is required
before any actual adapter run. No actual source/data/hash/metadata/HTTP/return job
was launched by this implementer, and no implementer subprocess remains alive.

## Built

New `alpha_panel/fund_release_runtime.py` and
`alpha_panel/privatefund_runtime_resources.py` provide bounded build/continuation,
full final merge/evidence/native manifests, unchanged native public validation,
immutable pointer publication, recovery and rollback. Native connection-taking
helpers and pointer helpers are reused without overrides. Only build and pointer
orchestration are repeated because the frozen native build hardcodes128MB and
native recover/rollback expose no hook for before-finish runtime provenance
rechecks. Full phase-order equivalence is fixture-tested against the actual
unchanged native build/recover/rollback functions.

An explicit file-backed256MB, one-thread UTC DuckDB connection uses unchanged
common.connect. Actual settings/database file are verified, recorded in immutable
runtime attempts and rejected if only the contract declares256MB while the actual
connection uses128MB. Windows named-job membership, enforced positive cap<=1GiB,
and40GiB disk floor are verified. Initial actual runtime command templates use
the0.8GiB guard and continuous40GiB disk floor. Missing guard or a fabricated job
name fails. Resource success grants no financial admission.

An immutable caller-pinned execution contract and actual two-module code inventory
are separate from the exact frozen native code/source/85-extract inventory.
Runtime contract/code and a generation companion proof are rechecked around work
and before publication/recovery/rollback pointer mutations. Companion and actual
resource attempts live outside exact native generation inventory. Caller-pinned
native snapshot equality is mandatory; embedded candidate assertions cannot
replace it. Mutable-directory contracts, missing/stale/mutated code/contract/
snapshot/proof refuse work.

Every prior and new unit remains independently replayed; all85 batches and144
preparation units, source/admission equality, original typed schema/population/
clock/counter checks, merge equality, native evidence and financial gates remain
unchanged. Failed native attempts and old generations are retained. Recovery
preserves stale previous-generation pointer identity without admitting that
generation; rollback requires complete native admission. No diagnostic output is
reused or converted to an admitted unit, no legacy import, no connection/global/
function cloning or override was added by this adapter.

Owned execution contract:1462bytes, physical SHA256
`45ea46d78385ad7969551cce8accd511f6c2d3c93e6b4b5bb70e4cdb20eaf49e`.
Runtime code inventory digest:
`e50412ff21622ca3710420baec9c6fc982ac8cb0ba7fe22347c2269b185a8594`.
Actual adapter15250bytes, SHA256
`1ded9097534e54253fded2e155798cba5049366be2fce9d89058476d6ec2e8e3`;
resource helper3080bytes, SHA256
`952b5830fccbd9c8a86759d090c81a74a0d512684a07a868b94759bb3f7f487d`.
Only owned code/config were hashed for these values. They are implementer
measurements pending independent admission, not authority for an actual run.

Parent supplied unchanged initial generation `r17_20261004_002` **physical
snapshot.json file SHA256**
`c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c`
and complete external extraction-admissionSHA
`168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f`.
These actual-source values were not read/rehashed by the implementer. All85
extractions are parent-reported independently admitted; no financial unit has
been independently admitted. Preserve failed128MB pilot/256MB diagnostic evidence.
The physical snapshot file SHA is not the canonical parsed-content digest
`fund_batch_release.digest(snapshot)` required by `--expected-snapshot-sha256`.
No actual canonical digest has been obtained or independently admitted here;
the initial operational handoff remains held pending root's guarded independent
identity admission and scoped re-review. See Fix round1 for the correction.

## Exact verification commands and results

All commands ran from `C:/atx` with
`$env:PYTHONPATH='C:/atx/atx-db/src'; $env:OPENBLAS_NUM_THREADS='1'`.
Every fixture is offline and every completed file ran in<60s. Orchestration
fixtures create no DuckDB connection; real connection-refusal uses128MB, only the
resource-positive fixture uses256MB. No production source or HTTP was consulted.

```powershell
atx-db/.venv/Scripts/python.exe .superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 10 -- atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_alpha_panel_fund_release_runtime.py -n 0 -o addopts='' -p no:cacheprovider -q
```

Initial15cases:15passed10.14s. Expanded17cases:17passed9.50s. Added
public-consumer/mutable-contract selector:2passed17deselected1.41s.
Final complete19cases: **19passed8.63s**, guard exit0/run11.0s,
peak78,458,880bytes,0cap events, receipt
`C:/Users/natha/AppData/Local/atx-memory-guard/receipts/20261004T114728-22860.json`.

```powershell
atx-db/.venv/Scripts/python.exe .superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 10 --disk-path C:/atx --min-free-disk-gb 40 -- atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_alpha_panel_fund_release_runtime_resource.py -n 0 -o addopts='' -p no:cacheprovider -q
```

First run had1collection error1.11s: pytest importlib mode did not expose the
existing native test module as a top-level import. Switched fixture-only loading
to importlib from its explicit file. Next run had1failed2.22s after successful
native ordered issuer join/exact replay: the assertion's unquoted SQL column
`at` was reserved. Quoted it. Then1passed2.65s/peak146,530,304bytes. After adding
the real128MB connection-refusal case, final **2passed3.84s**, guard exit0/run6.0s,
peak147,226,624bytes,0cap events, disk floor measured46.0849GiB at guard completion,
receipt `C:/Users/natha/AppData/Local/atx-memory-guard/receipts/20261004T114655-14236.json`.
These are synthetic measurements, not extrapolated full-data resource acceptance.

```powershell
atx-db/.venv/Scripts/python.exe -m py_compile atx-db/src/atx_db/alpha_panel/fund_release_runtime.py atx-db/src/atx_db/alpha_panel/privatefund_runtime_resources.py atx-db/tests/test_alpha_panel_fund_release_runtime.py atx-db/tests/test_alpha_panel_fund_release_runtime_resource.py
git diff --check -- atx-db/src/atx_db/alpha_panel/fund_release_runtime.py atx-db/src/atx_db/alpha_panel/privatefund_runtime_resources.py atx-db/tests/test_alpha_panel_fund_release_runtime.py atx-db/tests/test_alpha_panel_fund_release_runtime_resource.py atx-db/docs/datasets/fund_release_runtime.md atx-db/docs/datasets/fund_release_runtime.execution.json atx-db/docs/datasets/fund_release_runtime.registry.json .superpowers/sdd/tier1-v4/task-17-report.md
```

Both exit0. Native frozen producer/common/release-input/universe/Task5 files and
shared registries remain unedited. Graph discovery tools were unavailable;
read-only code/file discovery used rg and explicit source reads.

## Gates

| Criterion | PASS/FAIL | Measured | Command |
|---|---|---|---|
| Native build phase-order and all85 replay equivalence | PASS | native/adapted calls identical;85prior+85final readmissions | first pytest command |
| Bounds/continuation/interruption | PASS | bounded0/1new; retained failed start/no admission/no pointer | first pytest command |
| Independent contract/code/snapshot/proof identity and drift refusal | PASS | missing/stale/changed/internal identity cases refuse | first pytest command |
| Native consumer/recovery/rollback ordering and before-finish drift refusal | PASS | complete native public validator, native pointer order; no finish on drift | first pytest command |
| Actual resource configuration and real native typed replay | PASS |256MB/1thread/UTC/file-backed;1event+1catalog; exact native replay; actual128MB refused | second pytest command |
| Actual Windows guard/disk engineering check | PASS | verified<=1GiB job membership;147226624peak/.8cap;46.0849GiB;0cap events | second pytest command |
| Static verification | PASS | py_compile/diff check exit0 | commands above |
| Actual all85 financial units/full native gates | FAIL_PENDING |0independently admitted financial units;0new measurements of original9groups | no actual command; independent review prerequisite |
| Full Task17 product catalog/stagelake/lake verify/parity | FAIL_PENDING |0completed in this checkpoint | no actual command; original full Task17 obligations remain |

Engineering checkpoint:7PASS/2FAIL_PENDING. Actual financial acceptance remains
0PASS/9pending/unmeasured or inherited open groups; no economic gate is promoted.
Coverage2020–2025, date span and actual output rows/bytes remain unmeasured here.

## Files, sources, limits and self-review

Owned new files only: the two runtime modules, two direct test files,
`docs/datasets/fund_release_runtime.md`, `.execution.json`, `.registry.json`,
and this report. The registry is a runtime-integration stub for the existing
financial product; no shared registry was edited. Docs explicitly retain native
schema/keys/units/clocks/PIT/staleness, unmeasured yearly coverage and full Task17
obligations, plus the independent original128MB max-new-batches0 source-freeze
refresh path before a fresh generation obtains its expected snapshot pin.

Sources landed:0URLs/0bytes; no source history/span/cadence/terms/rate-limit
changes. No real stage outputs or acquisition disk use. Synthetic test disk use
is temporary; actual data volume was not inventoried. No return evaluation or
full forward-label windows were constructed.

Self-review: code/contract identity is non-circular and separate from native
receipts; runtime additions do not alter exact generation inventory; frozen
native protocols and economics are unchanged; no abbreviated validation or
existence-only admission; prior-unit replay is never skipped by work bounds;
runtime/disk/guard are checked before pointer mutation. Native writer lock and
pointer journal semantics remain intact. Admission remains expensive and bounded
new-work selection is not a wall-time bound. Full-source resource limits and
financial gates must be measured after independent review. The independent
reviewer should scrutinize repeated orchestration and provenance trust boundaries
before any actual adapter run. Full Task17 catalog/parity work remains REQUIRED.

## Fix round1: I1 snapshot hash-domain documentation; whole Task17 repair1/5

Independent initial review found Important I1: the root's supplied `c73f...`
identity is the physical pretty-JSON snapshot file SHA, while the unchanged
adapter requires the canonical parsed-content `fund_batch_release.digest`.
The previous documentation mislabeled the physical identity as the adapter
digest and would cause a safe refusal after freeze. Corrected both this report
and `docs/datasets/fund_release_runtime.md`; runtime code, CLI behavior, native
protocol, economic calculations, execution contract and code inventory remain
unchanged. No component counter resets; frozen Task16 remains5/5.

The operational option now explicitly names the canonical digest. Documentation
preserves the root's separately admitted physical snapshot and extraction SHA
and states that no actual canonical value has been obtained/admitted here.
Actual resume remains held for scoped re-review and root's independent guarded
identity admission. The root-prepared `t17-runtime-admission-v1.py` was read as
code only, not executed or edited. The documented proposed recipe first binds
the literal independently known physical snapshot SHA, parses unchanged bytes,
computes the canonical digest, verifies runtime and extraction identities, then
rechecks evidence in a second pass. Root must independently admit the guarded
receipt and both labeled identities before using the helper's canonical
`expected_snapshot_sha256`; output existence/assertions alone do not authorize
it. Refreshed generations need their own independently admitted physical identity
and reviewed canonical derivation, not the generation002 literal. Native source
freeze/unit replay/public validation and every financial gate still execute.

Docs-only correction: no code/interface behavior changed, no new pytest fixture
was needed and unchanged author suites were not rerun. Covering checks ran from
`C:/atx`, with the previously documented PYTHONPATH/OPENBLAS settings:

```powershell
atx-db/.venv/Scripts/python.exe -c "import hashlib,json; from pathlib import Path; from atx_db.alpha_panel import fund_batch_release as N,fund_release_runtime as A; sample={'binding':{'root':'offline-fixture'},'value':1}; physical=hashlib.sha256((json.dumps(sample,indent=2,sort_keys=True)+'\n').encode()).hexdigest(); canonical=N.digest(sample); assert physical!=canonical; doc=Path('atx-db/docs/datasets/fund_release_runtime.md').read_text(encoding='utf-8'); report=Path('.superpowers/sdd/tier1-v4/task-17-report.md').read_text(encoding='utf-8'); assert '--expected-snapshot-sha256 <independently-admitted-canonical-native-snapshot-digest>' in doc; assert '**physical snapshot.json file SHA256**' in doc; assert 'No canonical value\nis supplied or guessed here.' in doc; assert 'snapshot_physical_identity' in doc; assert 'independently\nadmitted' in doc; assert '**physical\nsnapshot.json file SHA256**' in report; assert 'No actual canonical digest has been obtained or independently admitted here' in report; assert N.digest(A.code_inventory())=='e50412ff21622ca3710420baec9c6fc982ac8cb0ba7fe22347c2269b185a8594'; assert N.identity(A.contract_path())=={'sha256':'45ea46d78385ad7969551cce8accd511f6c2d3c93e6b4b5bb70e4cdb20eaf49e','bytes':1462}; print('PASS: offline pretty-JSON physical SHA differs from canonical digest; documentation labels/held admission recipe and unchanged runtime/code contract pins verified')"
git diff --check -- atx-db/docs/datasets/fund_release_runtime.md .superpowers/sdd/tier1-v4/task-17-report.md
git diff --numstat -- atx-db/src/atx_db/alpha_panel/fund_release_runtime.py atx-db/src/atx_db/alpha_panel/privatefund_runtime_resources.py atx-db/docs/datasets/fund_release_runtime.execution.json
```

Results: Python exit0 with the quoted PASS output (offline illustrative
physical/canonical hashes differ); diff check exit0; runtime/helper/contract diff
numstat empty. Hash checks touched only owned code/config, not actual snapshot,
source, extraction-admission or product bytes. No actual job, source hash,
metadata job, cleanup, HTTP, financial or return run was launched. Root's live
universe job was left untouched. Only the two owned documentation/report paths
changed; all frozen/foreign paths and root's admission helper are unedited.

| Round criterion | PASS/FAIL | Measured | Command |
|---|---|---|---|
| Physical/canonical documentation and held admission recipe | PASS | offline hash domains differ; explicit labels and canonical option placeholder | Python check above |
| Runtime/native semantics and expected runtime identities unchanged | PASS | code digest e50412ff...8594; contract1462bytes/45ea46...49e; empty code/config diff | Python and numstat above |
| Actual canonical snapshot identity admission | FAIL_PENDING |0actual canonical values measured/admitted by implementer | prepared helper not executed; root slot/review prerequisite |

Full Task17 remains INCOMPLETE, original financial gates remain pending/open,
and scoped independent re-review must precede any actual adapter run.

## Original Task17 continuation: native reader and existing lake checkpoint

Status: DONE_WITH_CONCERNS for this engineering checkpoint; full Task17 INCOMPLETE. Whole-task repair count remains1/5; frozen Task16 cap remains5/5. Approved R23 runtime commits6bab983f and d847a7ce are unchanged. No native/common/Task5/Task9/runtime producer files or contracts were edited. No actual source, metadata, HTTP, financial, return or catalog job was run, and no source bytes were hashed by this implementer. Root owns all actual admission and recovery.

Implemented new private admission/accounting/earnings modules and tier1_native_readers. Fixed-name standardized/ratios publication orchestrates original connection-taking kernels, all32 buckets, prior/new-unit replay, typed schema/multiset comparison, complete source/code snapshots, retained failed attempts, original gates, final merge, immutable publication, activation/recovery/rollback and exact-byte legacy aliases. Financial sources are derived only from the independently pinned, completely native-validated finalfund leaf, including the full85 extraction inventory and separately pinned R23 256MB proof. Financial144-unit replay remains the frozen runtime's responsibility. Independent execution/code contracts for the new adapter are non-circular; actual connections require file backing,128MB maximum,1thread,UTC, original guard and40GiB disk floor. Daily ratio_cells uses unchanged native SQL over admitted immutable inputs. Task9 admission replays original retained filing bytes, native parser/cache provenance, full feature/calendar lineage and original audit equations; validated earnings additionally binds the qualified immutable standardized leaf and complete audit snapshot.

Implemented tier1_catalog and clean, Task17-owned changes to existing stagelake registry/catalog/verify entrypoints. All curated family stubs are registered. Existing `python -m atx_db.stagelake catalog --tier1-contract ...` creates the conditional catalog; existing `verify --tier1-contract ... --catalog ...` fully checks companion proof, actual database bytes, views/macros and before/after source/code inventories. Full four-reader admission runs before/after. Native Task15 original-byte reader is integrated explicitly; source-quality FAIL remains binding. Notes key collisions are checked without deduplication. Failed candidates are preserved and prior catalog survives changed inputs. Static JSON/JSONL evidence is exposed without inventing clocks. Catalog/proof are separate atomic replacements; interrupted proof mismatch fails verification and requires reviewed rebuild.

New documentation includes dataset docs and registry records, full product-family keys/clocks/method claims, the machine-readable original characteristic formula/primitive/history/source-gap matrix, and an honest pending note in PARITY_SCORECARD; historical measurements are unchanged.

### Covering checks and results

Exact final offline command (PowerShell, C:/atx):

```powershell
$env:PYTHONPATH='C:/atx/atx-db/src'; $env:OPENBLAS_NUM_THREADS='1'; atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_tier1_native_readers.py atx-db/tests/test_tier1_native_activation.py atx-db/tests/test_tier1_native_continuation.py atx-db/tests/test_tier1_catalog.py atx-db/tests/test_tier1_earnings_reader.py -n 0 -o addopts='' -p no:cacheprovider -q
```

Result:17passed42.03s, exit0. Fixtures are offline, one thread,128MB except the intentional256MB negative connection check; no actual source paths were opened. Real native85-file fact preparation, bucket derivation and cutoff checks match original native results (timing omitted). Real DuckDB catalog tests exercise strict cutoff/latest-vintage behavior, existing CLI routing, all-reader phase ordering, stale partition/catalog refusal, prior-catalog retention and original notes collision failure. Continuation tests use fixture kernels and actual Parquet/unit/manifest/pointer bytes, not actual financial evidence. Earnings audit tests compare original native equations and all18 population/year coverage cells. Resource/identity checks cover actual configured128MB file-backed connection, rejection of256MB and changed/missing execution/code pins.

Earlier covering iterations: reader/activation5passed10.10s; catalog/earnings7passed9.01s after correcting fixture mkdir and phase inventory; combined12passed16.20s; earnings3passed6.08s; activation/continuation3passed25.73s after fixing exact-byte alias publication (JSON newline changed physicalSHA); native readers3passed17.06s after fixing fixture facts location and UTC; catalog7passed15.45s. These failures did not alter native economic logic. Final17-test suite covers the final code.

| Criterion | Status | Measured evidence |
|---|---|---|
| Fixed accounting/earnings reader protocol and independent runtime identity | PASS offline |17 focused tests; full native fixture derivations and original audit equations |
| Existing stagelake catalog and lake verify integration | PASS offline | real database, existing CLI routes, exact serving proof and stale-input refusal |
| Full actual catalog/source qualification | FAIL_PENDING |0 actual catalog builds/admissions; no new coverage measurement |
| Full actual financial leaf/85extracts/144units admission | FAIL_PENDING |0 admitted financial units reported by root; frozen runtime unchanged |
| Original handcheck/science/economic/population gates | FAIL_PENDING | no actual measurements; standardized and ratio handchecks explicitly pending |
| Remaining complete native reader integration | FAIL_PENDING | gaps below, no NULL-only closure |

### Remaining implementation and source evidence

Task12 terminal and Task13 distribution catalog admission still needs fixed bindings to original public validators: native SHA-only file inventories and differing schemas are currently refused, not accepted by a shortened generic validator. Factor/risk immutable multi-product publication needs a reviewed mapping retaining original base serving views; generic per-file fallback is insufficient to claim complete integration. Daily characteristics/factors and ownership/short complete native public reader integration remain required. Task15's native original-byte reader exists and is wired into catalog admission; golden text/TNIC field mapping remains incomplete. Golden's existing char_monthly reader fully replays original source admission; that does not qualify unavailable daily/factor producers. Frozen producer/cap constraints remain binding; no producer edits or memory overrides were used.

Distinct unavailable actual evidence includes independent source contracts and complete native gate measurements, full13F authority bronze/cohort populations, primitive/feature histories, reviewed guidance gates, original notes context/period pins and whole-source key repair, complete corpus/SIC vintage proof, financial final leaf and complete native source coverage. Missing fields stay excluded with reasons; NULL does not establish completeness. Coverage2020-2025 for TRAIN_EXT/CORE/HOLDOUT remains unmeasured. No alternative TNIC method, unsupported estimate claim or predictive significance is asserted.

Owned paths: five new alpha_panel integration modules, five direct test files, reader execution JSON plus two dataset docs/two registry JSONs/completeness matrix, product catalog, PARITY_SCORECARD pending note, three shared stagelake modules and this report. Shared paths were clean and absent from foreign-dirty list before edits; all foreign work is preserved. Independent scoped review is required before any actual adapter/catalog run. Root remains sole actual-job authority.

Final static checks: `atx-db/.venv/Scripts/python.exe -c "import ast,json,pathlib; names=['private_tier1_admission','private_tier1_accounting','private_tier1_earnings','tier1_native_readers','tier1_catalog']; [ast.parse((pathlib.Path('atx-db/src/atx_db/alpha_panel')/(n+'.py')).read_text()) for n in names]; [ast.parse((pathlib.Path('atx-db/src/atx_db/stagelake')/(n+'.py')).read_text()) for n in ['catalog','registry','verify']]; [json.loads(pathlib.Path(p).read_text()) for p in ['atx-db/docs/datasets/tier1_native_readers.execution.json','atx-db/docs/datasets/tier1_native_readers.registry.json','atx-db/docs/datasets/tier1_catalog.registry.json','atx-db/docs/datasets/tier1_source_to_gold_completeness.json']]; print('PASS: eight Python modules and four owned JSON documents parse')"` exited0 with the quoted PASS. `git diff --check -- atx-db/src/atx_db/stagelake/catalog.py atx-db/src/atx_db/stagelake/registry.py atx-db/src/atx_db/stagelake/verify.py atx-db/docs/PARITY_SCORECARD.md .superpowers/sdd/tier1-v4/task-17-report.md` exited0 (only normal LF/CRLF notices). Neither check reads actual data.

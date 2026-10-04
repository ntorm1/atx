# Task17 initial R23 runtime integration checkpoint

Status: DONE_WITH_CONCERNS for the R23 engineering prerequisite only; full Task17
is INCOMPLETE. Whole Task17 initial dispatch, repair count0; no component counter
reset. Task16 remains at its exhausted5/5 cap. Independent review is required
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

Parent supplied unchanged initial generation `r17_20261004_002` snapshotSHA
`c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c`
and complete external extraction-admissionSHA
`168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f`.
These actual-source values were not read/rehashed by the implementer. All85
extractions are parent-reported independently admitted; no financial unit has
been independently admitted. Preserve failed128MB pilot/256MB diagnostic evidence.

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

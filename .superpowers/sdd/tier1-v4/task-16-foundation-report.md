# Task16 release foundation checkpoint (R15)

Status: DONE_WITH_CONCERNS for this implementation checkpoint only;
**NOT_PRODUCTION_READY**, no real build, no full Task16 completion.
Controller selected GPT-6.1 Sol implementer. No child agents were dispatched.
Owned new files only; native universe/checkpoint, source classifiers, legacy
outputs, foreign files, root progress and shared registries remain untouched.

## Built

`release_inputs.py` independently pins exact static native reader products,
actual SHA/size, producer manifests and members, explicit independently supplied
side-product provenance/clock/admission, binding JSON bytes, external French
and hand-verdict bytes, actual code bytes and complete configured inventory.
Missing, substituted, extra wildcard or empty inventories fail; path escapes
and changed actual bytes fail. Caller provenance never certifies economics.

`universe_release.py` reuses native classification/year SQL through private
cloned globals; two-context isolation is tested. New immutable
`universe_release_v1` generations use full2018-2026 inventory, exact typed
nonnull unique panel date/security axes, valid year/calendar dates, exact native
close clocks, full native output schema, bounded batch key hashing and sealed
unit receipts. Each admission/start/end/publication boundary rechecks the full
source/code inventory. Native generic cache is unused. Narrow or zero new-year
bounds do not shorten original acceptance; partial candidates cannot publish.
Interrupted unsealed units remain visible and refuse reuse. New generations
preserve all legacy evidence. A writer lock excludes competing release builds.

Complete generations retain original native gates/denominators, daily counters,
counts, 30-field coverage and freshly bound native stratified hand sample in
generation-local evidence. Manifest pins actual payloads, unit receipts,
snapshot and validation SHA/size. Unsupported generation outputs fail. Atomic
pointer last publication preserves the old pointer on interruption; rollback
revalidates the entire previous generation and current sources before swapping.
No implicit legacy fallback exists. Qualification is BYTE_IDENTITY_BOUND;
source_quality_certified is always false and NOT_PRODUCTION_READY is retained.

## Scope and controller checkpoint

Controller approved isolated native adapter and explicit versioned side-product
bindings after design checkpoint; independent code review and exclusive guarded
slot remain required before actual data IO. No real source scan/hash, HTTP,
warehouse job, return computation or producer rebuild occurred.

Interface sent to controller for Task6/7: consumers use explicit
`universe_release_v1/generations/<generation>/year=YYYY/universe.parquet`, pin
pointer/manifest/actual files, and run full validate_generation rather than
treating resolve_current's pointer check as qualification. Actual downstream
consumption stays held pending quiescent reviewed producer/candidate evidence.

## Fixture evidence

Exact command (cwd C:/atx):

```powershell
$env:PYTHONPATH='C:/atx/atx-db/src'; $env:OPENBLAS_NUM_THREADS='1'; atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_universe_release.py -q --tb=short
```

Final output: **15 passed in 23.56s**. Offline tiny temporary Arrow files;
every fixture connection is DuckDB 128MB, one thread. The orchestration fixture
keeps all nine original required years and compares every final payload byte
identity between bounded/interrupted-year completion and continuous build.
Its gate adapter is explicitly synthetic and retains French10/60 FAIL.
The rollback orchestration uses real full generation validation; the dedicated
atomic-pointer fixture mocks that validator solely to isolate interruption.

| Criterion | PASS/FAIL | Measurement | Command |
|---|---|---|---|
| Omitted required product must have explicit independent named binding | PASS | absent binding rejected; omitted parent member explicitly pinned, mutation rejected | pytest command above |
| Empty producer declaration, missing product, unsupported wildcard extra, path escape | PASS | covering fixture refusals | pytest command above |
| Typed/nonnull/unique/exact panel keys and schema/year/session/clocks | PASS | duplicate, missing/extra, null keys, wrong year/date, keyless, wrong type, wrong/null clock refused | pytest command above |
| Corrupt payload/daily receipt and stale source fail | PASS | covering fixture refusals | pytest command above |
| Full inventory, bounded zero/year continuation, equivalent completed output | PASS | nine original required years; identical final payload SHA/size | pytest command above |
| Private native adapter path isolation | PASS | two contexts; unchanged native globals | pytest command above |
| Publish-last interruption, authenticated rollback/pointer mismatch | PASS | old pointer preserved; full-generation rollback validated | pytest command above |
| Real whole-inventory/gate/coverage/hand/source qualification | FAIL | NOT_MEASURED; no real job grant; historical French10/60 FAIL retained | guarded operational commands in dataset doc, not executed |
| Full Task16/golden/predictive/consumer acceptance | FAIL | not implemented by this precursor | task-16 brief/addendum; no actual acceptance command executed |

Fixture checklist: 7 PASS / 2 FAIL rows; these are not dataset acceptance
gates. Real original dataset gates remain NOT_MEASURED in this checkpoint;
French historical FAIL is neither waived nor relabeled.

Self-review: final fixture rerun passes; `git diff --check --` owned paths
exits0. No broad suites or retstat checks were run.

## Limits and open dependencies

- Task2 R5-1/R5-2 legacy defects remain visible; no sixth fix or generic-cache
  certification is implied. Actual candidate source bytes are still unexamined.
- Original French/I4 and all original gate populations/thresholds remain binding.
  Completion of an operational command cannot make failed gates pass.
- Bounded new years do not bound whole-input/member rehashing or issuer-table
  loading. Full warehouse runtime/peak memory remains unmeasured.
- Reviewed caller binding JSON from quiescent producer evidence is still needed;
  some required optional-native products may be absent and then fail closed.
- Task5 legacy batch resume trusts CODE_VERSION/source size/mtime/FX identity
  and output existence without independent output SHA/code/preparation/key proof.
  This remains an explicit unqualified integration dependency; no Task5 modules
  changed and future batch-admission adapter is not implemented here.
- Task14 pinned nine-proof semantic admission, notes/segment exclusions,
  golden/labels/evaluation, source admission/export and consumer runtime remain
  future Task16 work, not claimed complete by this foundation.
- Atomic pointer replacement protects prior publication. A hard killed process
  can leave an orphaned writer lock/unsealed unit; inspect evidence and use a
  fresh generation, never blindly remove locks or delete legacy artifacts.

## Files / sources / disk

New owned files: release_inputs.py, universe_release.py,
tests/test_universe_release.py, docs/datasets/universe_release_v1.md and its
registry stub, this report. Warehouse rows/bytes/date span: none published.
New network source requests and source landing bytes: 0. Warehouse disk used:0.
Temporary offline fixture files only; no data are committed. Operational
commands and schema/clock/coverage/rollback contracts are in the dataset doc.

## Stopped at

Implementation checkpoint ready for independent named-risk review. No live
exec job remains. No actual source/data command is authorized by this report.

## Fix round 1/5 — independent initial findings

Review: `review-T16-foundation-findings.md`, Needs fixes. All three Important
findings and both Minors addressed in owned new integration files only.

- **I1:** Sealing/admission now recomputes daily all-common, NYSE, code-presence
  and missing-shrcd counts directly from the actual admitted payload. Every
  original diagnostic counter is regenerated with native load_issuer_tables /
  build_year / year_receipt from the authenticated reader snapshot and compared
  exactly. A self-consistent daily object/hash cannot certify wrong counters.
  Corrected fixture missing-code counters to zero for nonnull codes. Five
  separate wrong-counter fixtures include native-intermediate diagnostics.
- **I2:** Public validate_generation independently requires the exact current
  reader_contract(REQUIRED), reader snapshot version, full code-path inventory,
  binding/French/hand external inventory and exact external paths; versioned
  binding document content must match complete products. It no longer trusts
  abbreviated declarations supplied by a snapshot. Twelve public-validator
  negative fixtures cover abbreviated/versioned input contracts and unsupported
  qualification/status/gates before any warehouse IO.
- **I3:** Public admission enforces exact stage/schema/complete status,
  NOT_PRODUCTION_READY, BYTE_IDENTITY_BOUND, false source qualification, rule,
  original gate definitions and all required years. Validation evidence has an
  explicit schema and exact field inventory; independently derived measurement
  contract pins original populations/criteria, all coverage fields/universes,
  French60months, gate years and exact hand strata/seed. All native measurement
  evidence is regenerated and compared, and manifest gate statuses must match.
  Genuine measured FAIL remains permitted; fixture French10/60 stays FAIL.
  Snapshot, payloads, receipts, validation and manifest identities are rechecked
  after admission; receipt/output inventories are exact and reject extras.
- **M1:** Native coverage now receives a private common-root accessor. The
  two-context fixture actually exercises coverage history discovery using a
  capturing connection and confirms each private root's prior-year path; native
  shared globals remain unchanged.
- **M2:** Rollback uses the same stage writer lock as build. Pointer changes
  use a persisted transaction journal; consumers/builds hold on unfinished
  transactions. Explicit guarded `recover` validates the intended current target,
  checks retained previous manifest identity and completes both pointers before
  removing the journal. Tests cover interruption before and after current write,
  recovery, repeated reversible rollback and competing writer rejection.

Final covering command is the exact pytest command recorded above.
Final output: **32 passed in 40.47s**, offline, DuckDB128MB/one thread.
Named findings: 5 addressed; independent re-review still required.
No actual source scan/hash, data build, HTTP, return computation or producer edit
occurred. Root's separate two-pass source byte audit is not accepted here as key,
clock, economic or release qualification, and no new release binding was landed.

Cost/limits: independent unit validation now replays native year SQL and public
generation admission additionally replays all original measurements. Bounded
new-year selection still does not bound full input hashing or native replay.
Real warehouse peak/runtime and actual candidate/gate/source qualification remain
NOT_MEASURED; all prior Task2/French/Task5/fullTask16 dependencies remain open.
No live exec job remains at this coherent fix checkpoint.

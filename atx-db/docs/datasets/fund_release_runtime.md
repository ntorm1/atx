# R23 bounded fundamentals execution runtime

Task17 integration prerequisite only; independent review precedes actual execution.
The financial product remains `fundamentals_release_v1` with its exact physical
leaf `fundamentals_v11`, native schemas, keys, units, disclosure clocks, PIT rules,
staleness rules and financial gates from `fundamentals_release_v1.md`. No new
financial columns or dataset family is introduced. Readiness remains
`NOT_PRODUCTION_READY`; source quality and whole-dataset acceptance remain
unqualified. The full Task17 catalog, stagelake, lake verify, parity, 13F lineage
and profitability/growth coverage work remains required.

`atx_db.alpha_panel.fund_release_runtime` uses unchanged native connection-taking
helpers. It creates an explicitly file-backed DuckDB connection using unchanged
`common.connect(memory='256MB', threads=1, ...)`, checks actual settings
(`244.1 MiB`, one thread, UTC), and confirms an actual database file. The resource
helper queries the named Windows job, verifies current-process membership and an
enforced positive job-memory cap <=1GiB, and checks at least40GiB free disk before
and after operations and before publication. Initial actual runs must use the
external0.8GiB guard with its continuous40GiB disk floor. The guard queues normally;
never bypass it. Source quality and memory success are separate qualifications.

Callers independently pin both the physical execution-contract SHA256 and the
canonical digest of the actual two-module runtime code inventory. The contract
is `fund_release_runtime.execution.json`. The inventory maps each actual absolute
module path to physical SHA256 and size; its digest uses native `digest` (sorted
JSON keys, compact separators). Its two members are this adapter and
`privatefund_runtime_resources.py`. These pins are non-circular and separate from
the frozen native code/configuration inventory and extraction receipts. A missing,
stale or unsupported pin refuses work. The actual contract and code are checked
before/after work and before every pointer mutation. The execution contract must
be outside mutable release/extraction/preparation directories.

Each generation has an exclusively published immutable companion
`fundamentals_release_v1/runtime-provenance/<generation>.json`, containing the
caller-admitted runtime identities and independently expected native snapshot
digest. Actual connection settings, database path and enforced guard cap are
recorded in unique immutable attempt files under the companion's `attempts/`
directory. None of these files changes the native generation's exact inventory.
Consumers, continuation, rollback and recovery require the same independently
pinned runtime and matching companion. A companion alone cannot supply missing
caller authority. Missing or changed provenance fails before publication.

The complete frozen ordering is retained: native source freeze/admission;
independent snapshot equality; replay of every prior unit in the full85 loop;
new-only attempts with original before/after source checks, exact typed schema,
population, clock, native-counter and independent replay validation; complete
85-unit merge/equality; all native evidence and original gates; native leaf and
outer manifests; all85 units replayed again; complete native public-generation
validation; immutable journaled pointer publication. Bounded work limits new
units, never prior-unit validation or audit cost. All144 preparation units and
all85 extracts remain required. Default/forced extraction replay and externally
caller-pinned complete R19 admission follow the unchanged native protocol.
No legacy outputs, diagnostic outputs, alternate validator, memory global
override or altered financial semantics enter admission.

Recovery retains exact native locking, transaction schema, target/previous
manifest identity checks, full native target admission, both identity rechecks
and atomic finish. It preserves a stale previous generation as identity evidence
without qualifying it for consumption. Rollback fully admits the previous target
and refuses stale sources/code. The adapter repeats only the native build and
pointer orchestration needed to pass a connection and insert runtime rechecks;
the native algorithms, validators and pointer helpers are reused unchanged.
Hard termination retains the native writer lock for controller inspection and
failed attempts for forensics. No automatic removal of a retained lock occurs.

## Operational interface after independent review

Every runtime command requires:

```
--execution-contract C:/atx/atx-db/docs/datasets/fund_release_runtime.execution.json
--expected-contract-sha256 <independently-admitted-physical-contract-sha>
--expected-runtime-code-sha256 <independently-admitted-code-inventory-digest>
--expected-snapshot-sha256 <independently-admitted-canonical-native-snapshot-digest>
```

`--expected-snapshot-sha256` means `fund_batch_release.digest(parsed_snapshot)`:
SHA256 of sorted, compact JSON content. It does **not** mean the physical
SHA256 of `snapshot.json`, whose native writer uses pretty JSON serialization.
Preserve both identities with explicit labels. Never pass a physical snapshot
file SHA as this canonical adapter pin.

Build/continue uses `build --bindings <absolute-binding> --generation <id>
--max-new-batches 1`; consumer validation uses `validate --root <absolute-root>`.
Both accept `--extract-admission <absolute-external-admission>
--expected-admission-sha256 <independently-admitted-physical-sha>` and
`--force-full-replay`. `recover`/`rollback` accept `--root` and runtime/snapshot
pins; they preserve native unconditional full replay rather than using cached
extraction authority. Omit extraction options to preserve the default full replay.

Run serially using this guard prefix (commands are templates, not execution proof):

```powershell
$env:PYTHONPATH='C:/atx/atx-db/src'
$env:OPENBLAS_NUM_THREADS='1'
C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 10 --heavy --disk-path C:/atx --min-free-disk-gb 40 -- C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.fund_release_runtime build <reviewed-options>
```

The root-provided initial generation is `r17_20261004_002`; its independently
admitted **physical snapshot.json file SHA256** is
`c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c`.
This physical identity is not the canonical digest accepted by the adapter.
The actual canonical digest has not been obtained or independently admitted at
this documentation checkpoint. Actual resume remains held until that admission
and independent scoped re-review; the command above remains a template.
The root-provided complete external extraction-admission SHA is
`168b3ff61f4580066db74624bf58efb803993e8d2a0f1060daf4dfec1024ab1f`.
These values were provided to the implementer, not remeasured here. Preserve the
failed128MB pilot's native attempt and256MB diagnostic evidence; the latter
grants no financial unit admission and cannot be moved into a native attempt.

The root has prepared `.superpowers/sdd/tier1-v4/t17-runtime-admission-v1.py`
to bridge these hash domains without changing the native protocol. It has not
been executed by this implementer and is a controller evidence helper, not a
product or validator. After root grants its sole actual-data slot and admits the
reviewed helper, the proposed identity-admission command is:

```powershell
C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 10 --heavy --disk-path C:/atx --min-free-disk-gb 40 -- C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-v4/t17-runtime-admission-v1.py
```

The helper must first compare the actual physical snapshot identity against the
independently known `c73f...` literal, retain that full SHA/size identity, parse
the unchanged snapshot and compute `fund_batch_release.digest(snapshot)`. It
also checks independently expected extraction-admission/contract/code identities
and rechecks every evidence file plus runtime code/contract before recording
the result. Root must independently inspect and admit the successful guarded
receipt and resulting `t17-runtime-admission-v1.json`; output existence or its
own assertions alone cannot grant authority. Only then use its independently
admitted `expected_snapshot_sha256` canonical value for the adapter, while
retaining `snapshot_physical_identity` as separate evidence. No canonical value
is supplied or guessed here. These identity checks grant no current-source or
financial admission: the actual adapter still executes its complete native
freeze, snapshot equality, unit replay, validators and original gates.

For refreshed sources or a fresh generation, the independently reviewed original
native CLI remains useful for source freeze with zero financial computation:

```powershell
C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 10 --heavy --disk-path C:/atx --min-free-disk-gb 40 -- C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.fund_batch_release build --bindings <absolute-current-binding> --generation <new-id> --max-new-batches 0 --extract-admission <absolute-independent-admission> --expected-admission-sha256 <independently-admitted-sha>
```

Independently inspect and admit the complete resulting source freeze and physical
snapshot file identity. Bind that independently admitted physical identity before
deriving and separately admitting `fund_batch_release.digest(parsed_snapshot)`
with before/after evidence checks; supply only that canonical content digest as
the adapter's expected snapshot pin. The prepared helper above is specific to
generation002 and its existing physical literal; refreshed generations need
their own independently admitted identities and reviewed recipe. Do not infer
authority from an unreviewed candidate assertion. This zero-new native128MB freeze path
does not establish that the old128MB financial CLI is operationally accepted.

Measured coverage2020–2025, actual native financial rows, financial gates and
complete catalog/lake/parity acceptance remain unmeasured at this checkpoint.
Offline tests cover native phase equivalence, all85 prior-unit replay, bounds,
interruption, missing/stale runtime/snapshot identity, immutable proof, recovery
and rollback sequencing, drift before pointer finish, and a small real native
ordered issuer join with exact typed replay under the verified256MB connection.
They establish engineering behavior only, not full-data resource or financial PASS.

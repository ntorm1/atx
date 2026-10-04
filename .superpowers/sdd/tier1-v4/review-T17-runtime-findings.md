# Independent review: Task17 initial R23 runtime checkpoint

Reviewed commit `6bab983f` and `review-T17-runtime.diff` against Task17 brief,
R23 runtime ruling, global constraints, owner coverage addendum and implementer
report. Scope is the separately pinned engineering prerequisite, not full Task17.
Whole-task repair counter remains 0; Task16 remains exhausted at 5/5.

**Spec-compliance verdict: FAIL pending the Important integration correction below.**
**Code-quality verdict: PASS for runtime implementation; documentation/pin integration requires correction.**
Critical findings: 0. Important findings: 1. These verdicts do not establish actual-data financial,
resource, coverage, source-quality, economic or production acceptance.

## Important finding I1: documented actual snapshot pin uses a different hash domain

Locations: `atx-db/docs/datasets/fund_release_runtime.md:93-95`,
`.superpowers/sdd/tier1-v4/task-17-report.md:58-59`, and
`atx-db/src/atx_db/alpha_panel/fund_release_runtime.py:83,130-131`.

The documented initial-generation pin `c73f6293...` is the PHYSICAL snapshot file
SHA, independently confirmed by reading the existing diagnostic script
`.superpowers/sdd/tier1-v4/t16-fund-runtime-diagnostic-v1.py:14,21` (and its final
physical identity check). The new adapter requires `N.digest(snapshot)`, the
canonical compact sorted-JSON content digest. Native snapshot files use the
separate pretty JSON serialization from common.write_json_atomic. These hash
domains differ. The docs expressly present the physical pin as the independently
admitted digest required by `--expected-snapshot-sha256`.

Reproduction from the reviewed code: supply the documented initial-generation
`c73f6293...` as `--expected-snapshot-sha256` with otherwise admitted runtime and
binding identities. The adapter compares it against `N.digest(snapshot)` after
the expensive native freeze and raises `native snapshot differs from independent
caller pin` before financial work. Consumer companion checks have the same
mismatch. This is a safe refusal, but blocks the documented actual resume and
means the engineering handoff has no independently supplied pin in its required
hash domain. The fixtures consistently use `N.digest(snapshot)`, so they do not
exercise the real documented handoff.

Required correction: distinguish physical file SHA from canonical snapshot
content digest in the report and operational documentation; obtain and
independently admit the canonical digest from the unchanged physically pinned
snapshot before presenting an actual adapter command as usable. Preserve both
identities and all original native protocol/evidence. An offline regression
should ensure a physical pretty-JSON SHA is not mislabeled as the canonical
adapter pin. Changing the native protocol or deriving authority from an
unreviewed candidate is unnecessary. No actual source hashing or adapter run was
performed to reproduce this; the diagnostic's supplied provenance and direct
hash-domain comparison establish the defect.

## Review evidence

- `fund_release_runtime.py:38-58` requires explicit physical contract and actual
  two-module code pins and checks both before/after work. The execution contract
  is checked against the exact supported configuration, rather than accepted
  solely because its own embedded assertions agree. `:65-85`, `:117-139` and
  `:181-186` require the independent caller snapshot pin and a matching immutable
  generation companion outside the native generation inventory. Mutable native
  contract locations are refused. Existing companions cannot replace missing
  caller pins. Actual native snapshot/code/source/preparation equality remains
  enforced by the unchanged native freeze/recheck/validator.
- `privatefund_runtime_resources.py:33-60` verifies actual named Windows job
  membership and an enforced positive job memory cap at most 1GiB; a fabricated
  environment string is insufficient. `:64-74` checks the 40GiB floor and invokes
  unchanged common.connect with explicit 256MB, one thread and a unique database
  file. `fund_release_runtime.py:88-108` checks actual memory limit, thread count,
  UTC and a physical database file, and rechecks runtime, guard and disk at exit.
  `:181-186` repeats checks before pointer actions. The documented initial actual
  command retains the 0.8GiB guard and continuous disk floor.
- `fund_release_runtime.py:122-178` retains native freeze, all85 prior-unit
  admissions, bounded new-only compute, native source recheck, complete merge
  and equality, native evidence and leaf manifest, another source recheck and
  all85 readmissions, unchanged outer manifest, full native public validation
  and native publication. Compared directly with `fund_batch_release.py:727-772`.
  No existence-only output admission, abbreviated validator or diagnostic-output
  adoption was introduced. Every new unit still uses native compute_new and its
  independent typed replay; all144 preparation units remain required by freeze.
- `fund_release_runtime.py:198-233` uses the complete native public validator.
  Recovery preserves native transaction schema checks, current/previous target
  identities, full current-target validation, both identity rechecks and native
  finish. Rollback preserves complete target validation and native publication.
  Compared directly with `fund_batch_release.py:846-871`. Runtime/proof rechecks
  occur before pointer mutation. Recovery intentionally retains a stale previous
  pointer as identity evidence; rollback must separately admit its target.
  Recovery/rollback retain native unconditional extraction replay.
- The commit's complete eight-file path list contains only new owned runtime,
  fixtures, documentation/config/stub and report paths. The reviewed commit
  changes no frozen Task16/Task5/common code or shared registry. The adapter adds
  no monkeypatch, cloned function/module or global connection substitution;
  existing native internals are reused unchanged. Test-only monkeypatches are
  confined to offline orchestration fixtures.

## Independent checks and supplied tests

Ran an import-only owned-code/contract identity check with
`PYTHONPATH=C:/atx/atx-db/src` using `atx-db/.venv/Scripts/python.exe`; called
`N.identity(A.contract_path())`, `A.code_inventory()`, `N.digest(...)` and
`A.load_runtime(...)` with the supplied expected identities. Exit0; independently
matched contract1462bytes/SHA
`45ea46d78385ad7969551cce8accd511f6c2d3c93e6b4b5bb70e4cdb20eaf49e`,
adapter15250bytes/SHA
`1ded9097534e54253fded2e155798cba5049366be2fce9d89058476d6ec2e8e3`,
resource helper3080bytes/SHA
`952b5830fccbd9c8a86759d090c81a74a0d512684a07a868b94759bb3f7f487d`,
and inventory digest
`e50412ff21622ca3710420baec9c6fc982ac8cb0ba7fe22347c2269b185a8594`.
This check touched only owned implementation/configuration; no actual source or
data snapshot was read or rehashed.

Accepted supplied unchanged author test evidence without rerunning the suites:
19 offline orchestration cases passed in8.63s under0.6GiB, and two resource cases
passed in3.84s under0.8GiB (peak147226624bytes, zero cap events, reported disk
floor46.0849GiB). Inspected the fixtures: they compare calls against the actual
unchanged native orchestrators, test prior-unit replay/bounds/interruption and
stale runtime/proof refusal, and exercise real native typed replay with actual
256MB plus refusal of an actual128MB connection. Author py_compile and diff-check
results are supplied exit0. No additional synthetic regression was run; I1 is
established by direct static comparison with the diagnostic's pin provenance.
Graph discovery was unavailable as reported by root; used targeted source reads
and rg for native function locations.

## Limits and open obligations

Orchestration fixtures replace expensive native primitives, while the resource
fixture exercises only a small native batch. Neither is measured full85 financial
execution or full-source memory acceptance. The actual generation snapshot and
85 extraction admission identities remain root-provided evidence, not newly
verified here. No actual data/source/hash/metadata/HTTP/return job, builder,
cleanup or additional agent was launched by this reviewer.

All original financial/source/economic gates remain pending or inherited open;
zero financial units are newly admitted by this review. Catalog, stagelake,
lake-verify, parity, 2020-2025 universe coverage, 13F bronze-to-gold completeness,
profitability/growth matrix/features and all other full Task17 obligations remain
INCOMPLETE. Runtime engineering PASS cannot close those requirements. Preserve
the failed native128MB attempt and256MB diagnostic evidence. Any actual adapter
run requires root's independently admitted unchanged snapshot and runtime pins,
the guarded documented command and subsequent native financial admission.

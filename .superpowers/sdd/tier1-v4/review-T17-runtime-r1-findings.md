# Task17 R23 runtime scoped rereview: fix round1/5

Reviewed `d847a7ce` / `review-T17-runtime-r1.diff`, initial finding I1,
updated Task17 report and root's prepared `t17-runtime-admission-v1.py` as code
only. The commit changes exactly two documentation/report paths. Runtime code,
execution contract, native protocol and financial semantics are unchanged.

**I1: ADDRESSED.**
**Spec-compliance verdict: PASS for this scoped documentation correction.**
**Code-quality verdict: PASS for this scoped change.**
New changed-diff Critical findings: 0. Important findings: 0.

The runtime documentation at `fund_release_runtime.md:74-81` now defines
`--expected-snapshot-sha256` explicitly as the canonical sorted compact JSON
digest `fund_batch_release.digest(parsed_snapshot)`. At `:99-104` it correctly
labels `c73f6293...` as the previously admitted physical snapshot.json file SHA,
states the actual canonical digest has not been obtained or independently
admitted, and holds actual resume until independent admission and this rereview.
The updated report repeats the same distinction and pending admission, including
its explicit FAIL_PENDING row. No physical SHA is offered as a usable canonical
adapter argument.

The proposed recipe at `fund_release_runtime.md:111-135` is usable after root's
actual-data slot and evidence admission. Static inspection of the prepared helper
confirms it first binds the literal previously admitted physical snapshot SHA,
retains SHA/size, reads and checks the native snapshot shape, loads runtime with
the expected physical contract SHA and canonical code inventory digest, derives
the canonical snapshot digest, and rechecks all evidence file identities and
runtime code/contract before writing a new-only result. The output separately
labels `snapshot_physical_identity` and canonical `expected_snapshot_sha256`.
Documentation requires root to inspect and independently admit the guarded
receipt/result rather than infer authority from output existence or assertions.
Its refreshed-generation guidance requires separately admitted identities and a
reviewed derivation rather than reuse of generation002's physical literal.

The helper is a controller identity-evidence bridge, not a replacement native
validator. Its qualification and documentation retain complete native source
freeze, caller snapshot equality, unit replay, public validation and original
gates for actual execution. The pending actual canonical measurement is therefore
an explicit operational hold, not an unresolved documentation defect or implied
actual-data acceptance. This rereview does not itself admit an actual canonical
value or authorize skipping that hold.

Evidence: read the changed diff and helper source; `git show --format=
--name-only d847a7ce` lists only the two owned docs/report paths; the commit diff
for both runtime modules and execution contract is empty. Accepted the supplied
offline illustrative physical-versus-canonical check and documentation static
checks (exit0) and supplied diff check (exit0). Unchanged author pytest suites
were not rerun. No actual source/data/hash/metadata/HTTP/return job, builder,
cleanup or additional agent was launched; the prepared helper was not executed.
Only this rereview report was written, with no commit.

Whole Task17 repair count remains1/5, and frozen Task16 remains5/5. Full Task17
catalog/stagelake/lake-verify/parity, actual85 financial-unit admission, actual
resource acceptance, all original financial/source/economic gates and coverage
obligations remain pending/open exactly as before. No gate is promoted by this
documentation rereview.

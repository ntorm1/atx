# DL1: persist exact selected operands for derived metrics

User wants platform work while production backfill is memory-blocked. FQ1
identified a real prerequisite: an opaque hash of all candidate frame states
cannot prove the selected source operands/CIKs. Read derived-input-lineage-gap.md.
Deliver generic canonical-publisher provenance; do not weaken FQ1 eligibility.

## Ownership and dependency

Migration0323 is reassigned toDL1. The former uncommitted FQ1 body is now0324
and its own registry/__init__ imports removed. DL1 exclusively owns0323 body
and ONLY its registration/schema fixture lines until its task commit. FQ1 will
register0324 afterward. Neither new migration is live. No cli/activation/jobs
or FQ1 module edits. Own canonical derived DSL/publisher/annual helpers,
new derived_lineage.py,focused tests/docs and minimal required schema fixtures.
No API catalog/record version expansion in this task; internal SQL column and
resolver are enough for FQ1. Existing serving projections keep their contract.

Send root and /root/fundamental_signal_panel the exact proposed JSON and resolver
API early so consumer work can proceed. Fresh implementer, one fresh review,
focused tests only, root owns sole runtime. No Python/import/DB/tests/network
until root grants slot. No commit until validation/review coordinated.

## Producer contract

Add nullable selected_input_refs_json and selected_input_refs_hash to
derived_metric_values with catalog/schema metadata. Old rows remain NULL;
never backfill references from an opaque hash. Preserve numeric semantics and
existing candidate-input fingerprint meaning; do not relabel inputs_hash as
selected-only provenance. Document any deterministic ID/fingerprint change.

Each rebuilt event state persists a versioned, deterministic JSON payload of
direct selected refs, plus SHA256 of its exact serialized payload. Ref fields
identify kind(item/metric),code,frame bucket/offset,selected stable state ID,
availability, and for item the actual retained CIK,basis,source,fiscal span.
Metric refs identify dependency derived_value_id and its pinned input/definition
hashes. Explicit missing operands/status must survive NULL arithmetic. Literal
arguments do not invent a source leaf. Stable dedup/order; bounded count/bytes.

Emit provenance alongside numeric DSL lowering for ALL canonical supported
operators. Ref/arithmetic combine actual operands; lag/yoy/ttm/averages select
the correct offsets; coalesce follows the winning value branch; direct annual
fallback and weighted-share plans follow the exact existing branch/span
conditions. Do not persist the entire candidate frame as selected inputs.
Distinguish branch-selection diagnostics from selected value contributors if
needed. Unsupported provenance paths fail clearly instead of claiming proof.

Retain CIK in selected standardized structs and annual states. Carry refs/hash
through STATE_COLUMNS, frame, compression, staged DAG, and the same per-security
transaction as metric rows. Preserve same-value lineage transitions. Enforce
serialized-ref row/count caps before replacing prior security scope. Retain
bounded SQL event chunks; no full Python fact panels or transitive tree copied
into every metric. Existing historical rows become usable only after real rebuild.

## Bounded reader for FQ1

Provide a reusable resolver/qualifier accepting one or a bounded batch of root
derived IDs, expected dated CIK and decision cutoff/definition contracts. Return
explicit qualification status, selected leaf IDs/CIKs, input clocks/fiscal ends,
and a digest; do not infer ownership from security_id or ticker text.
Traverse direct refs with explicit depth/node/byte bounds and visited IDs.
Verify stored selected-ref hash, referenced row IDs and pinned input/definition
hashes, actual standardized CIK and clock/span fields, reconstructed history,
root/dependency/leaf visibility, and source/definition suitability. A dependency
cannot become visible after the consuming root event. Check all selected leaves
against the normalized expected CIK. Missing/cyclic/mismatched/legacy rows fail
closed with useful reasons. Reject no-source-leaf constants as issuer proof.
Avoid per-name/date unbounded roundtrips: support deterministic batching and/or
document a safe immutable-root cache contract usable by FQ1's bounded partitions.

Do not require derived issuer-owner ID to equal the trading security ID:
FQ1 binds actual selected CIKs to dated security identifier evidence and must
quarantine multiple eligible accounting owners rather than selecting arbitrarily.

## Focused proof

Temporary minimal fixtures only: direct item; nested metric; lag/TTM; annual
fallback with an unused differing-CIK annual candidate; coalesce chosen branch;
same-value source transition; NULL invalidation; actual cross-CIK selected leaf;
tampered hash/refs; missing/cyclic/legacy graph; decision/root-clock violations;
deterministic output across chunk sizes; oversized lineage retains old scope;
minimal0322->0323 upgrade/replay. Existing affected DSL/annual/PIT focused tests
must remain numerically correct. No fullsuite or live publication.
Write derived-selected-lineage-result.md with interface, checks, limits and
remaining measured-vintage limitations. Register only own migration lines.

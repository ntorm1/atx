# Shared accounting item vintages

This Parquet-only F.1 engine is the common accounting layer for 2.9. It consumes
explicitly pinned FSDS v2 and CF-R v2 manifests and never opens the warehouse or
forward-return labels. Owner-only construction can precede identity acceptance;
line projection and fundamental evaluation require the accepted 3.3 identity
artifact and the independent F.2 registration boundary.

Implementation and measurements are in progress. No accepted item snapshot,
fundamental coverage claim, or fundamental grade exists yet. Partial diagnostic
builds are unpublishable as accepted research inputs.

`research_fundamentals.py` exposes `prepare`, `normalize`, `normalize-all`, `plan`,
`build`, `audit`, `publish`, and `relocate`. Pass `--pins`, `--work`, and `--root` explicitly.
These last four commands also require `--plan`; `--bucket` selects a bounded
worker. Without a bucket those commands run separate guarded workers.

Every invocation runs under `run_memory_guarded.py`. Workers use 0.6 GiB,
DuckDB 256 MB and one thread, with `OPENBLAS_NUM_THREADS=1`. Orchestrators use
0.2 GiB and `--allow-nested-guards`. The default 128 CIK buckets are stable.
FSDS normalization can split one source quarter into eight disjoint accession
partitions (`--part N --parts 8`); full normalization uses that subdivision.
Partial files are unreadable until their completion receipt exists. A completed
slice is reused only after its provenance and every file hash have been checked.

Source and map provenance includes original seed bytes, mapping-unit discrepancies,
explicit controller authority, period policy, source manifests, normalized-source
completion receipts, and code hashes. Plan/code drift requires a new plan and build
ID. Prior plans and receipts remain retained evidence.

The C-111 internal closure includes existing active, statement-backed standard-GAAP
canonical items while the public `COMPUSTAT_ANALOG` mnemonic roster stays fixed.
Each added item retains its seeded rule and authority digest. Conflicting seed
versus statement units or period types are refused and disclosed; no additional
unit override is inferred. An added composition whose missing-input policy lacks
compatible historical evidence is explicitly refused rather than zero-filled.
Broad source mapping coverage counts unmapped cells as well as supported items.

FSDS uses `accepted_utc`. CF-only observations use the later of raw availability
and filing date plus 46 hours. Borrowing FSDS acceptance requires exact accession,
taxonomy, concept, unit, endpoint, duration class and numeric value, plus a unique
CF duration start. Candidate dispositions retain both terminal treatment and each
mapping outcome; their occurrence counts reconcile to the raw input population.
The `outer_identity_v1` mapping-outcome representation keeps nested item/reason
fields and stores candidate/CIK/year once in their enclosing disposition row.
Its version is pinned in plans, completion receipts and rows.
`decode_mapping_outcomes` reconstructs full decisions and reads retained legacy
JSON. This lossless storage change does not alter candidate or audit denominators.

Periods are proven from visible filing endpoints. Duration facts remain separate
quarter, YTD and FY series. Additive quarter differences and complete TTM sums
carry dependency edges and recompute on restatements or nullification. Exact FY
values are TTM only at their own endpoint. Weighted shares and EPS are nonadditive.
An explicitly null latest revision is authoritative. Equal-precedence economic
conflicts and competing fiscal-slot durations produce NULL rather than a
provenance-hash winner.

`audit` records source/disposition reconciliation, uniqueness, lineage closure,
dependency-clock ordering, validity intervals and direct numeric tie-outs. These
structural measurements do not replace the original 10,000-cell numeric/mapping
gate, accounting identities, 50-issuer fiscal audit, 50-owner by 12-formation raw
oracle, annual exhaustive PIT lineage, real kill/resume equality, accepted
identity projection, or the 3,000-owner monthly coverage target. The build index
remains `accepted=false` until those independently reviewed gates are satisfied.

`publish` seals immutable bucket/year snapshots through the existing research lake
exporter and verifies its explicit `ok` result. The final index references those
actual lake manifests and source candidate files. `ItemVintageStore(root,
build_manifest)` refuses unaccepted builds unless `allow_diagnostic=True` is
explicitly requested. Its `items_asof` API uses fiscal-quarter slot lags, XNYS
month-end decision cutoffs, dense owner rows and 200/400-day origin age policies.
The explicit-store `items_asof(..., store=store)` entry point is shared with 2.9.

`relocate` prepares a verified storage receipt and removes no files. It requires
an existing successful audit and sealed lake outputs, then proves complete
dataset/year value-multiset equality with bounded bidirectional comparisons.
The v2 receipt preserves the original plan file and canonical payload hashes,
complete/audit/published hashes, and separately pins the storage implementation.
Plan identity is checked before any receipt reuse. A historical computation keeps its original
code/map plan; relocation does not recompute values or change acceptance.
Bucket resume and audit readers use original pinned files when present and may
resolve absent originals only through this verified sealed destination chain.
A corrupt present original cannot silently fall through. The diagnostic
`bucket_materializations(..., prefer_sealed=True)` path measures destination
reads without removing originals.
Normal computational commands retain strict code pins. When code has changed,
`build|audit|publish --storage-only --bucket N` explicitly verifies an already
completed bucket and its v2 sealed storage proof. This route returns the original
receipt pins; it cannot compute items, rerun an audit, publish new content or
rewrite an index. Missing or failing original audits cannot use this route.

`prepare-release --bucket N` writes an immutable, reviewable manifest of exact
resolved intermediate leaf files and verified destinations, removing nothing.
After that scope is explicitly authorized, `release --bucket N` requires both
`--release-manifest` and `--release-manifest-sha256`. It checks source and sealed
replacement hashes before each unlink and durably records intent/progress for
crash-safe resume. No recursive removal exists. Plans, proofs, original JSON
receipts, source data and sealed outputs remain intact. A changed release scope
or code pin refuses execution; partial releases remain readable through v2 proof.

No interface in this module grants permission to register another wave, inspect
returns, use reconstructed identity as verified history, or open the sealed
holdout.

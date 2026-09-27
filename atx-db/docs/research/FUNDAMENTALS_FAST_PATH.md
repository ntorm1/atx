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
`build`, `audit`, and `publish`. Pass `--pins`, `--work`, and `--root` explicitly.
`build`, `audit`, and `publish` also require `--plan`; `--bucket` selects a bounded
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

No interface in this module grants permission to register another wave, inspect
returns, use reconstructed identity as verified history, or open the sealed
holdout.

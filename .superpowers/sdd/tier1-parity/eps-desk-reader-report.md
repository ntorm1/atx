# ED1 implementation report

Status: accepted after root-controlled focused checks, independent review,
and guarded live prerequisite measurement on 2026-09-24. No runtime, DB,
test, or network workload was launched by the implementer.

The generic CIK reader delegates all numeric selection to IQ2, requests the
quarterly `eps_diluted_q_growth_yoy` metric, keeps latest whole revisions,
includes unavailable states among latest observed periods, and retains IQ2
owner and exact selected-lineage rejection diagnostics. It converts the
stored growth ratio to percent for presentation only. It never reconstructs
Q4 EPS or derives a fiscal comparison by calendar subtraction.

Missing prerequisite columns, missing materialization, insufficient observed
periods, ambiguous source ownership, lineage gaps, truncated results, and
selected NULL states produce explicit non-success statuses. Only a complete
requested count of unambiguous numeric observations exits 0; this is a read
outcome and does not certify warehouse/release readiness. Output creation is
atomic, exclusive, and bounded to 2MB.

The read-only transaction uses 256MB/one thread/2GB spill. Before IQ2 fetches,
SQL aggregates cap owner/collision pairs at 64 rows/64KiB, the registered
definition catalog at 4096 rows/2MB, and visible EPS revisions at 2048 rows/2MB.
IQ2 retains its own bounded lineage proof and scan caps. The period-range
limit is 3660 days; latest selection accepts 1-12 observed periods.

The tiny tests exercise actual IQ2 qualification, NULL invalidation PIT,
foreign selected CIK exclusion, zero-base NULL and negative-base canonical growth preservation, missing
schema versus missing materialization, and byte refusal before service fetch.

Production limitations remain: schema 323/materialization are prerequisites;
no numeric CVX production output is asserted here. The reader selects latest
observed states rather than inventing absent fiscal quarters. CIK selection
does not resolve a ticker; `issuer_lookup_as_of` is null, and accounting cutoff
is independent of any caller's separately verified ticker lookup. Reconstructed
event clocks do not become verified historical-vintage data.

## Measured validation

The parent's single independent review found no Critical or Important
findings (`eps-desk-reader-review.md`). Root serialized all runtime work:

- `eps-desk-tests1.log`: six cases passed; process-tree peak 0.600845GiB.
- After correcting the fixture/documented negative-base expectation to the
  canonical absolute-denominator rule, only the two changed parameter cases
  were rerun. `eps-desk-base-tests2.log`: both passed; peak 0.598804GiB.
- `eps-desk-ruff1.log`: scoped Ruff checks passed.
- `cvx-eps-desk1.json`: live CIK 0000093410 at accounting cutoff
  2026-09-20T22:00:00Z, requested period-end range [2025-10-01,2026-07-01),
  latest three, returned `schema_prerequisite_missing` and exit 2. Missing
  columns are exactly `derived_metric_values.selected_input_refs_hash` and
  `derived_metric_values.selected_input_refs_json`.

The live read used 256MB/one thread under the lower 1GiB process guard and
peaked at 0.642921GiB (`cvx-eps-desk1-memory.json`). Stderr was empty. Root
confirmed unchanged warehouse size and modification time. The reader stopped
at schema preflight; it did not query source facts, qualify live metric rows,
or establish new EPS numerical/source coverage evidence.

This is a usable consumer and measured production-prerequisite refusal,
not a passed numeric CVX acceptance case. Migration 323, source completion,
fundamentals/derived materialization, and the full production/release gates
remain open. Root retains the runtime receipts and review separately from
this five-file task commit.

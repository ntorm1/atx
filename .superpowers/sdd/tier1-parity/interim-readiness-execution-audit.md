# Interim Tier-1 readiness execution audit (static, 2026-09-23)

Scope: `atx-db/scripts/measure_tier1_readiness.py`,
`atx-db/docs/FUNDAMENTAL_SIGNAL_READINESS.md`, the current program ledger and
production runbook. Codebase graph tools were unavailable; discovery used `rg`
and source reads. This audit did not run Python, connect to DuckDB, change the
warehouse, make a network request, or test capacity. The existing warehouse
file is `C:/atx/atx-db/data/warehouse.duckdb` (12,883,341,312 bytes at the
filesystem inspection); the current runbook says applied schema 0322. These
are separate observations, not a fresh schema query.

## Recommendation and exact invocation

The script can produce useful *stored evidence* while source loading and the
long schema/numeric checks wait for sustained 6 GiB free physical and 8 GiB
free commit headroom. Serialize it with all other heavy work, first verify no
writer, and use the existing 2 GiB process-tree guard. Keep 1 GB DuckDB memory,
one DuckDB thread, 2 GB spill, the fixed 2026-09-20 snapshot and 22:00 UTC
cutoff. The guard itself requires at least 4 GiB free physical and commit at
preflight and stops its child below 1.5/3 GiB respectively. A prior production
measurement prepass completed at 1.530 GiB process-tree peak with 1 GB DuckDB;
that supports one guarded trial under the present 2 GiB cap, although today's
larger raw table may raise the peak. The sustained 6/8 GiB wait applies to the
long source and schema/numeric batches; this read-only measurement may start
when the guard's own preflight passes. Use fresh receipt and report filenames
for each attempt. From
`C:/atx/atx-db`:

```powershell
& C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py `
  --job-gb 2 `
  --receipt C:/atx/.superpowers/sdd/tier1-parity/tier1-interim-sep24-1-memory.json `
  --stdout C:/atx/.superpowers/sdd/tier1-parity/tier1-interim-sep24-1.log `
  --stderr C:/atx/.superpowers/sdd/tier1-parity/tier1-interim-sep24-1.err `
  -- C:/atx/atx-db/.venv/Scripts/python.exe scripts/measure_tier1_readiness.py `
  --db-path data/warehouse.duckdb --as-of-date 2026-09-20 `
  --output-json C:/atx/.superpowers/sdd/tier1-parity/tier1-interim-sep24-1.json `
  --output-markdown C:/atx/.superpowers/sdd/tier1-parity/tier1-interim-sep24-1.md `
  --memory-limit 1GB --include-cf1 --include-fundamental-signals
```

The output names above are proposals: check they do not already exist. The
script refuses overwrite. It uses `read_only=True`, disables external access,
opens one transaction for a consistent read snapshot, and writes its two
artifacts only after successful queries. A partial artifact-writing failure
can leave the first new file present; use new names on retry. A concurrent
writer may prevent the read-only connection and would break the single-heavy-
workload rule. A query/memory/spill failure exits nonzero, not as missing
evidence.

## What the result means

The script only reads stored activation attempts, table aggregates, recorded
annual item coverage, provider SLO snapshots, data-quality-check records,
historical-evidence aggregates, CF1 run/evaluation inventories, and FQ1/FQ2
manifests/coverage/summaries. It does **not** execute quality checks, run
activation, run the FQ1 validator or FQ2 label/digest validation, migrate,
recompute provider coverage, or certify a release. `certification` is always
`unmeasured`. The optional FQ2 candidate flag is explicitly a stored research
flag; it cannot support an alpha or production eligibility claim. The latest
complete FQ2 summaries are suppressed unless the linked FQ1 build is complete,
snapshot eligible, and its **recorded** panel hash equals the evaluation's
recorded build hash. Hash equality is not a read-side digest proof.

With the current incomplete CompanyFacts source and no full downstream
fundamentals rebuild, expect many empty or stale sections. Such gaps are useful
as a dated baseline, but successful stage rows, static metric breadth, 47.9M
raw facts, and 31.9M bars do not satisfy the unchanged source floors, annual
110-item/90%-in-every-completed-year/top-3000 gate, quality gates, historical
US-common eligibility, or first-release conditions. The script labels many of
these limitations, but an operator must preserve the distinction in any
readout.

## Resource and confidentiality review

The 1,000-row outer limit in `Measurement.rows()` bounds transfer to Python;
it does not bound the input scan, hash table, sort, or window inside DuckDB.
`surface()` scans every stored row, including revisions, for each of 12
surfaces. The hottest known tables are about 47.9M `sec_company_facts` rows
and 31.9M `equity_daily_bars` rows. Each surface has `COUNT(DISTINCT
security_id)`, possibly additional distinct dimensions/run IDs, and multiple
filtered aggregates. `annual_coverage()` joins and groups stored item coverage
and cohort years; `provider_coverage()` ranks all snapshots; `quality()` ranks
all check records; `historical_gaps()` ranks terminal returns and joins events;
CF1 and FQ1/FQ2 inventories sort/group their manifest and coverage tables.
The optional FQ1/FQ2 code does not read the large score/proof/label panels.
At 1 GB/one thread, the big `COUNT DISTINCT` surface queries are the main
potential memory or spill failure. There is no per-query timeout or section
selector. The 2 GB spill limit and 2 GiB process guard bound damage, not
completion. Avoid treating a failed full report as a zero count.

`safe()` redacts email patterns in strings and sensitive keys in parsed JSON
objects. It does not sanitize arbitrary secrets in plaintext or truncated JSON.
The report transfers excerpts (up to 2,048 characters) of activation `error`,
provider `failed_slos_json`, and quality `details_json`; malformed or oversized
JSON stays plaintext. Old dataset `params_json` may contain private contact
data: this script extracts only `$.as_of_date` or `$.run_id` from it and does
not dump the raw field. FQ1/FQ2 blocker text and manifest JSON are excluded;
only bounded blocker counts are read. Do not publish raw JSON/Markdown artifacts
outside the private workspace without inspecting these three diagnostic fields.
The Markdown renderer also embeds data in tables/code fences without sanitizing
arbitrary Markdown fence text.

## If the full report exceeds the cap

Do not raise DuckDB or process memory, relax gates, or retry it alongside other
work. The current CLI has no safe subset switch for the mandatory surfaces: the
two `--include-*` flags only *add* CF1 and FQ1/FQ2 work. Omitting those flags
would save only small optional queries and lose the requested signal evidence;
it would not avoid the 47.9M/31.9M surface scans. A genuinely cheap interim
subset would need a separately reviewed, read-only manifest/ledger query that
selects only activation, CF1, FQ1 and FQ2 run records with bounded results,
without touching the large value panels. Its outputs would be explicitly
`stored run inventory, certification unmeasured`, not a substitute for the
full readiness measurement or any source/quality gate. No such query was run
or implemented in this audit.

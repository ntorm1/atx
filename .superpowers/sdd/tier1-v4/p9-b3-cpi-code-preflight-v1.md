# P9 B3 CPI source preflight: code and documentation only

The configured existing macro path does not by itself satisfy the requested
CUUR0000SA0 NSA series with original release clocks. This is a scoped code
finding, not a warehouse inventory or proof that no suitable source exists.
No database, macro value payload, HTTP endpoint or return evaluation was opened.

In atx-db/src/atx_db/macro.py, DEFAULT_SERIES includes CPIAUCSL; its local
metadata explicitly labels it seasonally adjusted. The loader accepts arbitrary
series IDs, so absence from that default tuple cannot establish absence from a
live warehouse. Exact searches of atx-db/src and atx-db/docs found no configured
CUUR0000SA0/CPIAUCNS reader; these searches do not cover all retained raw data.

More importantly, _normalize_series derives available_at directly from
observation_date plus23hours. It does not authenticate a publication instant.
The loader's own source metadata states that its feed is latest-revision FRED
and points to ALFRED for strict revision PIT. Its replacement operation deletes
prior rows for the selected series instead of preserving release vintages.
Those mechanics cannot be promoted to the requested C1-C6 contract by renaming
the series, adding a year column or using observation dates as release dates.

organization_capital.py references this CPIAUCSL path. Its historical presence
is not evidence that P9 B3's specific NSA/release-clock requirement is met; no
organization-capital calculation or signal trial was run or changed.

Next lower-priority source step, after the index work: a root guarded read-only
metadata inventory of configured warehouse/lake macro series, original source
manifests, historical files and clock/vintage evidence. The default connection
resolves data/warehouse.duckdb unless ATX_DB_PATH changes it; resolve actual
configuration before opening a store, never initialize or migrate it for this
inventory. Check any existing NSA series and original BLS release provenance
before deciding that new acquisition is necessary. If new data is required,
implement explicit original publication clocks and immutable vintages; no
current FRED observation-date timestamp may be reused as that proof.

P9-B05 remains NOT_MEASURED_IMPLEMENTATION_REQUIRED and conditional on the
actual inventory. This note neither grants source rights nor changes resource,
return-evaluation or source qualification rules. Existing macro code is read-only.

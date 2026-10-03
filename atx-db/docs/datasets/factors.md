# factors engineering checkpoint

This is an incomplete offline engineering checkpoint, not a certified factor dataset.
No real sources were read, acquired or evaluated. Production adapters are pending.
The configuration in `alpha_panel/factors.py:CONFIG` is versioned before comparisons;
changes need a new rule/version and documented justification before data comparisons.

Prepared input interface: UTF-8 JSONL, sorted `(freq, session_date, cik)` with one
consolidated issuer per period. Each row requires session_date, freq D/M, cik,
formation_date, cutoff (aware UTC), source_available_at (aware UTC), me_lag (USD),
ret (decimal total return), RF (decimal period risk-free return, spelled `rf`),
nyse, signals, membership_basis=`dated_history`, cap_basis=`exact_source_certified`,
config_sha256 and input_manifests_sha256. No source truth is inferred from names.
The maximum bounded cross-section is 20,000 issuers. Unavailable signals stay null.

Explicit release bindings require exact final manifests/output hashes for
fundamentals_v11, compustat_std_v11, ratios_v11, universe and prices_history.
Old stages/defaults are forbidden. Physical compustat vintage keys include
available_at; upstream adapters must select latest strictly visible issuer/period
snapshots. Certified issuer caps require independently reconciled share class,
units, issuer aggregation and availability. CIK 77159 remains unqualified pending
the separate source-basis audit. A prepared input's assertion is not that audit.

All return products are rejected outside 2013-2022 before portfolio computation.
Formation precedes the return date and uses strictly earlier source clocks.
Daily output carries session_date 22:00 UTC available_at; monthly inputs must use
monthly stock returns from frozen portfolios, never compound daily long-short spreads.
SMB is the average of independent value/profitability/investment small-minus-big
2x3 spreads. Required legs missing => null. q ME/IA/ROE uses independent 2x3x3;
q EG remains null pending its published model/source adapter. BAB uses ranked
beta distance weights and beta-scaled excess leg returns. These implementations
still require independent formula review, especially q formation conventions.

Outputs currently use immutable generation JSONL plus a publish-last manifest
pointer, not the required production Parquet stage. Interrupted generations do
not replace a previous pointer; source snapshots are rechecked before publication.
Concurrent publisher locking and unit-level resume are pending. Source mutation
while producers write is prohibited by the controller's exclusive slot (R9).

Return columns MktRF/SMB/HML/RMW/CMA/UMD/STREV/LTREV/q_ME/q_IA/q_ROE/q_EG/BAB
are nullable DOUBLE, decimal period returns. n_universe is an issuer count;
config_sha256 is the formation rule identity. Keys `(freq, session_date)`.
Benchmark validator requires every one of 120 calendar months 2013-2022, reports
FAIL on any missing month, and does not shrink the correlation window.

Coverage/gates: unmeasured; no publication accepted. Remaining native adapters:
dated price-history universe/issuer shares; prices/delisting total return alignment;
June annual accounting and December ME; monthly return lookbacks; RF/benchmark
column normalization; q quarterly ROE/investment and EG estimation; daily carry
of fixed formations. Formula sources still need complete primary-source review.

CLI (guarded real use is not yet authorized): `python -m atx_db.alpha_panel.factors
--prepared PATH --release-bindings PATH --destination PATH`.

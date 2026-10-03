# risk_inputs engineering checkpoint

Incomplete offline checkpoint. Native adapters and production Parquet publication
are pending; no real-data coverage or acceptance has been measured.

Prepared UTF-8 JSONL is sorted uniquely `(session_date, security_id)`. Each row has
UTC cutoff, exact release/config hashes, cap_quality, dated primitive objects
`{value, available_at}`, and at most 252 sorted, unique line observations with
session_date, ret, RF and MktRF/SMB/HML/RMW/CMA. Missing factor history remains
missing. OLS includes intercept, requires 200 complete rows and full column rank;
idio vol is the daily residual standard error (decimal return), not annualized.
Predictions use no observation after their session. available_at is session 22 UTC.
Post-2022 risk regression is explicitly missing pending a controller ruling on
latest risk estimation under GC2; no prohibited return statistic is computed.

Nullable fields: beta_252 DOUBLE; ff5_betas five DOUBLE; ivol_ff5_252 DOUBLE;
ff49 integer; size/value/momentum/volatility/liquidity/growth/leverage/quality
style z-scores DOUBLE; adv_21/adv_63 USD; spread_cs_21 decimal spread;
borrow_proxy (explicit source-defined value). Primitive clocks are strictly before
the prior-session cutoff. Sample z-scores preserve nulls. Uncertified cap basis
nulls styles. No borrow fees/utilization or FF49 is invented from missing sources.

Generation JSONL/manifests mirror factors snapshot checks. Coverage uses complete
rows/input rows and cannot certify a universe denominator until native universe
adapter validation is implemented. Required acceptance denominator remains each
universe cell per year 2020-2025, including missing rows/fields. Gates unmeasured.

CLI: `python -m atx_db.alpha_panel.risk_inputs --prepared PATH
--release-bindings PATH --destination PATH`. Real runs require controller slot.

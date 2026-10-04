# Native monthly characteristics

Task7 publishes `char_monthly/generations/<explicit-generation>/month=YYYY-MM/char_monthly.parquet`.
`current.json` is an atomic manifest pin. Consumers must call `validate_generation`,
not merely resolve the pointer. Qualification is `NOT_PRODUCTION_READY` until
the independent source and real acceptance work is completed.

The executable private registry is `char_monthly_registry.py`. The committed
`char_monthly.formulas.json` and `char_monthly.config.json` freeze definitions,
natural stored signs, economic signs, source dependencies, units, clocks,
staleness, history, missingness, sector applicability and original denominators
before any real return comparison. There are 153 original JKP names, of which
152 have executable kernels. Equity duration remains an explicit unsupported
column because the source parameter contract is unresolved. Additional JKP and
owner columns do not inflate that count. The synthetic complete-input fixture
actually computes 152 original columns, including the six cross-sectional
composites; this is engineering evidence, not real input availability evidence.

Definitions are based on [JKP documentation](https://jkpfactors.s3.amazonaws.com/documents/Documentation.pdf)
and its [public canonical factor catalog](https://raw.githubusercontent.com/bkelly-lab/jkp-data/main/src/jkp/data/config.py).
This implementation uses issuer ME consistently, actual SEC publication clocks,
and strictly certified components. Optional JKP missing-as-zero fallbacks are
withheld when absent facts do not establish an economic zero. This changes
coverage, and sometimes available primitive alternatives, relative to the
licensed source library. Annual formulas currently use the latest genuine
annual snapshot; quarterly and owner TTM formulas are separate. No claim of
exact licensed stock-level value replication is made.

## Columns and timing

Key: `(session_date, security_id)` within an immutable generation.
`session_date` is the last exchange session in a completed requested month from
the pinned calendar, including holidays/weekends; it is never calendar month end.
The default `--end` is the previous completed calendar month. Do not request an
unfinished month against a truncated calendar.

| Column | Type | Unit / definition |
|---|---|---|
| session_date | DATE | pinned last exchange session of month |
| security_id, cik | BIGINT | historical security and dated issuer keys; issuer may be unknown |
| available_at | TIMESTAMPTZ | monthly publication boundary: session_date 22:00 UTC |
| source_available_at | TIMESTAMPTZ | conservative maximum of all admitted source and quality clocks used by the row |
| accounting_datadate | DATE | latest genuine annual statement period; may differ from final ratios TTM basis |
| accounting_available_at | TIMESTAMPTZ | annual source vintage clock |
| accounting_currency | VARCHAR | original standardized currency; current kernels require USD |
| issuer_me | DOUBLE | absolute USD; all eligible constituent classes retained before qualification |
| issuer_cap_reason | VARCHAR | null when independently qualified; keyed reason otherwise |
| member_equity, univ_all_common, univ_r3000p | BOOLEAN | independent source population flags; unknown stays NULL |
| quality_reasons | VARCHAR / JSON | every missing feature has a reason; no absent-event zero without source completeness |
| source_clocks | VARCHAR / JSON | conservative per-feature source clock; nonnull value requires a clock strictly before monthly publication |
| registry features | DOUBLE | complete definitions and units in frozen formulas JSON |

Every selected accounting, price-quality, factor and source-primitive snapshot
has `available_at < 22:00 UTC` of the month-end session. Prices have dates no later
than that session. Monthly values become available at their stamped 22:00 boundary;
daily carry-forward must apply its own strict previous-session cutoff. A missing
publication clock remains missing; there is no fiscal-date clock fallback.
Restatements and delayed components are selected separately through each cutoff.
Historical accounting AR1 and variability reconstruct each of 72 historical
monthly states through that month's own pinned session and information cutoff.
Calendar/fiscal/currency gaps never become row-number lags or zero prior periods.

Annual staleness is 550 calendar days and quarterly staleness 400. Growth uses
positive prior bases, exact fiscal-year/quarter matches, and a 35-day annual
calendar tolerance. Average-denominator formulas require every component.
Generic operating and working-capital formulas are structural nulls for banks
and insurers. REIT NI metrics remain NI metrics and never stand in for FFO.
Per-share TTM growth requires weighted earnings/share evidence; unsupported
versions remain null. Gold insider/event features retain their exact requested
names and missing cohort/definition reasons. `ceq_iss_5y` requires 61 complete
months and source-backed issuer cap, splits, dividends and delisting returns.

## Native input admission

The controller supplies a `char_monthly_bindings_v1` JSON containing:

- Explicit `universe_generation` under `universe_release_v1/generations/`,
  `universe_manifest` SHA/bytes; full `universe_release.validate_generation` is mandatory.
- Explicit `fundamentals_generation` under `fundamentals_release_v1/generations/`,
  `fundamentals_manifest` SHA/bytes; full `fund_batch_release.validate_generation` is mandatory.
- Exact named products from `contract`: each contains `pattern`, `manifest`,
  `files` with SHA/bytes, and nonempty independently reviewed `source`, `clock`,
  `admission`. Wildcard membership must exactly equal the independently bound inventory.

`fundq` and `funda` must come from `compustat_std_v11`, whose persisted
`fund_stage` and input hash must identify the actual admitted fundamental generation's
`fundamentals_v11` leaf. `ratios_state` must come from `ratios_v11` and pin the exact
standardized manifest. No unversioned/fundamentals_v10/current-lake fallback exists.
All consumed producer members, not just pointer manifests, are authenticated.

Native source schemas are the current clean `compustat_std.py`, `ratios.py`,
`universe_release.py`, and these reviewed proof-product contracts:

| Product | Mandatory source-backed fields |
|---|---|
| calendar | session_date DATE; complete exchange calendar for requested months/history |
| member_equity | session_date, security_id, member_equity, available_at, source_available_at |
| primitive_quality | cik, datadate, accession, item, snapshot_available_at, available_at, source_available_at, certified, unit |
| cap_quality | security_id, session_date, available_at, source_available_at, certified, membership_certified, currency |
| price_quality | security_id, session_date, available_at, source_available_at, shares, close_adjusted, high_adjusted, price_certified, return_certified, corwin_schultz_spread; optional independently certified first_observation |
| prices | security_id, session_date, close, volume, ret (only pre2023 return values selected) |
| daily_factors | session_date, available_at, rf, mkt_rf, smb, hml, q_size, q_ia, q_roe |
| monthly_factors | month_end, available_at, rf, mkt_rf, smb, hml |
| monthly_market | security_id, month_end, available_at, source_available_at, shares_adjusted, issuer_me, dividend_dollars, return_certified, ret |
| sector | cik, sector, available_at |

Monetary primitive units must be absolute `USD`, shares `shares`, EPS `USD/share`,
employees `thousand persons`. No million-dollar/share scaling is silently inferred.
Equal-clock duplicate quality keys are rejected. Latest visible revocations
supersede older passes. Any eligible constituent with missing, zero, negative,
NaN or infinite ME invalidates the entire issuer cap; no class is discarded.
Missing proof products can only be supplied as explicit typed empty publications,
which produce documented nulls and coverage FAIL. They are not fabricated by this
consumer from a producer's self-certification.

## Build and validation

All real commands require the controller's exclusive <=1GiB guard allocation.
No Task7 real build, file hash/inventory, source acquisition or return run has occurred.

```powershell
# Run these through ../.superpowers/sdd/tier1-parity/run_memory_guarded.py.
# Set PYTHONPATH=src and OPENBLAS_NUM_THREADS=1 in C:/atx/atx-db.
.venv/Scripts/python.exe -m atx_db.alpha_panel.char_monthly contract
.venv/Scripts/python.exe -m atx_db.alpha_panel.char_monthly build --bindings <reviewed.json> --generation <g> --end 2026-09-30 --max-new-months 1
.venv/Scripts/python.exe -m atx_db.alpha_panel.char_monthly validate --generation <g>
```

The connection is file backed with spill, 380MB and one thread. One security's
1260-session daily and 241-month market history is fetched at a time; issuer
accounting cache is capped at 128 and each issuer snapshot at 500 rows (overflow
fails, never silently truncates). Output insert batches are 32 and Parquet row
groups 2048. Monthly receipts bind exact source/config/code/window hashes and
independent keys/flags/counts. Resume revalidates every completed unit. Incomplete
or changed candidates never replace the atomic pointer. Validation checks exact
schema, every file and receipt, complete month inventory, strict row/feature clocks,
unsupported/holdout nulls, source rechecks and regenerated full coverage/gates.
Never delete another writer's lock; a failed writer's recovery requires controller ownership.

## Evidence and current coverage

Coverage is per feature, year 2020--2025, on all three independent populations;
every missing/unsupported feature retains the original eligible-cell denominator.
The accounting >=90% and price >=97% gates use the worst original feature/year
on `univ_r3000p`. Unknown membership is reported separately. All six years and
original 153 names remain binding. Post2022 return-derived products are prohibited
and explicitly null, which can legitimately fail original price coverage.

| Years | Population | Actual coverage |
|---|---|---|
| 2020, 2021, 2022, 2023, 2024, 2025 | member_equity | FAIL / unmeasured; upstream admission held |
| 2020, 2021, 2022, 2023, 2024, 2025 | univ_all_common | FAIL / unmeasured; upstream admission held |
| 2020, 2021, 2022, 2023, 2024, 2025 | univ_r3000p | FAIL / unmeasured; upstream admission held |

`replicate --bindings <evidence.json> --output <json>` accepts exact independently
bound labels, JKP, OSAP and formation-quality products plus the frozen registry hash.
Labels include formation, entry/exit dates, issuer keys, complete total return and
delisting qualification. Formation quality supplies one dated primary security,
issuer cap, NYSE flag and cap certification. Source date metadata is checked before
return values are read. Every entry/exit/benchmark date must be strictly pre2023,
and next-session entry follows formation. Terciles use nonmicro NYSE20 cap screening,
equal-count characteristic thirds, NYSE80 capped VW, >=5 names per leg and >=60
matched monthly observations. Every original factor outcome is reported, including
unsupported/insufficient windows; factor and sign-share denominators remain 153.
No statistical significance or predictive ability has been measured or asserted.

R18 fixes the requested holding axis at 2013-01 through 2022-12 (120 slots) and
the formation axis at the pinned preceding-month last exchange session,
2012-12 through 2022-11. Minimum paired observations remains60. Missing calendar
or feature history remains explicitly failed in the120-slot inventory. Labels and
benchmarks outside these exact axes are refused before aggregation. Existing
2020–2025 coverage dates and all153 JKP outcomes remain unchanged.

OSAP requires `osap_mapping: {path, identity:{sha256,bytes}, expected_sha256}`;
the caller SHA must identify a separately independently reviewed contract.
Contract version `char_monthly_osap_mapping_v1` binds this registry SHA, an
`authoritative_metadata` file-identity inventory, all153 `entries` (mapped or
excluded with independent metadata identifier/reason), and a separately pinned
`admission_record` with caller expected SHA. The record contains the registry SHA,
canonical entries digest `mapping_sha256`, and phase `frozen_before_any_return_run`.
Mapped entries explicitly identify OSAP name, sign (+1/-1), decimal units,
vw_cap weighting and usa country. Duplicate names, unsupported conventions,
missing inventory or absent reviewed metadata refuse evaluation. All matched
members, including missing/failed correlations, stay in the OSAP denominator;
a median of successful matches cannot pass an incomplete overlap. This interface
does not invent or certify an actual authoritative mapping; root/Task8 must
independently supply and review it before any actual returns.

Formation-quality input columns are allowlisted: formation, security_id, cik,
available_at, source_available_at, me, nyse, primary_security, certified. Keys/CIK
and issuer cap must match the validated feature row; both clocks precede cutoff.
Only the last four qualifications enter portfolio rows. Features, population
flags and publication/source clocks cannot be overwritten by external audit rows.

`compare-ratios` takes explicit final `ratios_v11` monthly file pins, leaves labels
out, and measures all72 dates x bm/ep/sp/gpa (min10 finite pairs, rho>=.99).
It compares be_me, nix_me (NI incl extraordinary items), sale_me, gp_at to the
matching WFRS formula. It reports paired/eligible counts and every keyed disagreement
in bounded month artifacts: issuer/clock, currency, annual-versus-TTM period,
issuer cap, missing primitives and book/numerator definitions. Diagnostic cohorts
do not replace the binding member-equity comparison. Task4/17 acceptance remains held
until this actual comparison is measured on completed final producer files.

Comparator manifest identity must equal the fully admitted feature snapshot's
ratios_v11 manifest, and caller monthly files must equal its complete monthly
inventory. `ratio_basis` uses the same separately reviewed `{path,identity,
expected_sha256}` contract interface, version `char_monthly_ratio_basis_v1`,
registry SHA and independently pinned `product` of per-date/security/formula
basis rows. Each row supplies CIK, fiscal datadate, genuine_annual flow basis,
same_annual_snapshot balance basis, USD currency, issuer cap and
all_eligible_issuer_classes cap basis, matching fundamental vintage, the explicit
book/NI/sales/GP definition, and information/source/fundamental/cap clocks.
Unknown or incompatible basis never forms a pair; every original member must
pair compatibly and all72 dates/288 comparisons must exist for acceptance.
Reported rho cannot turn incompatible monotonic values into a passing gate.
Actual independent basis proofs remain an outstanding source dependency.

Seasonal annual lags are12a..12b at multiples12; nonannual lags are
12(a-1)+1..12b excluding multiples12. Current observation is t, so lag i reads
index-(i+1); lag240 requires241 observations. rmax5_rvol_21d separately validates
its trailing252-session denominator, minimum120 observations and certification
of every used price/return cell. Quarterly metadata includes txditcq book equity
and explicit standalone-flow YTD predecessors; four owner TTM EPS/share metrics
are structurally unsupported until weighted earnings/share inputs are admitted.

Remaining limits: no qualified actual source bindings/publication; exact duration
parameters; complete insider/event source cohorts; historical source completeness,
currency/sector/source zero validation; production memory/time measurement; annual
versus latest-quarter TTM characteristic basis; actual original gates and Task16
predictive evidence. These are explicit outstanding acceptance work.

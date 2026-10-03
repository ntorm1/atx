# universe_release_v1 (Task16 foundation)

Status: **NOT_PRODUCTION_READY**. No real candidate has been built or admitted.
This independent layer contains Task2 R5-1/R5-2 at release admission; the
legacy universe/checkpoint defects remain open. Byte identity is distinct from
economic/source qualification. `source_quality_certified` is always false.
French historically fails 10/60 months; no threshold, population, period or
denominator changed. Task16 golden panels, labels, predictive evaluation,
segment/note admission, exports and consumer acceptance remain future work.

The immutable explicit stage is
`universe_release_v1/generations/<generation>/year=YYYY/universe.parquet`.
`current.json` pins generation and manifest SHA/size; it is published last.
`previous.json` supports independently revalidated rollback. Consumers must
bind this versioned stage, generation manifest and actual output files, never
fall back to `universe/`. `resolve_current(root)` checks the pointer; full
`validate_generation(root,generation,con)` additionally rechecks source/code,
every unit, exact keys, schema, clocks, receipts and evidence. A pointer alone
does not certify the generation. Coordinate this contract with Task6/7 before
any actual consumer measurements; their old coverage stays provisional.

Keys are `(session_date DATE, security_id integer)`; both are nonnull and
unique and must equal the complete original panel key set, including delisted
lines, on valid calendar sessions in the exact year. Required years remain
2018-2026 independently of bounded work selection. Missing a required year
returns `partial_cache_only`, without gates, manifest or publication. A payload
without a sealed receipt remains visible and refuses reuse; use a fresh
generation rather than deleting interrupted evidence. Completed generations
refuse further writes. A stage writer lock excludes competing release writers;
an orphaned lock requires inspection before recovery.

| Columns | Type/unit | Definition/source | Clock/staleness |
|---|---|---|---|
| session_date, security_id | DATE, integer/id | Native panel axes | session |
| cik | integer/id, nullable | panel or PIT link_table_v3 | issuer input < previous-session cutoff |
| shrcd_like, exchcd_like | INTEGER/code, nullable | native identity_v3 and FINRA repair | native strict issuer/listing clock |
| exchcd_basis, filer_regime | VARCHAR/category, nullable | native codes and SEC regimes | native strict cutoff |
| is_adr, is_reit, is_spac, is_cef_etf, is_foreign_issuer | BOOLEAN | unchanged native classification engine | sparse historical assertions; refresh remains needed |
| me | DOUBLE/USD, nullable | shares_daily shrout × raw close | close of session |
| univ_all_common | BOOLEAN | unchanged universe-all-common-v3 | close formation, next-session holding |
| univ_r3000p, univ_r1000p, univ_r2000p, univ_sp500p | BOOLEAN | unchanged indexes proxy membership | rank-date availability < native cutoff |
| available_at | TIMESTAMP/UTC | exactly session_date 22:00 UTC | daily; same-day close information |

Daily consumers expose a row only at strict GC1 cutoff; joining formation-date
flags for next-session entry preserves the native formation convention. These
rows must not be interpreted as available before their close clock.

`release_inputs.reader_contract()` is the static actual-native reader inventory:
calendar; every original panel/shares/membership year; FINRA names/codes, links,
SEC regimes/filings/profile, industry, cover pages, both incorporation products,
REIT status, reconstitution summary; 2020-25 liquidity/borrow coverage inputs.
French benchmark and hand verdict bytes, binding JSON bytes and actual module
SHA/size are also pinned. Producer members are independently verified; empty
producer declarations fail. An omitted consumed side product needs an explicit
independent named binding rather than inheriting the parent certification.
Required optional-native source products now fail closed when absent.

Binding document `version=reader_binding_v1` contains `products`, exactly equal
to the static reader contract. Every product has an exact `files` mapping from
lake-relative path to `{sha256,bytes}`, and nonempty `source`, `clock`,
`admission` provenance. Extra wildcard files, missing files, substitutions,
path escapes, absent provenance and stale actual bytes fail. The controller
must derive/review these explicit bindings from quiescent producer evidence
under the guard; this layer never turns caller text/flags into semantic proof.

All original native gate adapters are retained: daily 3200-4800 mean each
2020-25 year (T2-a), French ±5% every original 2018-22 month, R2000 1900-2100
each reconstitution, codes ≥99% each 2020-25 year, ≥95% precision on the exact
native 100-line stratified sample. The original no-price diagnostic, counts,
daily counters and complete 30-field coverage on all_common/r3000/member
denominators remain in generation-local validation evidence. SHA/size bindings
tie evidence and hand/benchmark inputs to the final candidate. Native SQL is
reused through isolated function globals; no native module/global paths or
legacy cache/publication are mutated. Coverage counts return availability only;
no return statistic is computed.

| Real measured coverage/gates | 2020-2025 |
|---|---|
| 30 fields / three original universes | NOT_MEASURED (no authorized real build) |
| Current acceptance gates | NOT_MEASURED; historical French FAIL retained |

Operational commands, after independent code review and exclusive slot:

```powershell
$env:PYTHONPATH='C:/atx/atx-db/src'; $env:OPENBLAS_NUM_THREADS='1'
# contract prints static code-only inventory, without data IO
atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.universe_release contract
# Run from atx-db; every actual hash/scan/build/check uses guard <=1 GiB.
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe_release build --bindings <reviewed-json> --generation release_001 --max-new-years 1
# repeat with same binding/generation for authenticated completed-year reuse
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe_release validate
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe_release rollback
```

Each admission boundary rehashes the full input/code inventory; bounded years
do not bound identity hashing or issuer loading. Resource/runtime at warehouse
scale remains unmeasured. Existing producer manifests may be stale/incomplete;
no actual binding has been supplied yet. Task5 legacy fundamental batch reuse
also remains unqualified: CODE_VERSION/source-size/mtime/output existence does
not prove output SHA/code/preparation/key binding. This independent layer is
reusable for future batch admission, but that adapter has not been implemented.

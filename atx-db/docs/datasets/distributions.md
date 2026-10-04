# Distributions and indicated dividend rates

Task13 publishes `corporate_actions/distributions.parquet` as the current pointer copy of
`corporate_actions/distributions_products/<content-id>/distributions.parquet`. Products
retain every source vintage; existing product directories cannot be replaced. The
companion `distributions.latest.json` pins the product manifest. This does not replace
the existing vendor factor events, which are an ex-date comparison source only.

Native inputs are retained SEC 8-K/EX-99 declaration text or original per-share declared
XBRL facts. No paid historical dividend can create an indicated rate. The bounded native
text extractor requires an explicit USD currency, common-stock class, declaring act and
per-share cash amount. It annualizes only explicitly **regular** dividends with a stated
monthly, quarterly, semiannual or annual cadence. Bare `$`, special status unknown,
unknown cadence, missing security/class/currency mapping, preferred stock and payment-only
statements do not support an indicated rate. Unknown dates remain NULL. Future declaration
dates relative to source availability reject the build. XBRL duration totals retain
`CommonStockDividendsPerShareDeclared`, source currency and duration and never annualize.
Explicit distributed shares per parent share support a spin-off ratio; no inferred cash
amount or declaration date is attached to that ratio.

Replay command, from `C:/atx/atx-db`, under the root-coordinated guard:

```powershell
$env:PYTHONPATH='C:/atx/atx-db/src'; $env:OPENBLAS_NUM_THREADS='1'
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.3 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.distributions --documents <reviewed-retained-native-catalog.jsonl> --input-manifest <pinned-security-class-linkage-manifest.json> --input-manifest <pinned-source-clock-manifest.json>
```

The catalog must contain one JSON object per retained document, with `source_path`,
`source_sha256`, `source_url`, `accession`, integer `cik`, timezone-aware `available_at`,
`link_basis`, and nullable `security_id`, `link_tier`. An indicated rate additionally
requires `security_class` and `currency` matching the extracted native class and currency.
`format: xbrl_json` selects a retained JSON document with `facts`, each containing
`concept`, `value`, `unit` (`USD/shares` or another ISO currency/shares), `start`, `end`
and optional explicit `security_class`. Other native concepts are skipped. Catalog clock
and security assertions require independently reviewed upstream provenance; hashes alone
do not certify their semantic correctness. Native HTML conversion and original-source
acceptance/class reconciliation are outstanding; do not present normalized text as raw
EDGAR bytes. UTF8 replay currently limits each document to2MiB and each catalog to100,000
emitted rows. Partition and review larger inputs before expanding these limits.

Inputs and code define the product identity; all supplied manifests and native document
bytes are hashed before and after replay. The native document hash, source URI, accession,
source path and evidence survive in every event. Event key is `event_id` (accession,
source hash, extraction ordinal), so later source vintages are distinct events.

| Columns | Type | Unit/definition/source |
|---|---|---|
| event_id; accession; source_url; source_doc; source_sha256; evidence | string | Immutable native event identity and exact source evidence |
| security_id; cik; link_basis; link_tier | integer/integer/string/string | Explicit supplied line linkage; no issuer-to-class expansion |
| declare_date; record_date; pay_date | date | Explicit native dates; missing remains NULL |
| kind; currency; security_class | string | cash_dividend, historical_declared_xbrl, spin_off; native ISO money and class |
| amount; declared_regular_rate | double | Native currency per share; regular rate only if expressly regular |
| declared_cadence | string | Native explicit cadence; unknown NULL |
| special_flag | nullable boolean | Explicit special true, explicit regular false, otherwise NULL |
| indicated_annual_dividend_rate | double | Native currency/share/year, regular rate × explicit cadence only on a mapped class/currency |
| dividend_announcement_available_at; available_at | timestamp UTC | Supplied source-publication clock, never a declaration/payment clock substitute |
| security_class_currency_basis | string | Exact mapped native class/currency; NULL on unsupported mapping |
| spin_off_ratio; distributed_security | double/string | Distributed shares per parent share and explicitly named distributed company |
| source_concept; period_start; period_end | string | Native XBRL concept and duration |
| vintage_risk; extractor_version | boolean/string | Source-vintage qualification and extraction rule |

Daily visibility is strictly `available_at < 22:00 UTC of session d-1`. Monthly
visibility is strictly `<22:00 UTC of the month-end session`; future payment and future
declaration information cannot select an earlier rate. Rows are event data without
automatic expiration. A downstream rate carry policy/staleness rule requires separate
source certification and must not silently carry historical declarations forever.

Actual measured 2020–2025 member_equity, univ_all_common and univ_r3000p coverage: **not
measured**. No actual publication/build or new source acquisition has occurred in this
lane. All three original gates remain FAIL/unmeasured: declaration date on≥90%of
member-line cash dividends2020+, amount within1%of vendor factor-implied cash on≥95%,
and200distinct native buyback handchecks with≥95%agreement. Missing/unmatched source
amounts stay in the cash denominator. The pure gate arithmetic is fixture verified,
not actual acceptance evidence. All publication manifests fail closed without actual
gate evidence. Task4 must refresh `divyield` source handchecks/gates before gold/Task17;
this producer contract alone does not close its40missing source primitives.

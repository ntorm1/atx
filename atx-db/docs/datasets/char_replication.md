# Characteristic replication

`python -m atx_db.alpha_panel.char_replication` compares reviewed Task7 issuer-level characteristic portfolios with native JKP and OSAP monthly benchmark returns. No actual return run has been admitted. Source admission and the three original economic gates remain FAIL/unmeasured; this implementation is not a production qualification.

The output is a validation JSON artefact keyed by admitted manifest SHA256, holding month and original JKP characteristic name. Monthly returns/means/standard errors are decimal returns; rho and mean-sign agreement are dimensionless, t-statistics are dimensionless and pair/name counts are integers. Missing values are JSON null with explicit reasons. The formation clock follows Task7 PIT (`available_at` and source clocks strictly before formation22:00UTC); labels are realized evaluation evidence. This artefact has no daily as-of macro and does not replace a feature table's PIT clock. Any source/code/config byte change makes its generation stale.

| Actual evidence | Rows/months | Status |
| --- | ---: | --- |
| Admitted real monthly replication | 0/120 | FAIL/unmeasured |
| JKP original153 per-characteristic outcomes | unmeasured | FAIL/unmeasured |
| OSAP independently admitted overlap | unmeasured | FAIL/unmeasured |

Offline fixture results are implementation tests and do not populate this coverage table.

The frozen [configuration](char_replication.config.json) retains all original153 characteristics, including unsupported `eq_dur`, and exactly120 holding months January2013–December2022. Formation months are December2012–November2022. A usable label covers the preceding formation close and every actual holding-month calendar session. A holding window reaching2023 is rejected before any statistic. R21 permits later historical predictor construction and individual source-backed DLRET records; that permission does not extend this evaluation or forward labels. Changes to the reviewed upstream Task7 semantic freeze invalidate every Task8 snapshot and require a separately reviewed refreeze before measurement.

The offline engineering checkpoint pins proposed Task7 R21 SHA `caf3bc45efda101fa4f5f96831093858388d27c71c45d836392b0fa4b00caa16` and Task6 factor-config SHA `0d38a84b7a12825511686956ac52f87bd24769ee8741477cdb425e61b5a4f4b0`. Their independent approval is a prerequisite for an actual run. The Task8 canonical configuration hash is `4db383560aff8770c50cb0f9fcd634e5b89d3fd74d35ddf21a887a3361601cfa`; use this canonical semantic hash in reviewed metadata, and the separate physical-file identity when pinning code bytes.

R20 resolves the brief's breakpoint wording: Task7 `char_monthly_evidence.tercile` supplies the unchanged economic recipe. NYSE20 defines nonmicro (strictly greater ME), characteristic thirds use **all country nonmicro** qualified primary common issuers, and those cutoffs assign micro issuers too. NYSE80 caps issuer-ME weights. The source requires at least10 NYSE size-reference names,10 characteristic-reference names and5 names per leg. Every primary common issuer needs positive finite cap and explicit NYSE status matching its independently reviewed classification proof. Unknown classification, missing caps or missing complete monthly labels block the month's own portfolios; these rows cannot shrink a reference population or silently disappear from a denominator.

JKP input is native `bench_jkp_us_factors` Parquet with the producer footer and exact schema `month_end,date,location,name,freq,weighting,direction,n_stocks,n_stocks_min,ret`. Only usa/monthly/vw_cap is accepted. `ret` is already direction-signed decimal return: do not apply direction again. Each selected month is fully reconstructed from its pinned native ZIP CSV member, including all native factor rows and counts. Later raw rows are skipped by date before return conversion.

OSAP input is the original `PredictorLSretWide` CSV, with a complete independently admitted native-column catalogue. Percent values are divided by100 once; any sign conversion and per-signal weighting must come from reviewed actual source metadata. Missing weighting columns do not imply vw_cap. All independently admitted original153 overlap members remain in the OSAP denominator; no successful subset can replace the complete overlap.

Label admission calls the reviewed Task7 complete-window qualification and additionally reconstructs each actual source daily total return as `(close * split_multiplier + cash_dividend) / prior_close - 1`. Prices and explicit action-ledger rows must be independently pinned, with one row per security/session, USD, unadjusted-close/split/cash-per-prior-share convention. Missing source cells or unsupported terminal treatment remain missing. A Boolean certification or a hash without native economic reconstruction cannot qualify a label.

Each characteristic reports separate JKP/OSAP paired samples, Pearson rho, own/benchmark means, strict mean-sign agreement, IID t and Bartlett Newey-West HAC t with six calendar lags. Missing months do not compress HAC lags. At least60 finite pairs are required; constant-series rho/t are undefined. Original gates remain JKP rho≥0.7 for≥70% of153, JKP mean-sign agreement≥90% of153, and OSAP median rho≥0.6 with every admitted overlap member testable. Reported benchmark replication does not establish executable net profitability, source-quality certification or Task16 predictive significance.

## Controller admission

The controller must independently review and pin complete native products, source documentation, source mappings and label/formation proofs **before any return run**. The API/CLI requires an absolute, unambiguous binding path and a separate caller-supplied SHA256 of those exact binding bytes. A candidate's own marker or expected hash is insufficient. Public validation requires that caller hash again and always performs full native replay.

A reviewed pin is exactly `{"path":"root/relative/or/absolute/path","identity":{"sha256":"64lowerhex","bytes":123},"expected_sha256":"same64lowerhex"}`. All nested pins derive their authority from the independently admitted binding bytes. A product is exactly `{"files":{"relative/file.parquet":{"sha256":"64lowerhex","bytes":123}},"source":"actual source description","clock":"actual clock/convention","admission":"controller evidence reference"}`; no self-declared product string replaces byte reconstruction.

The complete binding schema is:

```json
{
  "version": "char_replication_bindings_v1",
  "registry_sha256": "reviewed Task7 semantic SHA256",
  "config_sha256": "canonical Task8 configuration SHA256",
  "feature_generation": "char_monthly/generations/explicit_generation",
  "feature_manifest": {"sha256": "64lowerhex", "bytes": 123},
  "products": {"jkp": "product", "osap": "product", "labels": "product", "formation_quality": "product"},
  "jkp_contract": "reviewed pin",
  "osap_contract": "reviewed pin",
  "formation_contract": "reviewed Task7 formation pin",
  "label_contract": "reviewed Task7 complete-window pin",
  "label_source_contract": "reviewed Task8 native label-source pin"
}
```

Replace schematic string values with full objects. Task7 public `validate_generation` must admit the entire feature generation and its source dependencies; a partial feature candidate does not qualify. Labels contain exactly `formation,cik,security_id,entry_date,exit_date,ret,return_certified`, with no duplicate formation/CIK keys. Formation quality has no duplicate formation/security keys. Proofs retain Task7 R18-window2 versions and per-security/month complete observations. Classification proofs additionally contain actual `nyse` Boolean and `issuer_me` matching the quality row.

For each kind (`jkp` or `osap`), `char_replication_<kind>_contract_v1` includes registry/config hashes, exact `product`, `entries`, reviewed `catalog` and reviewed `admission_record`; JKP also includes reviewed `raw_archive` and exact `zip_member`. All153 `entries` occur identically in contract and `char_replication_<kind>_catalog_v1`. A mapped entry is exactly `{"status":"mapped","native_name":"name","sign":1,"weighting":"actual_convention","metadata_id":"metadata/path"}`; an excluded entry is exactly `{"status":"excluded","reason":"source-backed reason","metadata_id":"metadata/path"}`. Native names are unique. Catalogue `metadata` inventories actual documentation bytes; catalogue `native` inventories all native names with `weighting`, `unit`, `sign_to_own` and JKP `direction`. The separately reviewed `char_replication_mapping_admission_v1` contains registry/config hashes, `phase:"frozen_before_any_return_run"`, canonical `mapping_sha256` and exact `catalog_identity`. These artefacts require independent source-based decisions; the harness does not create authoritative mappings from observed outcomes.

`char_replication_label_source_v1` contains registry/config hashes, `convention:"unadjusted_close_split_cash_per_prior_share_v1"`, `currency:"USD"`, and `prices`/`actions` products. Price rows expose `security_id,session_date,close`; action rows expose `security_id,session_date,split_multiplier,cash_dividend,terminal_treatment`. An explicit no-action source row has multiplier1, cash0 and terminal`none`; absent data is not filled with zeros. Each Task7 window's price/action source pins must belong to these exact products.

## Bounded operation and restart

`contract` is offline. Build and validation require the controller's exclusive memory guard (`ATX_GUARD_JOB`), DuckDB128MB/one thread, explicit root/generation and external binding hash. Root owns all real-data commands. Example argument vectors for the root guard (the SHA placeholder must be independently computed/admitted first):

```powershell
$env:OPENBLAS_NUM_THREADS='1'
$env:PYTHONPATH='C:/atx/atx-db/src'
& C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.char_replication contract
# Under the root-owned exclusive guard, with a unique log and --wait-minutes10:
& C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.char_replication build --root C:/atx/atx-db/data/alpha_panel/v1 --bindings C:/atx/.superpowers/sdd/tier1-v4/task8-bindings-v1.json --expected-bindings-sha256 <independently-admitted-sha256> --generation explicit_generation --max-new-months 1
& C:/atx/atx-db/.venv/Scripts/python.exe -m atx_db.alpha_panel.char_replication validate --root C:/atx/atx-db/data/alpha_panel/v1 --expected-bindings-sha256 <same-independently-admitted-sha256> --generation explicit_generation
```

`--max-new-months1` limits new monthly work; upstream/source admission and replay of existing receipts remain mandatory. Restart repeats the same source-pinned command. Source/code/config drift, changed caller admission, duplicate or abbreviated inventory, tampered receipts/report and unsealed candidates fail closed. The source snapshot pins native products, metadata/proofs, complete feature receipts/payloads and relevant code before/after monthly work. New snapshots/monthly receipts/report/manifest use exclusive immutable writes; failed attempts and partial-file forensics remain. An interrupted writer lock requires controller inspection, not automatic stealing.

Only after120 exact monthly receipts and full public economic replay does the candidate publish `char_replication/current.json` and `validation/char_replication.json`. A partial run publishes neither. Public validation replays all120 native months and recomputes the complete report and gates, with before/after source/code/receipt pins. The validation artefact exposes the admitted manifest and report; it never changes readiness from `NOT_PRODUCTION_READY` or certifies source quality.

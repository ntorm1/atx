# `equity-universe` — point-in-time dollar-volume universe construction

Checkpoint 15. The first membership series in this program ranked only on data at or
before each rank date and effective from the next session: 84 monthly rebalances over
2013-01-02..2019-12-31, three sizes x two bands side by side, with churn, per-year
coverage and union sizes, an inferred delisting table and a quantified survivorship
caveat. It builds universes and counts them. **No IC, no forecast, no alpha claim.**

Frozen design: `atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md`
(Revision 3 plus §15 implementation rulings), SHA-256
`4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8`, embedded in
`atx-impl/src/stage_equity_universe.cpp` as `kEquityUniverseDesignNoteSha256` and re-hashed
by the runner before every launch. §6 is the pre-registration; §11 the qualifications.

---

## Validation status

Stage 1 universe construction measured 2026-09-20, attempt 1, status
`stage1_universe_construction_measured`. See the
[checkpoint receipt](../../atx-engine/reviews/2026-09-20-point-in-time-universe-validation.json)
and checkpoint 15 in the [platform progress record](../../atx-engine/docs/PLATFORM_PROGRESS.md).

Receipt: atx-engine/reviews/2026-09-20-point-in-time-universe-validation.json (SHA-256 2c52c6a4c2d2e841d8ffe3b8e1ced3e8c79d40eeba5a2176bcfc75b06068c07e; status stage1_universe_construction_measured)

Native evidence: engine `DataPointInTimeUniverse` 37/37, oracle comparator 42/42 (twice),
data-group regression 94/94, impl `ConfigEquityUniverse` + `StageEquityUniverse` +
ledger 60/60. Real run: 1,955 sessions, 13,369 source ids, 84 rebalances, 28.84 s wall,
peak working set 87,539,712 bytes against a 3,000,000,000-byte budget, ledger lines
`iteration15-point-in-time-universe-0001` pre-registered then completed. The runner crashed
after the stage finished; `iteration15-equity-universe-measurement-attempt1-reeval.json`
recomputes acceptance from the recorded digests (`accepted_reevaluated` true) and the stage
exit code is **inferred** 0 from the terminal evidence. No re-run was performed.

**Data-hole caveat (read before using any 2016/2017 number).** The vendor archive carries
corrupted open/high/low on the session before every NYSE holiday from 2016-01-15 through
2018-02-16 (19 sessions; `open` equals the prior close, so about half of each session's rows
fail `ohlc_order_violation` under `tickerhistory-qa-v1`). 2016-12-30 is the only such session
that is also a rank date, and a member must have a bar on the rank session, so the 2016-12-30
rebalance drops 556 / 1,093 / 1,674 top-1000/2000/3000 incumbents as `drops_last_bar` and
2017-01-31 reverses it. Contaminated: `churn.csv` at those two dates (all cuts), the 2016
`drops_last_bar` totals, `union_by_year.csv` 2017 `distinct` and every `cumulative` column from
2017 on, the 2016/2017 `nonmissing_fraction` medians. Not contaminated: every other rebalance,
`delisting.csv` first/last-bar extents, survivorship per-year fractions. The run stands as
measured (ruling R15-17); remediation (`tickerhistory-qa-v2` and/or a hole-aware rank rule) is a
new pre-registration. Full diagnosis:
`.superpowers/sdd/equity-platform-parent-goal/cp15-idgap-investigation.md`.

---

## 1. Running it

```powershell
build-equity/bin/atx-impl.exe equity-universe `
  --segments-dirs 'C:/atx/data/tickerhistory_training_native_20260919/segments;C:/atx/data/tickerhistory_training_native_2014_20260920/segments;...;C:/atx/data/tickerhistory_training_native_2019_20260920/segments' `
  --preparation-manifests 'C:/atx/data/tickerhistory_training_20120326_20131231_20260919/manifest.json;C:/atx/data/tickerhistory_training_20140101_20141231_20260920/manifest.json;...;C:/atx/data/tickerhistory_training_20190101_20191231_20260920/manifest.json' `
  --out 'C:/atx/data/equity_universe_pit_2013_2019_20260920' `
  --rank-start 2012-12-31 --rank-end 2019-11-29 `
  --max-working-bytes 3000000000 `
  --trial-ledger 'atx-engine/reviews/trial-ledger.jsonl'
```

The allow-list is frozen in `stage_equity_universe.cpp` `resolve()`: `--segments-dirs`,
`--preparation-manifests`, `--out`, `--rank-start`, `--rank-end`, `--max-working-bytes`,
`--trial-ledger`, `--quiet`, `--digest-only`. Any other set flag fails with `unsupported
flag`; any legacy override (`--config`, `--panel`, cost or book fields) fails with
`unsupported override of the frozen pre-registered recipe`. **There is no `--top-n`, `--band`,
`--rank-key` or `--cadence`**: the cuts, key and cadence are constants.

| Flag | Semantics |
| --- | --- |
| `--segments-dirs a;b;...` | `;`-separated segment directories (an empty entry is rejected; a path containing `;` cannot be expressed). Every `*.seg` filename must parse as `YYYY-MM-DD`; a date at or after 2020-01-01 is **refused**, never skipped; dates are sorted globally and each directory's range must be disjoint from every other's. |
| `--preparation-manifests a;b;...` | Same count as directories, paired positionally; each must hash to the `preparation.manifest_sha256` of that directory's `_ingestion.manifest.json`. Source of the three quarantine/rejection columns of `coverage_by_year.csv`. |
| `--out` | must not exist; a `.pending` marker is reserved and removed on success |
| `--rank-start`, `--rank-end` | inclusive `YYYY-MM-DD`; rank sessions are the attached sessions whose UTC month differs from the next attached session's, filtered to `[start, end]`; `end` must precede the last attached session and 2020-01-01; at least 63 attached sessions must precede or equal the first rank session |
| `--max-working-bytes` | default 3,000,000,000; must exceed the overhead reserve |
| `--trial-ledger` | default `atx-engine/reviews/trial-ledger.jsonl` |

`config.cpp` stores the four new flags verbatim (each rejects an empty or `--` value, exactly
as `--trial-ledger` does); list splitting and date validation belong to the stage.

---

## 2. The pre-registration (design §6, frozen)

| Parameter | Value |
| --- | --- |
| Rank key | ADV63: median of `close x volume` over the trailing 63 sessions ending at the rank session, inclusive; median `(a+b)*0.5` binary64 |
| Fields read | `close` (raw as-traded), `volume`, `shares` (reported only), `gics` (reported only) |
| Bar validity | id present, `close` finite and > 0, `volume` finite and >= 0; otherwise a NaN slot, never filled |
| Eligibility | >= 57 of 63 valid bars; raw `close` > 1.0 on the rank session (strict); a bar on the rank session required; `min_adv_usd` 0 |
| Cadence | monthly by data: the last attached session of each UTC month ranks; the next observed session takes effect |
| Rank dates | 2012-12-31 .. 2019-11-29, **84** rebalances; last effective 2019-12-02; membership covers 2013-01-02 .. 2019-12-31 |
| Warmup | 63 sessions; 2012 is warmup only, no 2012 membership |
| Cuts | `top_n` {1000, 2000, 3000} x band {0.00, 0.10} (`band_bp` {0, 1000}) = six cuts, all emitted side by side |
| Band rule | incumbent kept iff `rank <= top_n + top_n * band_bp / 10000`; best-ranked `top_n` when band-kept incumbents exceed `top_n` |
| Tie-break | key descending, then first-seen slot ascending |
| Drop classification | `last_bar < rank ordinal` => `last_bar`, else `rank` |
| Market cap | `vendor_market_cap_usd = shares x close`, `no-filing-vintage`, never a rank key |
| Seal | `RefuseAtOrAfterValidationBeginV1`: refuse any segment >= 2020-01-01; `kValidationBeginNs` / `kSealedBeginNs` unchanged |
| Ledger | purpose `point-in-time-universe-construction`, `trial_count_declared` 0, `trial_id` `iteration15-point-in-time-universe-NNNN` |
| Trial accounting | `N_15 = 0`; `N_14 = 30` unchanged; the six cuts are restrictions reported side by side (AR-7); selecting among them later is a new trial |

Changing any value above is a new pre-registration: a new ledger line and a new design SHA.
The trial-ledger validator accepts `trial_count_declared == 0` only for a purpose in
`kNonTrialPurposes`, and rejects a positive count for such a purpose.

---

## 3. Reading the outputs

All files are written under `--out`, LF endings, reals as shortest round-trip text, dates as
`YYYY-MM-DD`; two runs over the same inputs are byte-identical. `band` is the literal `0.00`
or `0.10` in every CSV. Cut columns in the per-year files are suffixed `_t{top_n}_b{band_bp}`.

| File | Columns / content |
| --- | --- |
| `membership.csv` | `rebalance_rank_date,effective_date,top_n,band,security_id,rank,adv63_usd,raw_close,vendor_market_cap_usd,gics,valid_observations,nonmissing_fraction,status` — one row per (rebalance, cut, member) in rebalance, cut, then rank order; `status` is `add` or `keep`; `vendor_market_cap_usd` and `gics` empty when NaN |
| `churn.csv` | `rebalance_rank_date,effective_date,top_n,band,adds,drops_rank,drops_last_bar,kept,members,one_way_turnover` with `one_way_turnover = (adds + drops_rank + drops_last_bar) / (2 * top_n)`; the seed rebalance has `adds = members`, turnover 0.5 by construction |
| `coverage_by_year.csv` | `year,sessions,dates_with_quarantined_duplicates,duplicate_positive_keys,rejected_rows,ids_seen,ids_with_valid_bar_median,rebalances,eligible_median,members_median_<cut>...,nonmissing_fraction_median_<cut>...,gics_missing_members_median_<cut>...` — one row per observed calendar year 2012..2019; rebalance-derived columns are attributed by the **rank** session's year as measured (2012 carries the 2012-12-31 rebalance, 2019 carries eleven) |
| `union_by_year.csv` | `year,distinct_<cut>...,cumulative_<cut>...` for 2013..2019, attributed by the **effective** session's year; `cumulative` at 2019 is the number checkpoint 16 compares with `kMaxIcInstruments` 4096 |
| `delisting.csv` | `top_n,band,security_id,first_bar,last_bar,first_member_effective_date,last_member_rank_date,exit_kind` — one row per (cut, ever-member); `exit_kind` in `rank_drop` (0), `last_bar_within_window` (1), `window_end` (2); `first_bar`/`last_bar` are true valid-bar extents over all sessions |
| `survivorship.json` | per cut: `ever_members`, `ended_before_window_end`, `censored`, `fraction_ended`, `per_year[]` of members at the rebalance effective on the year's first session and those whose last bar precedes the year's last session; a Russell 3000 2019 comparison and the lower-bound caveat |
| `membership.bin` | binary membership, layout below |
| `request.json` | the frozen config, attached segments with digests, `design_note`, `producer_executable_sha256`, `rebalance_count`, `trial_ledger` (`trial_id`, pre-registration line SHA) |
| `seal.json` | policy, `validation_begin` 2020-01-01, `sealed_begin` 2023-01-01, `segments_refused`, `latest_attached`, statement |
| `manifest.json` | written last: every output file's SHA-256 and bytes, `membership_bin` digest and FNV trailer, `parents` (1,970), `loader_executables`, `runtime_seconds`, `universe_id`, the nine qualification strings |

### `membership.bin` layout (little-endian throughout)

```text
"ATXPITU1" magic (8) | version u32 = 1 | adv_window u32 (63) | min_valid_observations u32 (57)
min_raw_price_exclusive f64 (1.0) | top_n_count u32 then top_n u32[] ascending
band_count u32 then band_bp u32[] ascending | rebalance_count u32 = R
per rebalance: rank_session_key i64 | effective_session_key i64 (> 0)
  per cut (top_n outer, band inner): member_count u32 | security_id i64[] ascending | rank u32[] parallel
trailer: FNV-1a-64 over every preceding byte, u64
```

`decode_membership_bin` parses the body structurally first (short read, count beyond the
remaining bytes, trailing bytes, non-ascending ids => `InvalidArgument`) and verifies the
trailer last (mismatch => `Internal`). Real run: 84 rebalances, header keys 2012-12-31 ..
2019-11-29, 12,099,428 bytes.

### Measured headline (attempt 1, `C:/atx/data/equity_universe_pit_2013_2019_20260920`)

One-way turnover per rebalance over the 83 non-seed rebalances, mean / median, and the
cumulative 2013-2019 union: top 1000 band 0.00 4.65% / 3.50%, 2,278; band 0.10 2.86% / 1.60%,
2,159; top 2000 band 0.00 3.86% / 2.60%, 4,447; band 0.10 2.43% / 1.20%, 4,220; top 3000
band 0.00 3.66% / 2.37%, 6,624; band 0.10 2.32% / 1.07%, 6,276. Only the two hole rebalances
exceed 10% in any cut. Members median equals `top_n` in every year and cut; the median count of
members without GICS is 0. Ever-members ended before 2019-12-31: 20.5% / 20.0% / 22.6% /
22.3% / 24.9% / 24.6% in the same cut order, lower bounds.

---

## 4. Reproducing it

1. **Ingest** the 2014-2019 windows: `python build-equity/audits/iteration15_ingest_tickerhistory.py`
   (`--dry-run` first; `--year` repeatable). It runs `prepare_tickerhistory.py` then `atx-impl load`
   per calendar year and writes `iteration15-ingest-<stamp>-attemptN.json`; the 2012-2013
   segments are the 2026-09-19 ingestion re-used unchanged.
2. **Run** the stage through the runner only: `python build-equity/audits/iteration15_run_equity_universe.py
   --dry-run`, then without `--dry-run` (`--attempt N` versions every audit output and the data
   directory; `--pin-segments` makes a dry run hash every `.seg`, which a real run always does).
   It refuses to launch unless the on-disk
   design note hashes to the embedded constant, the stage source embeds the same value, the seven
   directories and manifests are in date order, and the output directory is absent.
3. **Compare** the native markers with the oracle: `python build-equity/audits/iteration15_native_comparator.py
   --logs build-equity/audits/iteration15-data-t1fix1-tests.log --out ...`. It never reads
   real-data numbers.
4. **Write the receipt**: `python build-equity/audits/iteration15_write_validation_receipt.py
   --measurement build-equity/audits/iteration15-equity-universe-measurement-attempt1-reeval.json`
   (`--dry-run` lists what is missing; it refuses an existing `--out`).

---

## 5. What this does not establish

1. **No alpha, no forecast, no Sharpe, no capacity.** Membership lists are inputs to a future
   measurement; nothing here is evidence that anything predicts anything.
2. **No float or common-stock eligibility.** ETFs, ADRs, preferreds and funds can rank into the
   top 3000 by dollar volume; `instrument_type_eligibility` is `unknown`; GICS gaps are reported,
   not applied.
3. **Survivorship fractions are lower bounds.** The archive's backfill policy is unknown, it has
   no delisting date, code or return, and a name never in the archive is invisible.
4. **Vendor `shares` are unreliable** (lag splits, zeros): `vendor_market_cap_usd` is reported
   only and must not be used as a size screen.
5. **The 2018 thinning** from 83 dates (86 rows) with quarantined duplicate keys is counted in
   `coverage_by_year.csv`, not repaired; securityID reuse (77622) makes a handful of histories
   composite.
6. **The cumulative unions of 6,624 (top 3000) and 4,447 (top 2000) exceed `kMaxIcInstruments`
   4096**, as does the contaminated 2017 top-3000 distinct count of 5,022: checkpoint 16 must use
   per-year contexts or rule on the cap before any full-block evaluation.
7. **The data hole above** contaminates the listed 2016/2017 numbers; they describe the archive
   defect, not the market.
8. **No sanitizer, static analyser, include-clean build or CI covers this work.** No live trading
   and no broker action is performed or authorized.

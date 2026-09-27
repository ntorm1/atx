# Tier-1 v2 F.1/F.2: fundamentals research fast path

Design date: 2026-09-27. Authority: C-90, C-94..96, the tier-1 v2 index, and the next-goal-prompt-3 handoff. This is a design, not measured fundamental alpha or an implementation acceptance report. Paths below are relative to `atx-db/` unless prefixed `docs/` or `.superpowers/`.

## Decision and scope

Build a Parquet-only accounting vintage engine now, sharing the planned 2.9 modules and API. F.1 consumes verified FSDS v2 and CF-R v2 staging and produces issuer item vintages without B0/B1 or a warehouse connection. F.2 builds the priority fundamental features, projects them onto the price spine through the 3.3 rehearsal link contract, and grades the 2013-01 through 2023-12 selection sample under the unchanged policy v4. Holdout remains sealed until final labels at 3.8 and the registry's one authorized opening.

Recommended controller ruling: use **`w2_fund_fast`**, with a frozen, disjoint hypothesis roster. F.1 is the early implementation of 2.9's common item engine; 2.9 later adds the B1 lake adapter and parity acceptance. F.2 owns the priority tranche; 2.10's `w2_fund_a` contains only residual hypotheses. Selected existing `w0_existing` rows move to the fast wave before registration; 3.11 must not test them again as w0. A source-path change is not another independent discovery. Section 8 gives the registration boundary and its required controller adjudication.

Production accounting remains FC1 (`sec_filed_date_plus_46h_v1`). This design introduces a labeled research clock; it neither changes `_fundamental_clock.py` nor represents date-only CF-R history as verified acceptance-time history. No source fetching belongs to F.1/F.2.

## 1. Preconditions and observed state

PowerShell manifest inspection during this design found **69 v1 quarters** in `data/staging/fsds/` and **15 v2 quarters** in `data/staging/fsds-v2/`. The respective manifest schemas were `x4_fsds_staging_manifest_v1` and `x4_fsds_staging_manifest_v2`; all entries matched their v1/v2 stager version. The parent reported CF-R v2 at 3/85 batches last known. None of these partial inputs is publish-ready.

After that observation, the controller resumed both source jobs from the committed `8aba655c` export (FSDS progressed past 2015q3; CF-R resumed with one worker). Consult `task-X.4-ops-session5.md` and `task-1.4-ops-session5.md` for completion, not the historical counts above. The price-input rebuild is also in progress; this design claims no existing price grade.

| Dependency | Contract before acceptance |
|---|---|
| X.4 | Complete declared 69-quarter scope from 2009q2 through the pinned latest available quarter; every admitted quarter `x4_fsds_stage_v2`; verification receipt, checksums, EST/EDT/DST checks and orphan dispositions. Pin a completed directory explicitly; never choose a path because it is named canonical. |
| 1.4 | `cf-extract-v2` manifest and `members.parquet`, all 85 batches, 20,390 member dispositions, equivalence/regression receipts; freeze archive SHA and allowlist fingerprint. |
| 1.9 / 1.11 | Existing lake/store/guard infrastructure and XNYS calendar. |
| 3.3 | Rehearsal Parquet in section 5; owner-only accounting construction can proceed before this arrives, but projection and F.2 acceptance cannot. |
| 1.12 | Accepted price spine, source/repair hashes, formation session, line ME basis and split evidence. |
| 1.10 / 1.13 / 4.1 | Immutable policy and registry; wave runner available after its current owner hands it over; evaluation code frozen during a real run. 4.1 outputs are reported metrics, not a policy change. |

Partial-source dry builds may diagnose coverage and periods, with `scope_complete=false`; they cannot seal the final snapshot or register the wave as complete. Source completion is owned by X.4/1.4; 3.3 owns identity rehearsal and any OPS export requests.

## 2. Existing APIs and exact implementation ownership

The interfaces below were inspected in the working tree; source WIP was read but not adopted by this design task.

| File / API | Required integration |
|---|---|
| `fsds_baseline.py`: `STAGER_VERSION`, `verify_fsds_staging`, `canonical_rules` | Validate v2 manifest and file metadata; use the seed-driven alias/rule definitions. Its pandas `canonical_fsds_facts` benchmark keeps only qtrs 0/1/4 and selects latest vintages: it is not a bulk or PIT engine. |
| `companyfacts_stage.py`: `fact_schema`, `RULE_VERSION`, manifest | Use explicit `cik`, never unresolved `security_id` as an owner. Preserve `value_exact` and source archive lineage. |
| `research/research_lake.py`: `connect_bounded`, `export_lake_snapshot(None, snapshot_id, datasets, root=..., source_meta=...)` | Export immutable Parquet-only slices with integer `year`; existing `register_external_dataset` permits only `bench_*` and must not be used for accounting datasets. |
| `research/research_lake.py`: `lake_files`, `lake_relation`, `lake_dataset_digest`, `verify_lake_snapshot` | Read pinned manifest files, not ambient globs. Large item history is a catalog of sealed bucket/year snapshots, not one whole-history COPY. |
| `research/feature_store.py`: `FeatureStore.write`, `compute_feature_sha`, `standardize_to_store`, `write_manifest`, `load_feature_table_from_store` | Existing schema is dense `(eom,line_id,owner_id,raw,signed,rank_u,z,reason,clock_offset_s)`. It does not have an owner-key schema. Preserve existing schema and transforms. |
| `research/workers.py`: `run_jobs` | Guarded resumable slice orchestration; existing receipt semantics. |
| `calendar.py`: `expected_month_end_session`, `decision_cutoff_utc`, `next_session` | Actual last XNYS session at 22:00 UTC determines visibility; calendar eom is just the row key. |
| `research/trial_registry.py`: `TrialRegistry.register_wave`, `require_registration`, `verify_anchor`, `open_holdout` | Freeze rows, exact cells and policy, then commit anchor before any label read. Registrations cannot receive addenda. |
| `research/qualification.py`: `evaluation_spec_kwargs_v4`, `grade_wave_v4` | Use policy-built specs and cumulative registry burden; no replacement grading implementation. |

**F.1 creates** `src/atx_db/research/items_map.py`, `item_vintages.py`, `fundamental_sources.py`, `fundamental_identity.py`, `scripts/research_fundamentals.py`, and `docs/research/FUNDAMENTALS_FAST_PATH.md`. Optional narrowly scoped checks: `tests/test_research_item_vintages_pit.py`, `tests/test_research_fundamentals_resume.py`. The common files are then handed to 2.9 rather than recreated there. No MIG/ACT/STD/PANEL/FEAT edits are needed; a seed correction is relayed to the STD owner.

**F.2 creates** `src/atx_db/research/fundamental_chars.py`, `scripts/research_fundamental_wave.py`, `src/atx_db/seeds/research_fund_fast_preregistration.json`. Under **CAT**, modify `research/catalog.py`, `seeds/research_anomaly_catalog.csv`, `docs/research/ANOMALY_CATALOG.md`. Under explicit runner handoff from 1.13, extend `scripts/research_run_wave.py`; under **EVAL**, run registration/grading and update the committed `seeds/research_trial_registry_anchor.jsonl`. Optional one pipeline check: `tests/test_research_fundamental_wave.py`. Reuse `research/features.py` unchanged; changes there require FEAT, not implied ownership.

New pure-data source kind `item_vintage` must be added to catalog external-source validation with explicit allowed feature shapes and this wave's availability clock. It must not be mislabeled a warehouse `seed_metric` or a bar `panel_native`. Existing feature IDs, signs, domain rules and economic definitions remain canonical; their storage adapter may change before registration. Validate catalog and render its documentation under CAT. Formula deviations require a distinct named row or a documented pre-registration decision, never silently changing an existing definition.

## 3. F.1 datasets and public interfaces

Proposed additions, using 2.9's names:

```python
# items_map.py: immutable typed rows and a deterministic mapping digest
COMPUSTAT_ANALOG: dict[str, ItemChain]
# ItemChain records canonical item codes, alias priority/validity, unit, period kind,
# transform/sign, optional fallback, and missing-value policy, all in the digest.

# fundamental_sources.py: yields bounded normalized raw candidate batches
iter_fsds_candidates(manifest_path, *, ciks=None, quarter=None) -> Iterator[pa.RecordBatch]
iter_cf_candidates(manifest_path, *, batch_id=None) -> Iterator[pa.RecordBatch]

# item_vintages.py: reader is explicitly bound to immutable inputs, no hidden latest root
ItemVintageStore(root, build_manifest)
ItemVintageStore.items_asof(formations, items, lags) -> Iterator[pa.RecordBatch]
# compatibility entry point for 2.9: items_asof(formations, items, lags, *, store)

# fundamental_identity.py: parquet-only projection, with evidence eligibility enforced
project_owner_features(owner_batches, spine_snapshot, identity_manifest, *, basis)
```

`lags` is keyed by item mnemonic and contains integer fiscal-quarter offsets, not row offsets. For annual series, l4 is the prior FY; readers expose explicit columns `<item>_<freq>_l<k>` and an aggregate maximum input clock. Requested missing quarters stay NULL. Formation requests are processed by one formation year and an issuer bucket, with sufficient prior history for the requested lags. No reader consumes labels.

Files under `data/research/work/fundamentals/<build_id>/` hold slice receipts and temporary bounded Parquet; final `data/research/fundamentals/<build_id>/manifest.json` lists sealed lake snapshots `fund-<buildhash>-bNNN-yYYYY` and their hashes. Do not hand-write a competing lake manifest format: each slice uses `export_lake_snapshot(None, ...)`; the build manifest is an index over valid lake snapshots.

Datasets:

| Dataset | Fields and key |
|---|---|
| `fundamental_candidates` | CIK, accession, taxonomy/version, concept, unit, period_start/end, qtrs, reported fy/fp, form, value/value_exact, filed_date, accepted_utc, raw/staged hashes, source row ID, period/clock/mapping status. Stable row ID uses content, not physical row number. |
| `item_vintages` | 2.9 base schema: `owner_id,cik,item,freq,period_start,period_end,fiscal_year,fiscal_qtr,value,available_at,valid_until,accession,rule_id`; extensions `vintage_id,year,source_kind,clock_basis,period_basis,value_exact,unit,status,lineage_id`. `owner_id` is zero-padded 10-digit CIK text. `freq` is q/ytd/fy/ttm/instant. Key `(owner_id,item,freq,period_start,period_end,available_at,vintage_id)` is unique. |
| `item_lineage` | `(vintage_id,input_candidate_or_vintage_id,input_role)`, plus mapping/build hashes. Complete dependencies, including quarter subtraction and restatements; no huge list aggregations. |
| `item_dispositions` | One terminal disposition per candidate, with quarantine reason, duplicate/conflict group and chosen source; counts reconcile to candidates. |
| `owner_features` (F.2) | `(eom,owner_id,feature_id)` raw/status/max-input-clock and lineage references before line projection. Unique issuer observations, no multiplication by common classes. |

Add at least the full 2.9 mnemonic map, with core priority AT/ACT/LCT/CHE/IVST/RECT/INVT/XPP/PPENT/PPEGT/LT/DLC/DLTT/SEQ/CEQ/PSTK/MIB/AP/XACC/DRC/SALE/COGS/XSGA/XRD/DP/OIADP/XINT/TXT/IB/NI/OANCF/CAPX/DVC/PRSTKC/SSTK/CSHO. Include **RE** (retained earnings), **WAB** (quarter weighted basic shares), and cash-flow working-capital components needed by this tranche. No equivalence of WAB and CSHO. Preserve all other planned 2.9 mnemonics for later waves; unsupported maps emit NULL and an explicit disposition.

Resolve canonical codes and aliases through `item_registry`, `statement_map`, and `standardization_rules` seed readers, pinned by hash. Use period-valid alias windows and the same sign/scale rules; do not independently maintain a new unversioned US-GAAP synonym list. `canonical_rules` may supply small mapping metadata, but never route bulk data through its pandas benchmark. FSDS custom tags remain raw candidates unless a separately approved, issuer-specific dated mapping exists; PRE label similarity does not establish semantic identity.

## 4. Point-in-time and fiscal-period rules

### Clocks, source precedence and null revisions

1. Pin the snapshot date to 2026-09-20. Admit no future filing or post-snapshot fact. Preserve retrieval and archive dates separately from historical availability; these files reconstruct filing history and are not proof of the complete source feed as delivered at the time.
2. Normalize accession format and CIK, then join FSDS NUM to SUB on accession with CIK consistency. Use `accepted_utc`, already converted from America/New_York; never reinterpret `accepted_et_naive` as UTC. Unknown clock, missing SUB, ambiguous/nonexistent local time without resolution, or conflicting SUB rows quarantines the candidate. X.4 owns source clock repair.
3. FSDS source values use the measured filing acceptance clock. CF-R rows may use that clock **only when the same accession, CIK, concept/taxonomy, unit, end, duration and value are corroborated by FSDS**; that is one fact with two provenance records. A matching accession alone proves the filing existed, not that a CF-R-only fact was present. Uncorroborated CF-R values use `max(raw_available_at, filed_date at 00:00 UTC + 46h)` and carry `cf_fc1_reconstructed`; the extractor's raw stamp is only +22h. Null/nonfinite filing dates yield no effective clock.
4. Unified basis is `fsds_acceptance_cf_fc1_v1`. Mixed-source formulas expose the maximum operand clock and every input basis. Keep an FC1-only *value parity* view for later B1 comparison. Do not run alternate-clock returns unless those configurations are separately preregistered and counted.
5. Partition facts by exact economic period and unit. Select a revision by descending effective availability, then filed date, accession and deterministic source row identity. Deduplicate exact corroborated rows; within the same filing prefer the exact FSDS value over its equal CF copy. Conflicting equal-ranked values become an explicit NULL conflict vintage and a blocking discrepancy, not an arbitrary winner. Later revisions outrank source preference. Newest NULL is authoritative; no `arg_max(value,...)`, `coalesce` to an older vintage, or `is_latest_revision` filter.
6. A later filing's comparative value is a new vintage **at that later filing's clock**, never backdated to the comparative period. An amendment without this item does not erase an earlier item; an explicitly null/invalid latest candidate does. Derived rows recompute at every relevant input event, including later NULL operands. Their clock is max(input clocks), their end is the economic period, and their `valid_until` is the next event for that exact derived key. Half-open validity is `[available_at, valid_until)`; readers use the newest eligible vintage.
7. At eom, compute formation session using `expected_month_end_session`; cutoff is that session's 22:00 UTC. Require all item, share, price, link, tier and classification inputs visible by cutoff. Weekend/month-end filings after the last session are excluded. Entry is close of `next_session(formation_session)`. The month-end calendar key must not add weekend information.

### Period construction

FSDS has end date and `qtrs`, but not the duration start. CF-R supplies start/end; `fy`, `fp`, and `frame` identify reporting context and are not sufficient labels for comparative periods. Determine a fiscal grid per CIK from the then-visible filings and periods, preserving non-December and 52/53-week years. No calendar-year `quarter()` assignment, no fiscal grid inferred from a later filing, no fiscal-year-change bridging.

- **Stocks:** qtrs=0/instant and a stock item only. Carry the end-date balance without summing; require a proven prior quarter/year endpoint for ratios and differences. Share counts need their own split/measurement basis. Do not confuse DEI cover-page share dates with fiscal balance dates.
- **Flows:** qtrs=1/2/3/4 is a duration, never four interchangeable quarterly observations. Direct qtrs=1 flows are q; qtrs=2/3 with a proven fiscal-year start are YTD; qtrs=4 with a proven full-year start/end is FY. Preserve YTD and FY separately. Reject duplicate dimensional/co-registrant contexts (`segments` or `coreg` present), abstract tags, disallowed forms, mismatched unit/currency, future ends, and unsupported durations with counted reasons.
- **Starts:** use a same-filing CF-R start only when exact fact corroboration holds; otherwise use then-visible fiscal endpoints and filing context to derive a start, labeled `fiscal_grid_inferred`. Require uniqueness and contiguous fiscal intervals. Unproved/transition periods remain stored with `period_ambiguous` and cannot supply lag/TTM operations. FY mapping of a comparative fact comes from its own end/start, never blindly SUB.fy/fp.
- **Discrete q:** use a direct single-quarter fact when available. Else q1=YTD1; q2=YTD2-YTD1; q3=YTD3-YTD2; q4=FY-YTD3. Both operands must be as-of eligible, same unit/mapping, same fiscal start, and adjacent proven endpoints; clock=max operand clocks. Same-filing comparatives take priority within a coherent revision, but previous-filing operands may be used with explicit lineage. Never FY/4. When direct q and subtraction coexist, reconcile them and quarantine unresolved conflicts.
- **Weighted shares, EPS, margins and averages:** not additive flows; no YTD subtraction or FY-minus-9m weighted shares. WAB q4 stays missing unless directly reported. Do not derive EPS by summing or dividing an annual value. Existing share-issuance caveats remain.
- **TTM:** exactly four contiguous discrete fiscal quarters, or one exact full-year flow at that fiscal endpoint. A new partial-year endpoint requires four proven quarters, or the algebraically equivalent current-YTD + prior-FY - prior-same-YTD with coherent starts and complete lineage. No missing-quarter zero fill, double-counted annual plus YTD, or stale annual relabeled TTM. Use the maximum operand clock; a restated component creates a new TTM vintage.
- **Lag/staleness:** q l1/l4/l12 address prior fiscal slots, including explicit gaps; FY l4 is prior FY. Pin tolerances in period-policy metadata and audit 52/53-week exceptions. Reuse `research/panel.py` defaults: 200 days for quarterly/instant current anchors and 400 days for annual-fallback/annual-dependency origins. Match `fundamental_signal_research.stage_selected_states` age semantics (selected fiscal endpoint and newest input endpoint), without applying current-anchor age limits to intentional historical lag operands. Missing refresh is `stale_input`, never an indefinite forward fill. Record the defaults and origin classification in the build digest; any alternate limit is declared before returns.
- **Missingness:** no unbounded `COALESCE(0)`. Missing XRD may be zero solely for the explicit 2.9/JKP convention with a flag and then-visible policy evidence. Debt/payout/inventory presence decisions use only the historical prefix at cutoff, never whether the issuer ever reports a tag in the full snapshot. Issuance/repurchases/dividends require explicit components in each quarter or a valid FY alternative. Zero is an observed or explicitly justified value, not non-reporting.

## 5. Contract to 3.3: identity rehearsal, without a warehouse copy

The 3.3 owner publishes `data/research/identity_rehearsal/<build_id>/manifest.json`, with `rehearsal=true`, source/code hashes, snapshot date, build scope, permanent-ID allocation version, evidence clock definitions, row counts, schema versions, and per-file SHA/bytes. Files are sliced exports from its retained/scratch inputs; needed production inputs are requested from OPS as short HEAVY read-only Parquet exports. Neither F.1 nor F.2 opens production or creates a full DB copy.

Required Parquet datasets preserve 3.3's schemas from the S3 plan:

- `security_permanent_ids`: `perm_security_id,security_id,first_trade_date,last_trade_date,created_at`; `security_id` must map exactly to `TBLTICKERHISTORY-<vendor id>` (or provide a separate explicit bijective crosswalk `security_id,line_id`).
- `company_permanent_ids`: `perm_company_id,cik,basis,created_at` with 10-digit CIK text and no recycled IDs.
- `security_company_links`: `perm_security_id,perm_company_id,cik,link_start,link_end,link_basis,link_primary,tier,available_at,evidence_ids,created_at`. Define end dates as inclusive business validity; open NULL end is unbounded. Revisions may additionally carry `valid_until` for evidence-version validity; distinct from `link_end`.
- `security_names_history`: `perm_security_id,ticker,issuer_name,venue,valid_from,valid_to,source,available_at`. Names/venue intervals likewise inclusive; no current name/venue propagated across historical changes.
- `link_evidence` extension: one row per `evidence_id` with source file/hash, assertion, effective interval, `available_at`, observed/retrieval timestamp, evidence kind, and any superseded ID. Export tier-changing evidence events, not just the final highest tier. Include security/common-class evidence and its clock; no preferred/warrant/unit admission from a substring match alone.

F.1 validates every evidence ID exists and `link.available_at >= max` of the evidence needed for that assertion/tier. At formation the line must be in its interval, evidence known at cutoff, and tier computed at cutoff. Future corroboration cannot raise a historical tier. `current_ticker_verified` only supplies its justified dated interval; it never backfills 2013 from a 2026 snapshot. Reconstructed links stay `reconstructed_high`/`reconstructed_medium`; strict requires dated identifier history and the strict universe. Effective date and evidence availability are different clocks. Retrospective linkage with no historical availability is reported separately as unavailable for the primary PIT projection, never silently clocked to the first trade.

Project on eligible common lines P/J only; N never contributes. All price-universe lines remain in the dense coverage audit, including unlinked or ambiguous rows. Rank/evaluate one deterministic **primary common line P per owner per formation** to avoid treating classes as independent issuers; J rows remain counted as nonprimary in audit and in company-ME construction. An ambiguous primary or multi-owner line is quarantined rather than selected by current liquidity. Primary designation must be point-in-time, not selected using later returns.

Company ME is the sum over all then-valid P/J common classes using the pinned formation-session `me_line`; require complete eligible class coverage. Do not substitute the primary line's ME for company ME. Rehearsal ME based on 1.12 vendor shares remains `vendor_shares_lag90_unverified`; it is suitable only for a labeled research result and may cause investable evidence to fail. Never label it verified DEI size or strict identity. Valuation output clock includes all class share/price/link clocks. Report all missing/unverified size failures without narrowing the denominator.

3.3 retains its acceptance: IDs for all 25,760 lines, >=88% operating-company link coverage, 50 dated delisted-name checks, 331 multi-line CIK reconciliation (recount and explain snapshot differences). F.1 adds monthly **as-of** coverage, because final retrospective link coverage does not prove historical usability. If as-of coverage is poor, improve the evidence in 3.3/X.6; do not backdate it to pass F.2.

## 6. Memory, slices, publication and restart

Every Python/probe/test runs through `run_memory_guarded.py`, `OPENBLAS_NUM_THREADS=1`. Orchestrator: 0.2 GiB with `--allow-nested-guards`; workers: 0.6 GiB, DuckDB 256MB/one thread at connect, private spill directory from `connect_bounded`. No HEAVY token for these lanes. No full-table pandas/Arrow materialization or large nonspilling list aggregates. `standardize_to_store` may receive one small complete formation only; raw/period stages stream Arrow.

Start with 128 stable CIK hash buckets. Normalize FSDS one filing quarter and bucket at a time, CF-R one extraction batch and bucket, reconcile revisions one owner bucket and item group, emit one fiscal year per sealed lake slice. Increase buckets if a slice exceeds 2M output rows or ~60 seconds; no cap increase. Include complete historical prefix within a bucket for vintage/lag construction rather than truncating it at a file/year boundary.

Each slice receipt pins code, mapping, period/clock policy, input manifests, bucket function/count, output rows/keys/hash and parent dependencies. Write temporary output, verify uniqueness/count/clock gates, atomically publish the output and completion receipt; restart verifies hashes and skips completed slices. A killed slice has no completed receipt. Build manifest publishes only after all expected slices and DQC gates pass. Rebuild identical inputs is a no-op; changed source/code writes a new build ID. No whole-stage transaction. Delete only owned scratch/spill after exact-path verification; preserve reusable sealed outputs. Check >=35 GiB free before >1 GiB writes; no duplicate large DB.

F.2 computes owner raw features by formation-year/bucket, then consolidates one full formation per feature for transforms. Splitting ranks by bucket is forbidden. Store output streams in `(eom,line_id)` order. `feature_sha` pins catalog row, formulas, map/clock/period/identity source hashes, lake slice index, spine and split/ME bases. Keep detailed missing reasons in a sidecar; translate to existing store `missing_input`, `insufficient_obs`, `stale_input`, `not_visible_at_cutoff`, `unverified_size` codes without silently adding incompatible reason semantics.

## 7. F.2 initial roster and domains

This is the pre-return requested roster, not a claim all rows will meet coverage. Existing IDs keep their seeded economic definitions and signs. New 2.10 rows have the definitions below. `Aavg=(AT_q+AT_q-4)/2`; unlabeled flows are TTM, current balances are at the latest proven quarter, and all operands are from the same as-of view. `ME` is complete company ME. CF outflows are positive magnitudes for payout/expense formulas after undoing the seed's signed-cash convention where required; `CAPX` is a positive expenditure magnitude, so FCF=OANCF-CAPX. Pin this conversion by mnemonic, not a blanket `abs` on arbitrary facts.

| Feature ID | Sign | Frozen construct / domain |
|---|---:|---|
| `share_issuance_1y` | - | log(WAB_q/WAB_q-4), same-quarter weighted-basic-share analogue; positive counts, same filing-restated comparison or proven split epochs; no annual-to-q4 inference. |
| `share_issuance_3y` | - | log(CSHO_q/CSHO_q-12), period-end shares on one proven split basis; 13 fiscal slots. No vendor-share substitution. |
| `net_equity_issuance` | - | (SSTK-PRSTKC)/Aavg; positive Aavg; each cash flow explicitly present for a complete trailing year. |
| `buyback_yield` | + | (PRSTKC-SSTK)/ME; ME>0, complete classes and cash-flow coverage. |
| `net_payout_yield` | + | (DVC+PRSTKC-SSTK)/ME; same domain and explicit dividend coverage. |
| `gross_profitability` | + | gross_profit_ttm/Aavg, using mapped gross profit or period-consistent SALE-COGS fallback; Aavg>0. Preserve this existing average-assets definition. |
| `op_at` | + | (SALE-COGS-XSGA+XRD)/AT, new 2.10 definition, AT>0; flagged missing-XRD-zero convention only. |
| `cop_at` | + | (SALE-COGS-XSGA+XRD - delta RECT - delta INVT - delta XPP + delta DRC + delta AP + delta XACC)/AT; deltas are four fiscal quarters; AT>0. No aggregate working-capital proxy silently replacing components. |
| `cfo_to_assets` | + | OANCF/Aavg, Aavg>0. |
| `fcf_to_assets` | + | (OANCF-CAPX)/Aavg, Aavg>0. |
| `roe_q` | + | NI_q/CEQ_q-1, CEQ_q-1>0 and opening quarter proven. Keep existing net-income analogue caveat; do not silently substitute IB. |
| `roe_q_change_yoy` | + | Existing seeded q-ROE minus its same-quarter prior-year value, with both opening equity domains satisfied. |
| `tax_gr1a` | + | (TXT_q-TXT_q-4)/AT_q-4, AT_q-4>0; quarterly ChTax analogue. This differs from existing TTM `tax_expense_change_yoy`; the latter is deferred, not relabeled. |
| `ebitda_to_ev` | + | EBITDA_ttm/EV; EV=ME+total debt+preferred+minority-cash/ST investments; EV>0. Exact seeded rollup and then-visible missingness policy; missing class ME is not zero. |
| `ebit_to_ev` | + | Seeded operating_income_ttm/EV with the same EV domain. Preserve this operating-income analogue; a new `ebit_best` construction would need a distinct declared row. |
| `cfo_to_price` | + | OANCF/ME; negative numerator remains the existing separate loss domain. |
| `fcf_yield` | + | (OANCF-CAPX)/ME; existing loss-domain behavior. |
| `noa_to_assets` | - | Seeded NOA / AT_q-4, where NOA=(AT-cash/ST investments)-(LT-total debt), AT_q-4>0. Do not substitute the different planned `sale_noa` construct. |
| `delta_noa` | - | (NOA_q-NOA_q-4)/AT_q-4; same domain. |
| `altman_z_book` | + | Seeded 1.2*(ACT-LCT)/AT+1.4*RE/AT+3.3*operating_income_ttm/AT+0.6*CEQ/LT+SALE/AT; positive scale denominators. |
| `altman_z` | + | Same seeded expression with ME/LT; labeled company-ME evidence. |
| `ohlson_o` | - | Seeded O-score and its full dependency closure from `derived_metric_definitions.csv`; no GNP deflator, log assets uses seeded dollar units. Preserve this documented analogue and domain. |
| `piotroski_f_cash_issuance` | + | All nine seeded terms, with explicit TTM cash-issuance indicator instead of split-sensitive share issuance; all nine required, else NULL. Zero issuance must be proven, not absent. |

`piotroski_f` share-count version is a predeclared deferred diagnostic until split evidence is complete; do not independently gate both versions by choosing the better one. `cash_profitability` is deferred in favor of `cop_at`, whose R&D convention is explicit. CHS, QMJ, composites, `compeqiss`, intangible capital and the remainder of 2.10/2.11 stay with later owners. Deferrals are scope entries, not failed hypotheses deleted after a return read.

SSTK/PRSTKC/DVC coverage is a key risk: never change population from `all` to only complete reporters to pass. Report numerator coverage against the original economic population. Fiscal-history starts come from input history and required lag counts, frozen before returns; first feasible dates are not chosen using IC or significance.

## 8. Registration, deduplication and grading

Policy remains `r4-qualification-v4`, semantic policy SHA `776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a`. Freeze its file SHA as an additional run artifact. No changes to split dates, thresholds, domains after inspection of returns, investable tests, BH, DSR, or allowed statuses.

Before registration, F.2 writes the full roster above and a **dedup ledger** in `research_fund_fast_preregistration.json`: feature ID, canonical hypothesis ID, expected sign, formula/map hash, previous wave, assigned wave, construction differences, availability basis, source hashes, first formation, status (`included`, `deferred`, or `unsupported`) and reason. Cross-check all registered wave rows and catalog IDs. `DUPLICATE_HYPOTHESES`/`blocked_duplicate_hypothesis` can express true alias rows, but never create a duplicate as a shortcut for different clocks. Controller approves the wave split and any pre-return unsupported finding before the registration artifact is committed.

The new roster includes 23 requested features. Required families stay visible even if coverage fails. A mathematically/source-unsupported row can be deferred pre-return with evidence; a low-coverage buildable row is registered and graded honestly. Do not automatically inherit w1's hard-coded unsupported list or first-history date. Set this wave's own preregistration and meta in the runner.

Register exact cells for `rank_normal`, `signed_raw`, and `zscore` at 1/3/6/12 months (12 per included feature; 276 for all 23), and any other actually evaluated configurations before reading labels. The primary remains policy `rank_normal`, 3 months. One declared reconstructed identity/clock/ME basis is the initial grading basis. Extra statistical clock/identity variants are not free retries: extend the pre-return cell/config design and registry accounting before evaluating them; numeric provenance parity without returns is not a trial.

`register_wave(... rows=..., preregistration=..., first_formations=...)` requires catalog rows whose `wave` matches. Commit its anchor, then verify that anchor and exact feature manifest, catalog, lake/spine/identity, label and evaluator code hashes before opening any label file. The current registry hashes row identity/clock/domain fields, but not full formula code: bind formula and input hashes additionally in the committed preregistration and feature manifest, and reject drift in the runner. Preserve 1.13's label-policy and artifact checks; remove its price-only assumptions by explicit wave configuration, not global defaults.

Use selection formations 2013-01..2023-12, with mature endpoints strictly before 2024-01-01. A 2023 formation's 6/12-month endpoint in 2024 is **sealed** for this run. Provisional selection labels inherit all terminal-pending exclusions and the known survivorship caveat; statuses stop at `selection_pass`, never `qualified_*`. Keep 2024 onward features constructible but never read their labels or aggregate holdout statistics during F.1/F.2.

The price wave's registration/trials remain in the cumulative registry. Report EWC IC/ICIR, primary and investable gates, EW/VW/capped-VW portfolios, 1/3/6/12 horizons, costs/capacity/spanning where 4.1 provides them, and the common ledger. Missing measured market/borrow inputs remain labeled, with no fabricated capacity or short financing estimates.

Later 2.9 validates the FC1 parity adapter against B1 on 50 owners x 12 formations; FSDS acceptance-clock values are compared at equivalent clocks, not asserted equal at different cutoffs. Later 2.10 excludes these canonical hypothesis IDs from `w2_fund_a`; 2.11 cannot append to an already frozen wave and needs a disjoint registered residual wave if its scores arrive later. 3.10/3.11 can rematerialize the same IDs from improved sources; changed measurements are reproducibility/sensitivity outputs. They do not silently replace the frozen F.2 selection artifact or earn a new independent qualification. If source/method changes affect the tested construct, controller records a new counted version/configuration and retains the original result. At 4.3, each canonical hypothesis enters qualification once with its complete lineage; there is no separate w0 and fast-wave vote.

Holdout opens only after final labels are complete and pinned, external/final evidence requirements are satisfied, and `TrialRegistry.open_holdout(wave, final_labels=True, label_sha=..., labels_root=...)` records the single opening. Commit the anchor before the read. A later reimplementation does not buy a second holdout.

## 9. Real-data acceptance runs and evidence

These commands are the **proposed CLI contract**, not claims that the scripts already exist. Run from `C:/atx/atx-db` with `OPENBLAS_NUM_THREADS=1`. Workers use fresh exports where required by CONTEXT. Replace manifest/snapshot pins with the exact accepted artifacts; never a latest-path default.

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 --receipt ../.superpowers/sdd/tier1-v2/receipts/F.1/build.json -- .venv/Scripts/python.exe scripts/research_fundamentals.py build --run f1-v1 --fsds-manifest <v2-manifest> --cf-manifest <cf-v2-manifest> --snapshot-date 2026-09-20 --buckets 128
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 30 --receipt ../.superpowers/sdd/tier1-v2/receipts/F.1/audit.json -- .venv/Scripts/python.exe scripts/research_fundamentals.py audit --run f1-v1 --identity-manifest <3.3-manifest> --spine-snapshot <1.12-snapshot> --formations 2013-01..2023-12
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 30 --receipt ../.superpowers/sdd/tier1-v2/receipts/F.2/build.json -- .venv/Scripts/python.exe scripts/research_fundamental_wave.py build --run f2-v1 --items-manifest <F.1-manifest> --identity-manifest <3.3-manifest> --spine-snapshot <1.12-snapshot> --wave w2_fund_fast --formations 2013-01..2023-12
```

The audit CLI must implement streaming bounded subpasses or accept `--bucket/--year` if the one-worker audit exceeds 0.6 GiB. Registration/evaluation use the inspected existing `scripts/research_run_wave.py register|run --wave ...` interface after its new wave configuration exists; pin its `--manifest-sha`, `--catalog-digest`, `--lake-snapshot`, `--bench-snapshot`, `--label-sha`, `--formations 2013-01..2023-12`, `--run`, `--root`, and `--out`. `register --confirm` is an implementation action after the documented pre-return decision; this design performs none. The full exact run commands and all pins belong in the implementation reports.

| Gate / measurement | Required result |
|---|---|
| Source completeness | 69/69 declared FSDS quarters v2, 85/85 CF-R batches, all 20,390 dispositions, hashes verified; 0 mixed v1 files; missing/unrecoverable SEC orphan rows explicitly counted and excluded. |
| Numeric/source reconciliation | Stratified 10,000 overlapping FSDS/CF-R cells across early/middle/late years, sectors, small/delisted issuers and forms: >=95% mapped, >=98% mapped within 0.5%; report exact-value equality separately. A=L+E >=99% of eligible issuer-periods with all needed components; never erase failures to pass. These are fast-path gates, not a substitute for 2.8's warehouse benchmark. |
| Period coverage | Count direct q, derived q, FY, YTD and TTM by year/item; 0 overlapping or gapped admitted TTM; direct-vs-derived quarter mismatches explained. Hand-audit >=50 issuer-periods spanning 52/53-week, non-December, amendments, annual q4 and fiscal transitions. |
| PIT | 0 values/links/tiers with an input clock after cutoff; 0 future-filing backfills; 0 duplicate publish keys; 0 newest-NULL resurrected old values. Audit every input for one sampled formation per year 2013..2023 and 50 owners x 12 formations against a slow raw-Parquet oracle. |
| Clock boundary | Real filings on both sides of 22:00 UTC, EST/EDT, month-end weekend/holiday, amendments and NULL replacements: exact expected eligibility and next XNYS entry; retain accession-level proof. If rare cases are absent from the source, say so and add one narrowly scoped clock fixture. |
| Identity | Monthly owner/line coverage split by link_basis/tier, pre/post-cutoff evidence, delisted status, P/J/N and ambiguity; 0 non-common admitted, 0 multiple primary observations per owner, 0 unproven historical upgrades. Reconcile all eligible multi-class company-ME sums, missing classes and unverified shares. |
| Coverage | Core fundamentals target >=3,000 owners/month, preserving the index target and reporting every deficit. Conditional definitions >=60% of their original population. Policy requires >=60% population coverage in >=90% history-eligible selection formations; >=36 formations. Do not reduce thresholds or denominators. Distress/score components and all nine Piotroski terms reported separately. |
| Resume/reproducibility | Kill one owned real-data slice before publication, resume, then compare logical key/value digests and sorted Parquet hashes to uninterrupted same-input output; no duplicates, committed slices skipped, incomplete snapshot unreadable. Report bytes, elapsed time, maximum slice rows and guard native peak/cap_hit. |
| F.2 integration | Store schema/clock/transform checks pass; dense denominator reconciles to pinned primary-line population plus counted exclusions. Registered cells equal evaluated cells, row/anchor/policy hashes match, holdout-open events remain zero for this wave. Report every feature's status, including failures; no success claim from coverage alone. |

No TDD. Real-data measurements are the primary evidence. Add tests only for the allowed PIT boundary, kill/resume or one multi-stage end-to-end check; run only touched files once each under guard with `-q -p no:cacheprovider -n 0`. This design task launches no Python, tests, warehouse, or returns. Implementation reports must update `## Progress (session 5)` immediately after each run/stage/commit and archive receipts with native peaks.

## 10. Sequencing and remaining rulings

1. Controller ratifies `w2_fund_fast` and dedup/clock/coverage scope, dispatches X.4 and 1.4 completion, and passes section 5 to 3.3. Design acceptance does not certify those inputs.
2. F.1 implements source validation, mapping, event vintages, periods and bounded build; identity-independent raw/item slices proceed in parallel with 3.3's rehearsal. Finish the real audit after identity/spine arrive, then review F.1.
3. F.2 takes CAT and runner ownership, builds raw features and coverage before registration, resolves only pre-return unsupported constructs, commits the frozen catalog/preregistration and anchor, then grades selection under EVAL while its code is frozen. Review includes trial/dedup and no-lookahead proof.
4. 2.9/2.10 consume these modules/artifacts under the handoff rules; final sources and labels later supply their own required parity/final checks. Strict evidence and release eligibility remain bounded by identity/source quality.

Principal risks are insufficient historical link clocks, incomplete payout tags, unproven fiscal duration starts, unavailable split evidence, unverified company ME, and the incomplete staging builds. Each produces an explicit quality/coverage failure or deferred construct, never a replacement historical fact or a hidden universe restriction.

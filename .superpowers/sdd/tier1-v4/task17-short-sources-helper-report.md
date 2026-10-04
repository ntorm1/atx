# Task17 R26 short-source helper

Same Task17 whole-task repair2/5. This is the disjoint original-family dependency
authorized by r26-short-source-helper-brief.md and the controller's preparation
projection ruling; no new task/counter, frozen producer repair or source waiver.
Own only private_tier1_short_sources.py, its direct test and this report.

## Implemented interface

VERSION=tier1-short-sources-v1; fixed names short_interest/short_volume_ext.
admit(name,spec,con) fully replays original raw sources, original native outputs
and separately immutable mapped outputs. materialize(name,spec,con) is root-slot
only for actual sources and publishes private immutable mapped products without
changing a native stage, scratch cache, activation or source. iter_rows/consuming
check complete actual source/code/product dependencies before and finally after
bounded consumption. The connection must be file-backed128MB/one thread/UTC,
external guard<=1GiB and40GiB free disk. Tests use0.6GiB external guards.

Return status=admitted_diagnostic, complete native products plus mapped products,
native validation, dependency_pins, schemas, source-derived view/key/clock maps,
family/spec and source_quality_certified=False. Original SI products remain
si.parquet/mapping_audit.parquet; ext remains
year=N/short_volume_ext.parquet for every complete selected native year.
Additional immutable products are short_interest_cells.parquet and
year=N/short_volume_ext_cells.parquet. They retain every original mapped primitive
and add qualified/qualification_reason/source_vintage_proven/source_sha256. SI
adds available_at TIMESTAMP UTC and native_first_visible_session DATE. Native
mapping_audit retains the complete population after original duplicate resolution;
original raw files and duplicate rows are fully replayed rather than substituted.

SI includes settlement/dissemination dates, shares/previous shares/FINRA DTC/ADV,
revision/split/market class, exact/canonical/repair/fallback flags, vendor dates and
source_file. No shares-over-NULL ratio is invented; a short_interest_ratio still
requires Task17's separately admitted dated denominator. Ext retains short/exempt/
total volume, all native B/Q/N/D/other facility values and native vintage_risk=True.
No generic score or caller supplied qualified Boolean is an input.

## Independent source and publication contracts

Common spec: interface_version/root/generation/manifest_pin/native
{plan_path,expected_plan_sha256}, independent_review{path,expected_sha256},
complete externally supplied dependency_pins and expected_validation_sha256.
mapped_generation is separate; public admission additionally requires independently
expected mapped_manifest_pin and mapped_product_pins. Those output expectations
are not derived from a candidate assertion during admission. The independent
review record must exactly bind fixed family/version/source-plan/native generation
and current complete code identity; status alone cannot satisfy it. Root must
create actual expectations through independent preflight/review, not authorize
candidate-generated source inventories.

Plan schema atx.tier1-short-source-plan/v1 binds the original ticker_history,
native ticker_manifest lineage, original ticker_source_url, complete original
clock_inventory, expected_raw_files/expected_selected_files/expected_sessions,
exact native_products pins and complete expected_native_rows. SI adds original
old_manifest/new_manifest, old_raw_root/new_raw_root, official schedule and its
original source URL. Ext adds complete source_manifest/raw_root/start/end and
expected_dates. Complete old/new manifests, successful files including later
duplicate landings not selected economically, original clocks and native products
are derived and compared with the independently expected complete pin inventory.
Missing source clocks are explicit NULL inventory entries, not omitted files.
Native old-landing precedence and original last-manifest-row date semantics remain.

Actual native ticker file lineage must match both independently pinned native
ticker_manifest and product manifest path/bytes/mtime identity; full source payload
SHA is independently pinned and original observation receipt hashes must match.
SI rule and complete selected source hashes are checked against its original
manifest. Ext original schema/status/stage/mapping/clock, complete published
product inventory and producer code hashes are checked. Complete typed Parquet
rowsets use EXCEPT ALL in both directions; neither duplicates nor denominators
are removed to obtain agreement. Complete physical producer/helper/shared code,
DuckDB/PyArrow binaries, runtime versions, stable loaded code fields and native
mapping/calendar/repair constants are bound and rechecked.

Original observation inventory schema atx.finra-short-original-observations/v1
has files{absolute original raw path:original receipt path or NULL},
ticker_receipt and schedule_receipt. Each independently expected original receipt
must authenticate actual HTTP200/official raw URL/raw SHA/downloaded_at with an
offset. It is an original contemporaneous capture document, not a new PASS
summary. No later download is backdated. FINRA raw URLs are fixed native CDN/API
URLs; vendor and official schedule source identities require independent original
source review. An official dictionary or modern schedule document alone is not
local historical-byte/vintage evidence. These receipts, full clock inventory,
all native source manifests and every consumed payload are physically bound.

## Original kernels and minimal authorized preparation

Unchanged SI._stage_sql (short_interest.py:362-438) and SV.map_year
(short_volume.py:163-230) remain the original mapping/duplicate/collision authority.
SV.parse_file (89-115) runs one bounded daily original file at a time. The ext
published SQL (short_volume_ext.py:118-134) is preserved exactly, including its
whitespace behavior; parse_market differs on whitespace and does not replace
published native SQL semantics. Original SI ADV0 produces DTC=NULL.

The controller authorized only these preparation projections because existing
entrypoints mutate fixed scratch/cache or read global source paths:

* SI raw parser217-260: same source ordering/types/delimiters/invalid-row handling,
  source_file/landing/line_no; streaming Arrow CSV batches replace whole-population
  collection and no old parsed cache is deleted.
* SI settlement281-298: independently bound schedule; unchanged add_business_days,
  publication-after-settlement requirement and seven-calendar-day mapping window.
* FINRA sid0 SQL233-280 and ticker SQL308-322: exact native source-bound SQL;
  complete bracket/ambiguity/own-row exclusions retained. Future bracket repair
  remains visible diagnostically and is not treated as PIT source qualification.
* SI as-of313-354: exact primary/need/window/latest-date fallback SQL and original
  overlapping fallback-window refusal, with explicit independent source paths.
* Native SI mapped output467-476 and ext published projection118-134: only the
  original output projection; no economic formula or whole producer is cloned.

No frozen function is patched, resource/global setting overridden or producer
invoked. Isolated private replay directories are retained. Individual raw files
have a16MiB decompressed bound; oversized originals explicitly refuse full
admission, rather than truncate or shorten the required population.

## Clocks and conditional qualification

SI available_at is00UTC of the first native vendor session strictly after official
dissemination_date. Native rule-7bd fallback without official source evidence,
unknown next session, revision/split exceptions, unresolved source clocks and
ambiguous/unmapped/future-bracket repaired rows remain excluded. The native
first-visible-session boundary remains explicit. Task17 confirmed GC1 continues
strict prior-calendar-day22UTC, so native Jan7 visibility can enter golden Jan8;
there is no intraday backdating to eliminate that consumer latency.

Ext retains native T+1 00UTC and vintage_risk=True. Separately derived
source_vintage_proven means independently authenticated contemporaneous original
bytes were actually observed by that unchanged source clock. It does not rewrite
the native flag, claim first-ever publication, prove no revisions before capture
or infer a historical clock from general T+1 documentation. Qualified requires
that capture, independently observed dated ticker mapping, valid original finite
nonnegative quantity basis and native mapping. Exempt>short or short>total remains
diagnostic/excluded. Original parsing/population exceptions remain visible.

Actual2026 capture receipts cannot qualify historical2018-2022 bytes. The
conditional path is reachable on source-shaped original fixtures while actual
historical receipt/vintage evidence remains absent. Source qualification is not
financial/common-stock/universe/currency/share-basis/rights/publication acceptance.
Those independent original gates and consumer seals remain mandatory. Global
source_quality_certified=False never changes.

## Focused offline evidence

All commands ran from C:/atx with PYTHONPATH=C:/atx/atx-db/src and
OPENBLAS_NUM_THREADS=1, using:

```text
atx-db/.venv/Scripts/python.exe .superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 1 --disk-path C:/atx --min-free-disk-gb 40 -- atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_private_tier1_short_sources.py -k SELECTION -n 0 -o addopts='' -p no:cacheprovider -q
```

1. repair_ticker or schedule_and_interest:2PASS1.28s, guard3.0s,
   peak120078336B, receipt20261004T154606-12520.json. Original frozen SQL
   expressions are read-only fixture oracles; full typed duplicate-preserving
   repair/ticker equivalence and original CSV conversion/invalid-row equivalence
   pass. Bracket/ambiguous/missing/duplicate sid0 reasons remain exact.
2. Initial full_ext or later_capture:2PASS10.80s, guard13.0s,
   peak156467200B, receipt20261004T154620-22576.json. Complete original CNMS
   parser/native published SQL/mapping and private publication/readmission;
   supported positive original captures, unmapped exclusions, native vintage flag,
   later capture/NULL clocks and consumed actual raw drift refusal.
3. full_interest:1PASS14.55s, guard17.1s, peak145006592B,
   receipt20261004T155218-7792.json. Original native as-of SQL oracle agrees;
   complete old raw/schedule/ticker/full native outputs and immutable mapped
   public replay; exchange-before-OTC duplicate resolution, prior-session fallback,
   ADV0 DTC=NULL, explicit first-visible-session clock and positive original
   fixture capture qualification pass.
4. Changed final full_ext or later_capture:2PASS12.10s, guard14.0s,
   peak157499392B, receipt20261004T155500-17624.json. Covers added strict native
   manifest/ticker/schema/code/full inventory lineage, binary/runtime pins and
   separate source-vintage diagnostic derivation. Both new files AST PASS.

All guards exit0, no cap event, every invocation<60s and file-backed128MB/one
thread. No fixture failures occurred. Earlier passes are not mislabeled final
covering runs. These are synthetic independently constructed source-shaped
expectations, not actual source review evidence. No native function monkeypatch,
whole producer clone, warehouse/global source read or source gate promotion was
used. Fixture lane returned to root/Task15; no test remains live. No cleanup or
temporary-directory inventory was performed.

## Remaining actual dependencies and integration

Root needs complete real raw source/receipt inventories, reviewed immutable native
outputs, original official dissemination and dated vendor source evidence, actual
historical contemporaneous captures, complete populations/coverage/legacy comparison,
source rights and all original handchecks/gates. FINRA's lack of revision history
and later landed vendor snapshots remain genuine proof limitations, not a reader
mapping implementation gap or a waiver. This report does not claim scientific,
consumer or source-family completion. Task17 owns native serving/registry/golden
field wiring and separately admitted share denominators. Independent combined
R26 review must precede actual materialization/admission. R21 permits later
historical predictors; complete labels and every evaluation remain strictly
pre2023. No actual data/hash/metadata/HTTP/return/consumer job ran here.

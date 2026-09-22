# Historical panel identity and provenance audit — 2026-09-19

This audit records the design and source findings before checkpoint 4. The
artifact wrapper, exact axes, ingestion provenance, and pipeline joins have since
been implemented and measured; see the
[checkpoint record](../docs/PLATFORM_PROGRESS.md#checkpoint-4-identified-artifacts-across-the-native-pipeline)
and [validation receipt](2026-09-19-panel-identity-validation.json).
Availability semantics, reference eligibility, complete experiment lineage, and
artifact-aware incremental append remain open. The findings below describe the
pre-implementation state unless explicitly identified as measurement.

## Where identity is lost

| Existing call site | Available information and current loss |
| --- | --- |
| `atx-engine/src/alpha/segment_panel.cpp::attach_multi_segment_panel` | Builds `date_axis`, `inst_names`, and source readers, then returns only `Panel::create(...)`. Instrument order is first seen across selected source rows, **not** sorted numeric ID order. |
| `atx-engine/src/data/history_panel.cpp::build_history_panel` | Receives only dimensions/values. Compaction applies `keep[]` to all fields and the mask, discarding that mapping. `HistoryPanel` retains only panel, numeric digest, and lineage names. |
| `atx-impl/src/serialize_panel.cpp::{write_panel,read_panel}` | APNL v1 stores shape, field names, values, mask, and FNV trailer; neither axis nor input identity is serialized. |
| `atx-impl/src/stage_panel.cpp::run_panel` | `.meta.txt` is informational, has wall-clock time and a partial recipe, and is not bound to source bytes. It omits actual axes and the compacted column mapping. |
| `atx-impl/src/{stage_optimize,stage_metabook}.cpp` | Research/combo compatibility checks compare dimensions. Different dates or permuted security axes with equal dimensions pass. `stage_report.cpp` similarly consumes positional books and period metadata. |
| `atx-impl/src/append_history_panel.cpp` | Infers the existing last date from the combined source's row count, then checks numeric prefix equality. Equal values do not prove equal instrument/date identities. |

`data::digest_panel` hashes fields through `SignalSet`; it does not identify axes
or the universe mask. The APNL trailer includes the mask but still lacks axes.
Neither is a complete dataset identity. Also, `run_panel` computes `hp.digest`
before optional field augmentation; provenance must bind the final serialized
bytes, not mistake that pre-augmentation digest for the output artifact hash.

`Dataset` already has typed date axes and availability lookup, but its `InstKey`
is `u32`; the archive contains IDs as large as **1,001,001,001,070**. Do not narrow
vendor IDs or cast them through floating point. Its free-text
`DatasetProvenance` and boolean `AsOfPolicy` do not supply revision history.

## Smallest compatible extension

1. Add an indexed attachment result beside `Panel`: owned `panel`, signed i64
   `session_keys`, ordered symbol strings, and selected source descriptors.
   Refactor the existing attachment implementation to produce it; retain the
   old `attach_multi_segment_panel` as a wrapper that returns its numeric panel.
   Read axes from `SegmentReader::times()` and `symbol_name()`, never filenames
   or `_symbology.parquet` order. Validate actual timestamp ordering and duplicate
   source cells; the current implementation assumes chronological filenames and
   can overwrite overlapping segments.
2. Carry the axes through `HistoryPanel` and apply exactly the existing `keep[]`
   to the instrument axis when compacting. Augmentation preserves axes. At the
   history boundary, validate canonical positive i64 IDs and label their namespace
   `spiderrock.securityID`. Dense engine indices remain local indices into this
   mapping. Ticker strings, especially `todayTicker`, are display metadata.
3. Introduce artifact IO beside `serialize_panel.{hpp,cpp}`: a wrapper containing
   the numeric panel, validated identity, and provenance. Keep `read_panel` and
   `write_panel` for legacy/synthetic consumers. The historical artifact reader
   requires the manifest and verifies it against the APNL bytes before returning.
   A bare legacy APNL remains inspectable but has **unknown identity**, not an
   inferred or certified one. No APNL v2 migration is necessary initially.
4. Have `stage_panel` publish the final panel and manifest in a fresh output
   bundle, with temporary files and the manifest published last. A missing or
   mismatched manifest means incomplete output. Preserve the existing plain-text
   receipt for people; keep wall-clock time and local paths outside deterministic
   artifact identity. Do not silently overwrite an already published bundle.

Suggested manifest contract (`atx.panel-artifact`, schema version 1):

| Group | Required contents |
| --- | --- |
| Numeric payload | APNL version, dimensions, ordered field names, SHA256 of the complete panel file; legacy FNV digest explicitly labeled |
| Axes | Actual ordered session keys, explicit `UnixNanoseconds` encoding with **session-date label** semantics, ordered positive security IDs and namespace, compaction policy and selected original column indices |
| Sources | Original archive SHA256/member identity; accepted/prepared archive SHA256; preparation script/config and QA report hashes; ordered selected segment hashes; source schema identifier |
| Transformation | Adjustment rule/version, raw/research field basis and units, NaN policy, complete resolved universe config including defaults, date window, compaction, augmentation/windows, loader key/duplicate policy |
| Producer | Executable SHA256, code revision plus dirty-source/patch digest when relevant; code revision alone does not identify this working tree |
| Availability | Snapshot status, observed acquisition time when known, documented delivery claim/source, explicit replay assumption and calendar/timezone version if used; unknown historical publication/revision times remain null |
| Eligibility | Source/type reference and version when present; instrument type, listing/primary-share status, and evidence status. Missing evidence stays unknown. Record the eligibility policy separately from numerical screens. |

Use a deterministic schema serialization and SHA256 artifact ID binding payload,
axes, recipe, and source hashes. Exact array order is significant. Store i64
nanoseconds and security IDs as decimal strings in JSON, avoiding binary64
rounding. [RFC 8785](https://www.rfc-editor.org/rfc/rfc8785) provides canonical
JSON rules and specifically recommends strings for integers beyond binary64
precision. Do not call an ad hoc JSON emitter JCS-compliant. An axis digest is
useful for joins, while the full artifact ID also binds the originating data and
recipe; equal axes alone do not establish common provenance.

This design uses W3C PROV's separation of source entities, processing activities,
and derivation without requiring a new graph service or RDF layer.
[W3C PROV overview](https://www.w3.org/TR/prov-overview/).
Arrow likewise distinguishes ordered typed schema fields from application
metadata; a future Arrow representation can preserve this contract rather than
invent another interpretation. [Arrow format](https://arrow.apache.org/docs/format/Columnar.html#schema-message).

## Availability and eligibility are separate contracts

SpiderRock documents `tradingDate` as a date, `securityID` as bigint, `todayTicker`
as the most recent ticker, and full-history publication at 05:00 CT T+1. That
current schedule supplies no historical release/revision timestamps for this
snapshot. [TickerHistory3 dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/).

Persist session labels independently from decision timestamps. For a later daily
replay, an explicit **assumed next-session-close availability** policy is a
conservative operational convention, not verified historical publication. Apply
it to features, liquidity/market-cap screens, sector membership, and eligibility
inputs used for decisions; shifting only the alpha leaves other information
unlagged. A row's close remains a historical return label. Resolving a session
requires a declared exchange calendar, and any use of 05:00 CT requires the
`America/Chicago` timezone rules. Missing source sessions require an explicit
gap policy, not silently treating the next observed row as the next session.
The current `Dataset::pit_delay` is calendar-day delay and cannot directly express
a next-session rule. Preserve `historical_vintages_verified=false` regardless of
the chosen lag.

The archive includes ETFs, rights, and test symbols. A positive key plus a raw
price/ADV mask does not establish a US common-stock long/short universe. GICS
presence, ticker suffixes, and current-name mappings are not sufficient type or
historical listing evidence. A later common-stock policy needs a dated security
reference with provenance; unknown classifications may be included in a labeled
data diagnostic, but must not be relabeled common stock. Identity preservation,
information availability, and trading eligibility must remain independently
inspectable.

## Consumer integration and acceptance gate

First ship attachment → `HistoryPanel` → `stage_panel` → validated artifact IO.
Then propagate that identity through `stage_combine` (same axes),
`stage_optimize`/`stage_metabook` (same instrument axis, schedule-selected dates),
and `stage_report` (exact parent artifact and schedule binding). `stage_discover`
and `stage_sweep` should record the source artifact ID even when they do not emit
a panel. Keep legacy numeric IO explicitly separate. Do not claim joins are
auditable while their call sites still check dimensions alone.

Six focused acceptance scenarios are sufficient for the initial extension:

1. Different per-day segment column orders, new IDs, and missing cells round-trip
   with exact axes and NaNs; a large i64 ID survives without narrowing.
2. Non-prefix compaction applies the same selection/order to values, mask, and
   identities; optional augmentation preserves that mapping.
3. Equal-shaped artifacts with swapped IDs or shifted dates are rejected by
   identified joins, even when all numeric cells are equal.
4. Modified payload/manifest, duplicate IDs, unsorted dates, mismatched lengths,
   unsupported schema, or absent manifest fail the identified reader. Original
   APNL v1 numerical round-trip/determinism tests remain valid.
5. Rebuilding identical inputs and recipe yields identical artifact identity
   despite a new receipt time/path. Changed source/QA/recipe hashes change the
   artifact ID even when the resulting numeric panel happens to be identical.
6. Availability across weekends/holidays/gaps and unknown instrument types remain
   explicit. No-op append retains identity; append checks actual old date/ID
   prefixes and source continuity instead of reconstructing identity from shape.

Relevant existing test homes: `atx-engine/tests/data/data_history_panel_test.cpp`,
`atx-engine/tests/alpha/alpha_multi_segment_panel_test.cpp`,
`atx-engine/tests/core/segment_panel_test.cpp`, and
`atx-impl/tests/{panel_test,append_panel_test,provenance_digest_test,optimize_test}.cpp`.

The current source-derived validator is an independent numerical oracle for one
declared archive and recipe: it reconstructs axes and compaction from accepted
source rows and records hashes under `build-equity/audits`. That evidence can seed
the first manifest and its acceptance fixture. It does not recover identities
from an arbitrary standalone APNL v1 file.

The completed `build-equity/audits/iteration3-panel-source-comparison.json`
reconstructs 31 session dates and 213 compacted IDs from 12,737 source IDs. The
independent validator reports exact agreement for all 6,603 mask cells (2,200
members), 46,151 finite price/economic values, and 70 NaN gaps. The panel SHA256 is
`4cf238d5ea396ead5cda5c5742f55ea93420346f82b56ada5d991c117eaaab6e`; the prepared
source SHA256 is
`907d877b4bbd11db6dc6d86bf5fc6cad92fc891fdadff16adfeb9ba6daa85c07`.

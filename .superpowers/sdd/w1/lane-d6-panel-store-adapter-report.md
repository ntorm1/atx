# W1 D6 panel-store stage and bounded consumer slice

Depends on core `98ad63d5` and review corrections `97a00229`. This is source-only
implementation followed by synthetic fixture additions; no C++ build/runtime,
market-data read, warehouse operation, acquisition or real artifact build occurred.
Original D6 RSS/rank-IC/overlap/causality acceptance remains unqualified.

## Actual producer and consumer paths

`run_panel` dispatches explicit `panel_storage_rule=mmap-f32-v2` to a chunked
producer. Root owns the new RunConfig field/parser; its default is the explicit
`legacy-f64-v1`, preserving APNL output and existing consumer behavior. V2 requires
an explicit pre-2020 window, a fresh output directory and dated common-stock-V2
membership. It refuses incremental mode, compaction and custom as-of fields whose
level-basis metadata is not yet expressible. Existing legacy custom-field handling
is unchanged.

The producer derives columns from the COMPLETE selected membership cut across its
bound artifact, not from the requested year or currently available source bars.
IDs are canonical sorted positive i64 values. Source-index namespace is explicitly
the complete membership union. A column absent throughout one output window still
exists. Each row's independent membership bit uses the last effective rebalance
with rank < session and effective <= session. Missing source rows do not erase
membership. There is no current-session re-ranking or price-based membership
override: economic/type eligibility comes from the bound D5 membership artifact.

`HistoryDataConfig::fixed_axis_ids` selects a new bounded assembly path. It maps
one intersecting source segment at a time, rejects malformed/unsealed metadata and duplicate
canonical identities/cells, and reserves the complete axis before copying fields.
The existing history path is unchanged when the fixed axis is absent. Source
presence is the returned panel mask on the fixed-axis path; a separate dated mask
is passed to the store. Data are assembled in 32-session chunks with strictly
earlier warm-up sessions sufficient for the requested ADV windows and one-day
returns. The same source-f64 transformation computes adjusted prices/returns;
only storage narrows selected fields. The store retains original-f64 adjusted
close and `returns` values, rather than reconstructing returns from f32 prices.

`read_panel_store_window` and `read_pipeline_store_window` materialize an explicitly
bounded date window. They return source-presence Panel data AND an independent
tradable vector plus field precision/basis descriptors. All arithmetic receives
f64; `close` is loaded from the original-f64 channel and other f32 fields widen.
The derived window identity binds the store manifest, selection and exact-close
read policy. Generic legacy pipeline reads and writes reject V2 inputs rather
than silently dropping separate membership. Migrating downstream strategy stages
to the explicit adapter is a later consumer step, not claimed complete here.

## Resource and provenance limits

The additive `SegmentReader::attach(path,max_bytes)` checks the captured handle's
extent through the reviewed Mapping overload before mapping. Its legacy API remains
unchanged. D6 source mappings are capped at 256 MiB each. Metadata bounds are 10,000
source paths, 100,000 session/ID entries and canonical finite field rules.
Source paths have an aggregate 8 MiB limit; the immutable source-axis index has
a conservative 16 MiB admission limit plus bounded 100,000-date inspection
scratch. A single index is reused across chunks; unrelated source payloads are
never mapped/CRC-checked per chunk.
Assembly admits the chunk/warm-up shape before whole-union numeric allocation:
2 GiB budget, 384 MiB reserved for mapping/writer/metadata, and conservative
512+24*ADV-window-count bytes per cell for overlapping transformations. The fixed
history builder separately bounds its concurrent copies and maximum source mapping.
These are explicit admission estimates, not measured peak RSS or speed claims.

The f64 window reader caps its materialized arrays and metadata before allocation;
mapped chunk pages have the core's separate shared mapping/handle limit. Large
research windows may be rejected rather than materialized unboundedly. Direct
chunk access is the intended large-artifact path.

The stage captures bounded source hashes, binds membership/source/executable and
all active transformation recipes, then rechecks the consumed source set before
publishing the final manifest. Session labels do not establish original market-data
availability; the archive-snapshot limitation stays explicit. A membership proof
is not a substitute for verified source-vintage data. No original data files are
rewritten. The stage digest is explicitly FNV1a64 of manifest-SHA ASCII, distinct
from the legacy numerical-panel digest.

## Prepared qualification and gaps

Seven core fixtures were prepared earlier. Two owning fixtures are added here:

- Fixed history axes retain absent/future columns and original f64 close, reject
  insufficient allocation/mapping budgets and incompatible compaction.
- A 36-session synthetic native stage builds two chunks from V2 membership,
  preserves an absent member, keeps a later entrant's warm-up, applies membership
  next session, reads an exact close/return across the chunk boundary, enforces the
  bounded reader and refuses legacy publication that would drop membership.

All nine new owning cases are uncompiled/unrun. No performance/precision evidence
is inferred from them. Root must add `panel_storage_rule` and the two new core TUs
to its next combined build; no CMake/config/ledger edits were made in this slice.
Independent source review, focused runtime, actual 2012-2019 t3000 artifact,
6,624-ID open RSS, f32 rank-IC tolerance, original per-year overlap comparison and
full causal qualification remain pending. Untagged custom as-of fields and implicit
legacy strategy consumption are deliberately refused until their separate-mask
and metadata contracts are wired.

## Adapter review corrections

Root and independent review identified a reopened-date `lower_bound(end)` cell
addressing bug and an O(chunks * all source bytes) scan. The corrected producer
captures one immutable `HistorySourceIndex` and passes it to every chunk. A
metadata-only handle read admits header/time-axis bounds before payload mapping;
all source names are checked first, rejecting explicit 2020+ years or mixed-era
range names. All axes must be positive, strictly increasing and before 2020.
This is a filename/axis seal check, not a claim of historic publication knowledge.

Only files with an actual session in the complete output window receive a full
CRC/SHA capture. Each chunk selects intersecting sources from the index, checks
the exact captured extent/time geometry before constructing borrowed time spans,
checks the complete time axis and SHA of the same reader mapping, then validates
every destination date by equality/end before any cell write. The lifetime-bound
`SegmentReader::mapped_bytes()`/`mapped_size()` accessors add no mapping ownership
and leave the legacy attach behavior unchanged.

Before publication the producer re-enumerates the complete path set and reads
bounded time metadata for every original file, including formerly disjoint
files. A new path or a source moved into the window fails. Consumed payloads and
ingestion/preparation receipts are revalidated by the existing final source
binding. The manifest remains last. Metadata scans occur once at capture and
once at publication; chunk work hashes only intersecting source payloads.

Three additional postimplementation synthetic cases cover an equal-sized,
valid-CRC source replacement moving a date beyond the captured axis; changed
prices under unchanged axes; corrupt disjoint payloads skipped by earlier
chunks; later actual consumption rejecting corruption; new/re-dated final source
entries; and named mixed-era refusal before attachment. **Twelve new owning D6
checks total are prepared, still uncompiled/unrun.** No scale timing or RSS claim
is made. Generic legacy history/panel behavior remains on its existing path.

# First annual context: existing builder memory review

The first declared context can use the existing history builder before a new streaming
builder is justified. The completed native ingestion has 445 dates, 7,650 source IDs,
and 2,883,147 accepted rows for [2012-03-26, 2014-01-01). This review reads code and
bound ingestion metadata; it does not inspect sealed-period returns or scan the ZIP.
Use a fresh standalone panel process, no augmentation or incremental mode, and an
isolated directory containing only this context's segments.

Let C = D*U, Q = D*K, and M = the sum of all segment file sizes in that directory.
The following are payload-byte counts, before allocator/runtime overhead.

| Phase | Live dense bytes | Evidence |
| --- | ---: | --- |
| Attach | max(136*C, 129*C+U) + M | `src/alpha/segment_panel.cpp:134-172` retains every reader; line 215 creates 16 columns plus one temporary column. |
| Corporate construction | 225*C | `src/data/history_panel.cpp:158-203`: raw panel 129*C, six source buffers 48*C, initializer-list copies 48*C. |
| Raw/corporate/universe retained | 198*C | Raw 129*C + corporate 48*C + cap/ADV/sector/mask 21*C. `src/data/universe.cpp:263-268`. |
| First compacted column | 295*C + 8*Q | Full twelve-field output and copied mask coexist with first replacement. Later replacements reduce this live total. `src/data/history_panel.cpp:310-350`. |
| Final digest | 198*C + 193*Q | `include/atx/engine/data/panel_digest.hpp:29-38` copies all twelve output columns while raw/corporate/universe remain alive. |
| APNL publication | about 194*Q | Final panel 97*Q plus writer buffer 97*Q; the builder has returned and its local arrays are destroyed. `../atx-impl/src/stage_panel.cpp:146,234` and `../atx-impl/src/serialize_panel.cpp:126-174`. |

`Panel::create` and `Dataset::create` move their owned buffers; the empty corporate
dataset mask stays empty. The corporate initializer-list backing elements are const,
so its vector elements copy despite the visible `std::move` expressions. This follows
the [C++ list-initialization rules](https://eel.is/c++draft/dcl.init.list).

Readers unmap at the attach function's return, before corporate/output assembly.
Microsoft documents that unmapping removes pages from the process working set.
This does not establish immediate heap-page release after ordinary vector destruction;
see [Windows working-set semantics](https://learn.microsoft.com/en-us/windows/win32/memory/working-set).

Use this explicitly conservative admission score with checked integer arithmetic:

```
max(137*D*U + M, 375*D*U + 290*D*K) + 512,000,000 <= 3,000,000,000
```

Before compaction is known, substitute K=U. The 359*C underlying allocation-volume
term counts all raw buffers, corporate copies, universe intermediates, and full output
buffers as retained. The 290*Q counts compacted columns/mask, digest copies and writer
buffer. Another 16*C conservatively allows every date's top-N index/sort request;
the installed MSVC stable_sort requests at most ceil(U/2) index elements per sort.
The 512 MB reserve covers dictionaries, source metadata, allocation overhead and runtime.
This is an admission policy with a measurement gate, not a hard allocator-independent
RSS proof. It deliberately does not assume freed large grid buffers are reused.

At D=445, U=7650, K=U, C=3,404,250: assembly's worst live dense peak is
1,331,061,750 bytes; publication's worst live dense peak is 660,424,500 bytes.
The conservative score is 2,775,826,250 bytes, provided the mapping term is smaller.
At D=445, its worst-case instrument cutoff is U<=8,407, also subject to M.
Measure working set, peak working set, private commit and peak commit through
[PROCESS_MEMORY_COUNTERS_EX](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex)
and stop only the child process on a 3 GB breach. Record measurements separately
from deterministic artifact identity.

## Minimal baseline adapter and remaining distinctions

The existing twelve fields retain unmasked numerical observations through `field_all`.
Borrow close/raw_close/volume and derive a byte observation mask from finite positive
prices and finite nonnegative volume. A persisted fourth double column is not needed
for this first in-memory adapter. The feature view must use that observation mask:
`Engine::evaluate` masks LoadField using the panel universe (`alpha/vm.hpp:581-595`).

Evaluate only the two frozen time-series/arithmetic expressions. Gate both signals
together by finite readiness, decision eligibility and evaluation window before
cross-sectional shaping. `extract_streams` creates a static full instrument Universe
(`alpha/streams.hpp:202-206`), so changing its panel mask alone does not enforce this.
Crop borrowed fields to evaluation rows, rebase exact session axes and decision indices,
and publish derived evaluation identity before optimize/replay; `fit_begin` alone does
not prevent warmup orders. Budget VM slots and signals separately after compilation.

Two differences from the proposed four-field streaming profile are confirmed:

- Existing compaction retains the union eligible anywhere in the entire context,
  including warmup. An evaluation-only union can be smaller. Extra stored names must
  never affect earlier time-series features or current eligible cross-sectional shaping.
- Existing membership evaluates shares*raw_close >= 0 even when min_mktcap_usd=0;
  NaN shares therefore exclude an otherwise price/ADV-qualified observation. This is
  not identical to a pure price/ADV rule. Preserve and declare this screen, or explicitly
  validate the selected source makes it immaterial; do not silently claim parity.

The panel remains a numerical archive context with unknown historical vintages and
instrument-type eligibility. Neither this memory review nor successful construction
authenticates cumulative factors or admits a verified investment return dataset.

Native execution evidence will be recorded in
`build-equity/audits/iteration6-context-measurement.json`; the driver is
`build-equity/audits/iteration6_build_training_context.py`.

## Native measurement, 2026-09-19

The approved invocation completed with exit code 0 in 66.9414 seconds (native wall).
The exact bound segment-size sum was 409,556,920 bytes across 445 verified files.
The preflight score with K=U was 2,775,826,250 bytes. Windows lifetime peak working
set was 1,026,482,176 bytes and peak private commit was 1,017,806,848 bytes; the monitor
observed no budget breach. It sampled the official counters at 100 ms intervals,
including lifetime peak counters, without trimming the process working set.

The output `data/tickerhistory_training_native_20260919/context.bin` contains
445 dates, 1,661 instruments and 12 fields, with 425,000 membership cells (at most
1,000 per date). Its payload size is 71,697,242 bytes. The receipt verifies exact
session keys, canonical positive i64 IDs, original compaction-index bounds/order,
the declared recipe, all selected segment parents, ingestion/preparation/producer
bindings, manifest component hashes, artifact ID, payload SHA and FNV trailer binding.
A complete numeric source comparison and economic adjustment attestation were not
performed by this task.

| Binding | SHA-256 / artifact ID |
| --- | --- |
| Panel artifact | `ec572b826dce65fbd4cd391921f57f01e1ce084864ec84b3e6a944003ead8079` |
| Panel payload | `c219dc237a58574e4b55df8ee51e9f239ef9d5963903e54aa253bd07eafb85ef` |
| Panel manifest | `e9d53a8932b242750f44f8744a46856b535a0b1c27bd91dcf8dda60555e5f68a` |
| Producer executable | `9415a6ab798f0e5fd0eb4fec31a7d2b3d09a47dd7eaf48e668deb844f0fe4daa` |
| Ingestion receipt | `47db6b5d3486322a4f26bd5ba75f3f06b7b22fa6dd50fc54a232fd27d96d6d40` |
| Preparation manifest | `ee17d68bfe33d58fa7279877c43d370a6b6b6a577bdb11082a70babec6e8c3df` |

An initial invocation exited 2 during argument parsing and created no context output:
this binary requires `--compact-universe true`, rather than the valueless spelling.
Its original driver and logs/measurement remain under `iteration6-context-attempt1-*`
and `iteration6_build_training_context.attempt1.py`. The corrected invocation used
the still-fresh output path; no prior panel was deleted or reused. No build, source
implementation change, archive scan or strategy evaluation was performed here.

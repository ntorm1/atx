# Historical bar availability audit - 2026-09-19

Research and read-only review. No correction, executable regression, or benchmark
is claimed in this note. This is the next proposed data integration checkpoint.

## Finding: completed Databento bars are released at interval start

The vendor-specific ingestion path preserves Databento's start timestamp, while
the generic segment feed interprets that timestamp as a completed bar's release
time. Completed close, high, low, and volume therefore become visible before
their interval ends. A next-observation execution delay does not repair the
incorrect information timestamp for joins, schedules, or other consumers.

The concrete call path is:

1. `atx-core/src/external/databento.cpp`, `decode_into`: copies
   `m.hd.ts_event` into the Parquet `ts` column without an interval adjustment.
   It also casts the vendor's unsigned timestamp to signed nanoseconds without
   first rejecting values above `INT64_MAX`.
2. `atx-tsdb/src/load_parquet.cpp`, `load_parquet_scaled` and
   `build_dated_segments`: carry that `ts` axis into the sealed segment.
3. `atx-engine/include/atx/engine/data/multi_segment_bar_feed.hpp`,
   `MultiSegmentBarFeed::step`: constructs `ShmBarFeed` without a bar convention.
4. `atx-engine/include/atx/engine/data/shm_bar_feed.hpp`, `ShmBarFeed::step`:
   advances the clock to the unmodified segment timestamp. `publish_row` sets
   both `bar.ts` and the event's knowledge timestamp to it.
5. `atx-engine/include/atx/engine/data/market.hpp`, `make_market_bar`: sets
   `event_ts=bar.ts`; the generic contract assumes a release-at-close bar.

`tests/data/databento_pipeline_e2e_test.cpp`,
`DatabentoPipelineE2E.LoaderBridgeFeedRoundTrip`, currently pins this behavior.
Its synthetic `OHLCV-1d` records start at 2024-01-02/03 00:00 UTC, and its
assertions expect the completed daily bars to be published at those instants.

## Verified vendor convention and its limits

[Databento's official OHLCV schema](https://databento.com/docs/schemas-and-data-formats/ohlcv)
defines the timestamp as the inclusive interval start and the schema suffix as
the duration. Daily bars use UTC dates, so a bar labeled 2024-01-02 spans
`[2024-01-02 00:00 UTC, 2024-01-03 00:00 UTC)`. This boundary follows from the
documented UTC-date convention; it is not the New York exchange session close.
Session-based daily bars require separate aggregation of more granular data.
The documentation also says intervals without trades produce no record and
warns that finalization/publication conventions differ across bar producers.

Interval end is the earliest possible availability of a complete aggregate; it
does not establish the provider's actual delivery time or include later trade
corrections. An end-plus-configured-lag model must be labeled a historical replay
assumption. Actual publication/revision timestamps require separate provenance.

## Proposed bounded implementation

Give the segment feed an explicit timestamp convention: existing timestamps
already represent completed bars, or they represent interval starts with a
specified positive duration. Preserve the generic completed-bar convention as
the compatibility default. Forward the convention through `MultiSegmentBarFeed`
and explicitly configure the Databento daily path for 24 UTC hours. Do not infer
duration from neighboring rows: weekends and no-trade intervals make that wrong.

For the start-stamped convention, normalize `bar.ts` to the interval end to honor
the existing engine contract, and publish only at that end plus a nonnegative
configured delivery lag. Preserve the original vendor timestamp in the source
segment/provenance; avoid rewriting generic segment timestamps or globally
adding a day. `event_ts` then describes the complete bar, while `knowledge_ts`
and the simulation clock describe when it becomes visible. If a first bounded
implementation omits the lag parameter, document the zero-lag approximation.

Validate the convention, positive interval, and nonnegative lag at construction.
Use checked arithmetic for both start-plus-duration and end-plus-lag, including
`INT64_MAX` boundaries. Fail before advancing the clock or publishing any row on
overflow/backward availability. The ingestion boundary should reject out-of-range
unsigned vendor timestamps instead of allowing a signed wrap. The generic
completed-bar path must retain exact existing timestamps.

No-trade gaps must remain absent, without fabricated bars or executable volume.
A last available bar still has a computable interval end at EOF; do not use the
next observed row as its end. Cross-segment ordering must use the same convention.
The existing market behavior of carrying marks but clearing absent-name volume
should be retained. This is independent of introducing exchange calendars.

## Buildable measurement checkpoint

- Extend `DatabentoPipelineE2E.LoaderBridgeFeedRoundTrip` to assert the source
  segment's timestamp separately from normalized bar/event/knowledge times. The
  first daily example must become visible on 2024-01-03 00:00 UTC, never Jan 2.
  The existing synthetic DBN ZIP passes through the actual loader, Parquet
  bridge, and feed, so this requires no credentials or new architecture.
- Extend the `ShmBarFeed_StepPublishesPresentBars_NoLookAhead.PublishesBars` test
  and `MultiSegmentBarFeed.*` suite with a gap, final EOF bar,
  one-minute interval, optional publication lag, unchanged generic convention,
  and overflow rejected before clock/bus mutation. These suites belong to
  `atx-engine-parallel-tests`; the pipeline suite belongs to
  `atx-engine-data-tests`.
- Add one bounded pipeline-to-`BacktestLoop` fixture proving that the signal sees
  only completed bars, default orders fill on a later normalized observation,
  and elapsed financing uses those timestamps. Compare exact fill timestamps,
  Decimal cash, and holdings across two identical replays; report counts and
  elapsed runtime. This is plumbing validation, not a profitability claim.

## Local real-data checkpoint and current limitation

The read-only probes found `ATX_DATA_DIR` and `ATX_ALPHA101_PANEL` unset, with no
`C:/atx/data` directory or configured serialized real panel. No credential files
were read and no `atx-db` files were changed.

The ready existing real-data assembly is `data::build_real_panel(RealDataConfig)`
in `atx-engine/src/data/real_panel.cpp`. The five `DataRealPanel.*` tests expect
`data/us_security_master_smoke/security_master/security_master.parquet` plus
`data/databento/equs_ohlcv_1d_by_date`. They use July 2024's 22 trading dates and
three symbols (AAPL, IBM, MSFT), and check a repeated digest, an AAPL price,
universe causality, missing cells, and lineage. Restoring that exact fixture is
the smallest genuine historical-data acceptance checkpoint. Record fixture
identity/digests and require all five to execute, rather than counting skips as
coverage. The existing ORATS `build_history_panel` / `atx-impl` panel stage is an
alternative when its per-date segments are available, not evidence that they
exist in this checkout.

There is a separate preexisting schema gap before replacing that exact fixture
with fresh C++ loader output. `build_real_panel`'s `read_partition` requests
`f64` OHLC and `u64` volume, but `databento.cpp::write_columns` emits `i64`
nanodollar OHLC and `i64` volume. `ParquetTable::column_view` rejects those type
mismatches. `load_parquet_scaled` already handles the C++ loader format for the
segment-feed path; the real-panel assembly needs explicit type/scale adaptation
before the two paths can be called interchangeable. Verify this with a synthetic
loader-to-panel integration fixture before making a real-data assembly claim.

The vendored Databento C++ dependency includes tiny `tests/data/*.dbn` decoder
fixtures. Their existence can validate format handling, but their symbols,
coverage, and suitability as an equity research sample have not been verified.
They must not be presented as a historical equity performance dataset.

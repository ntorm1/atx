# Binary persistence: the SpiderRock-derived MarketState store

Status: lane P1 of the wave-3 persistence sprint (2026-10-24). On-disk specification:
`storage/PERSIST_FORMAT.md`. Surface timelines and risk-run archives (lane P2) reuse the same
container: `storage/PERSIST_HISTORY_FORMAT.md`.

The risk engine's market input (`MarketState`, one per SpiderRock 30-minute bucket) used to be
rebuilt from Parquet every time: about 1.3-1.5 s per bucket on the licensed day. The state store
persists each bucket's `MarketStateStorage` as one ATXVPF file (`.atxms`) that opens, verifies
and serves a `MarketState` in about 11 ms (top-100 book), with marks, term carry and dividends
read in place from the mapping. The store is a derived, fingerprinted cache of the Parquet
ingest of record: it is rebuilt from the source, never edited, and a source change rebuilds the
affected bucket.

## API

```cpp
#include "atx/vol/api/persist/state_archive.hpp"

// Build (or bring up to date) a day's buckets.
StateStoreBuildOptions o;
o.out_root = "C:/atx-cache/persist";          // never inside spec.store_root
o.conventions_version = 1;                    // required, nonzero
auto reports = build_state_store_from_spiderrock(spec, buckets_et, o);

// Serve one bucket.
auto store = StateStore::open("C:/atx-cache/persist");
auto archive = store->open_bucket("2026-10-14", "1000");   // size policy, see below
const MarketState &state = archive->state();               // valid while `archive` lives
MarkExtras x = archive->extras(uid);                       // bid / ask / vendor Greeks per mark
auto bucket = selection_bucket_from_archive(*archive);     // feeds the bench book selector
```

`MarketStateArchive` owns the mapping; `state()` and every span it hands out borrow it, and a
moved archive keeps them valid (`PersistStateArchive.MovedArchiveKeepsStateValid`).
`materialize()` returns an owning `MarketStateStorage` copy when a caller needs one.
`encode_market_state` / `MarketStateArchive::open_bytes` are the in-memory pair.

## Tool: `atx-vol-state-store`

```
build   --store <root> --date D --out <root> [--from HHMM --to HHMM | --buckets a,b]
        [--top N] [--parallel N] [--no-extras] [--force] [--created-unix-ns N]
        [--sha256] [--json <file>]
inspect <file.atxms>
verify  <file.atxms> | --out <root> --date D
bench   --store <root> --date D --out <root> --bucket HHMM [--top N] [--runs N]
        [--parquet-runs N] [--no-codecs] [--preset <name>] [--json <file>]
synth   --store <dir> [--underliers N]
```

- `build` lists the day's buckets (or takes `--buckets` / `--from` / `--to`), skips the ones
  whose source fingerprint is unchanged, and prints per bucket marks, bytes, `load_s` (Parquet
  load + state build + extras), `encode_s` (`encode_market_state` alone), `publish_s` (file +
  manifest) and the file's sha256 with `--sha256`. `--top 0` keeps every underlier; the default
  is the top 100. `--created-unix-ns` pins the header timestamp so rebuilds are byte-identical.
  `--parallel` bounds concurrent bucket loads (an uncapped Parquet decode peaks near 1 GB; use
  `--parallel 1` on a loaded host). Build a day in ONE call when its archives will be replayed
  together: one call shares one uid space.
- `inspect` prints the header, fingerprints, sections, and bytes per mark.
- `verify` opens at `Paranoid` (every CRC including the whole-file CRC) and rebuilds the state;
  with `--out --date` it verifies every bucket of a day through the manifest.
- `bench` times the Parquet path against the archive: Parquet load + state build
  (`--parquet-runs`), warm open + `state()` at every verify level (`--runs`), a
  `FILE_FLAG_NO_BUFFERING` sequential read, bytes per mark, and (unless `--no-codecs`) zstd
  1/3 with and without byte shuffle as an informational cold tier. It samples host load before
  and after. It builds the bucket first if the store does not have it.
- `synth` writes a SpiderRock-shaped synthetic day for a data-free round trip
  (`synth -> build -> inspect -> verify -> bench`).

The licensed store `C:\atx-data\spiderrock-30min` is read-only. The output root is `--out`, else
the environment variable `ATX_VOL_PERSIST_ROOT`; `build`, `bench` and `verify --date` refuse
(exit 2) when neither is set. There is no compiled-in default: an absolute literal would name a
directory of whichever machine layout the binary was built for. On the research host the
sanctioned root is `C:\atx-cache\persist\` (set `ATX_VOL_PERSIST_ROOT` to it). The tool passes conventions version 1
(`kStateStoreConventionsVersion`, to become `kSpiderRockConventionsVersion` at integration).

## Store layout and rebuild rules

```
<out_root>/date=2026-10-14/persist-manifest.atxvpm
<out_root>/date=2026-10-14/persist.lock
<out_root>/date=2026-10-14/bucket_et=1000-g000000000001.atxms
...
```

A bucket is rebuilt when its source fingerprint changes: loader conventions version,
`ATX_VOL_VERSION`, every `SpiderRockStoreSpec` field, the bucket, and the (name, size,
last-write time) of each source parquet file; or when the extras choice changes or `--force`.
Publication is file-then-manifest under `persist.lock`; readers resolve buckets only through the
manifest and never see a partial file.

## Measured (licensed day 2026-10-14, 11 buckets 1000-1500, vol-rel, 16 logical CPUs)

Host load was sampled by the tool; other sprint lanes were building and benchmarking on the same
host during the build runs (busy ~60%), the clean bench windows had no other benchmark running.

### Open (bucket 1000, warm page cache, 31 runs; Parquet 5 runs)

| Book | Marks | File | Parquet load+state p50 | Framing p50 | Full p50 / p95 | Paranoid p50 | Read-mode Full p50 | Speed-up (Full) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| top-100 | 367,664 | 35.7 MB | 1.345 s | 2.25 ms | 11.08 / 15.35 ms | 14.62 ms | 19.25 ms | 121x |
| uncapped | 1,936,090 | 189.6 MB | 1.647 s | 16.20 ms | 67.47 / 74.99 ms | 88.13 ms | 104.43 ms | 24x |

A second top-100 sample (one quote-bench of another lane running, host busy 21%): Framing 2.47,
Full 11.63 ms, Parquet 1.487 s, 128x. `Full` over `Framing` is the CRC pass over every byte
(single-stream hardware CRC32C plus first-touch faults on the mapped view): about 9 ms for the
top-100 file and 51 ms for the uncapped one. `NO_BUFFERING` sequential read: 2.56 GB/s (top-100
file), 2.85 GB/s (uncapped).

**Default verify level (ruling 2026-10-24).** The default opens (`StateStore::open_bucket(date,
bucket)`, `MarketStateArchive::open(path)`) verify `Full` up to `kMarketStateFullVerifyMaxBytes`
= 64 MiB and `Framing` above it (`market_state_open_options(file_size)`). So the top-100 book
opens Full (11.1 ms) and the uncapped book opens Framing (16.2 ms instead of 67.5 ms; the
design target for it was 60 ms Full). Framing still checks the header, section-table and footer
CRCs and every structural rule of the reader; it skips only the payload CRC pass. `Lazy` is not
used: building the state touches every section at open, so Lazy measured the same as Full
(67.6 ms). Full / Paranoid stay available by passing options explicitly
(`kMarketStateOpenDefaults` is the Full set), and `atx-vol-state-store verify` runs Paranoid.

### Build (per bucket, p50 over 11 buckets)

| Book | Bytes / mark (core + extras = file) | load_s | encode_s | publish_s | encode + publish |
| --- | --- | --- | --- | --- | --- |
| top-100 | 41 + 56 = 97.20 | 1.80 s | 0.034 s | 0.052 s | 0.086 s |
| uncapped | 41 + 56 = 97.94 | 1.82 s | 0.201 s | 0.252 s | 0.431 s |

(After reserving the encode columns up front; before it, top-100 encode_s p50 was 0.077 s and
uncapped 0.416 s under ~60% host load.)

### Identity

- Rebuilds are byte-identical: the top-100 day built with `--parallel 2`, `--parallel 1` and again
  after the encode change, and the uncapped day built three times, give the same sha256 for all 11
  buckets (11 distinct files per book). A second `build` over a finished day skips all 11 buckets
  (0.75 s).
- `PersistStateStoreReal.Bucket1000ArchiveEqualsParquetState`: the archived state equals the
  Parquet-built state on every mark.
- `PersistStateStoreReal.ElevenBucketExplainTotalsBitEqual`: a `RiskStream` explain over the 11
  archived states gives bit-equal totals to the Parquet-built states on all 10 transitions
  (e.g. 1000->1030 pnl_total -18348.754358, unexplained -897.525620, n_ok 2050).
- `verify --out <root> --date 2026-10-14` (Paranoid: every section CRC and the whole-file CRC,
  then the state rebuild) passes on all 11 buckets of both books.

### Cold tier (informational, uncapped bucket 1000, 8 threads)

| Codec | Ratio | Encode | Decode |
| --- | --- | --- | --- |
| zstd-1 | 3.28 | 0.263 s | 0.71 GB/s |
| zstd-1 + shuffle | 4.56 | 0.233 s | 0.65 GB/s |
| zstd-3 | 3.80 | 0.320 s | 0.59 GB/s |
| zstd-3 + shuffle | 4.64 | 0.304 s | 0.72 GB/s |

The served store stays uncompressed: decode at well under 1 GB/s would cost more than the whole
uncompressed Full open.

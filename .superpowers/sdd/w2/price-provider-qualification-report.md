# Computed price-exposure provider qualification

2026-09-26. Production/configured source `9f635f3e`; final fixture-only source
`cb9eda17`. **All five distinct PriceExposureProvider checks now pass**, with
one initial fixture setup failure followed by its successful correction.
There are six executions, no unresolved failure and no skip.

The new producer computes log ADV63, Amihud63, cap-market beta252, population
residual volatility252, momentum252 excluding the latest21 dates and reversal21,
then returns the actual normalized ExposurePanel. Both Panel and D6 adapters
are implemented in a private CPP. Independent source review `244575d2` covers
prior-known weights, strict complete windows, clocks, missing support and joint
memory admission.

Four owning checks passed on the first run: independent scalar descriptor
arithmetic, previous-decision cap weights and unknown denominators, future
mutation/truncation, and distinct absent/late/zero-volume handling. The D6 check
stopped before invoking the provider because its synthetic PanelStore writer
omitted the required nonempty source-parent list. Test-only `cb9eda17` (owner
`c0de617b`) adds that declared synthetic parent and error diagnostics; production
admission/calculation is unchanged. The corrected case passes original-f64
close parity plus axis/manifest/basis/membership/budget/oversized-metadata
refusals. Its raw/volume fixture values are exactly representable in f32;
general stored f32 inputs retain their declared quantization.

First cohort:4/5, native0.975s. Corrected failed case:1/1, native0.497s. The four
already-passing cases were not rerun after this test-only change. Their original
binary receipt is historical; the current data executable matches the corrected
case receipt. No unrelated prior data suite is claimed rerun.

Initial build passed in **45.8889538 s**, Jobs3, including CMake's reported28.3s
configuration and1.0s generation. Exactly two new CPPs and two links; no existing
caller/PCH/dependency rebuild. Before that actual launch, one attempt was held
by the memory check (986 MiB commit headroom); the successful launch had2,725 MiB
available physical memory and3,668 MiB commit headroom. The fixture rebuild
passed in **21.0697718 s**, Jobs2, exactly one test TU and one link, no configure
or production rebuild. These are shared-host iteration measurements.

The companion JSON binds all XML/log/receipt and build files, with explicit
initial-versus-current binary attribution. Full index:
`build-equity/w2-price-provider-qualification-index.json`, SHA256
`0974cb6f7cad33f4642532bd6dde30c8d80f1b5404a444de3aa69d1aafa87f8c`.

This is computed, immediately consumable exposure production on bounded
synthetic inputs. Qualified cap/industry evidence is still supplied by the
caller; actual D3/D4 adapters, persisted exposure stage, real coverage,
large-scale memory/throughput and empirical alpha acceptance remain open.
Strict full-support defaults may make many dates unavailable; no implicit
support renormalization or coverage claim is made. No actual market payload,
warehouse write, long benchmark or alpha promotion occurred.

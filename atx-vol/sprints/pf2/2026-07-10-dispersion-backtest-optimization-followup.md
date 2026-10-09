# Dispersion backtest optimization follow-up

**Branch:** `feat/atx-vol-spy-listed-dispersion`  
**Baseline profile:** `0162fa2`  
**Main merged:** `c87deac`

## Changes

- Book construction no longer invokes `dispersion_signal`; implied correlation is
  IV-only and recorded only with `DispersionConfig::record_diagnostics=true`.
- Sizing evaluates every American leg once and carries its mark into lot creation.
- One full-book risk frame now supplies entry vega, per-UID hedge delta, and row
  Greeks. Per-UID hedge portfolios and the duplicate row-Greek pass are gone.
- P&L and row-only paths use totals APIs, materializing diagnostic frames only on
  an error. The remaining full-risk frame uses caller-owned reusable storage.
- A retained pricer/workspace retimes an unchanged absolute-expiry book in place.
- `SnapshotCache` coalesces archive loads and asynchronously prefetches the next
  date; callers may share it across backtest and reconciliation phases.
- Reusable run-spec, universe-schedule, strategy construction, and surface-only
  orchestration moved from the SPY CLI into `atx-vol` library modules.

## Instrumented five-date synthetic dispersion run

The compile-time profile/counter build reported:

| Measure | Result |
|---|---:|
| Engine wall | 25.94 ms |
| Archive open | 11.68 ms |
| Execution | 9.01 ms |
| Step P&L | 5.47 ms |
| Strategy book construction | 3.24 ms |
| Prepared builds | 5 (down from 9 before retained reuse) |
| Frame allocations | 0 reported (down from 70 before reusable output) |
| Boundary solves | 396 |

Ten direct production-build repetitions are noisy on this Windows host; steady
interactive runs are approximately 18-25 ms for five dates. Both instrumentation
options are restored OFF for the canonical Release build.

## Remaining cost

American pricing remains the dominant compute kernel (396 boundary solves and
353,664 normal-CDF calls in this small run). Archive open is now the largest
single inclusive phase on the synthetic workload; prefetch overlaps it with prior
step work, while a shared cache removes it entirely on subsequent workflow passes.

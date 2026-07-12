# ATX volatility workspace architecture

## Product boundary

The target is an institutional volatility workstation, not a static charting
demo. The desktop process should coordinate market state, models, risk, orders,
and replay while keeping pricing, persistence, and feed logic in reusable
libraries. UI panels render view models and dispatch typed commands; they do not
own pricing models or directly parse vendor data.

## Layers

1. **Shell** — lifecycle, workspaces, versioned layouts, panel/command registry,
   keyboard routing, notifications, preferences, and diagnostics.
2. **Market session** — symbol directory, underlying/option NBBO, rates,
   dividends, borrow, corporate events, freshness, and feed health.
3. **Analytics services** — surface fitting, theoretical values, Greeks,
   scenarios, relative-value signals, portfolio aggregation, and caching. These
   remain in `atx-vol`; UI adapters publish immutable snapshots.
4. **Workspace models** — selected symbol/expiry/account/scenario, filters,
   linked-panel events, and pure table/chart projections. These must be testable
   without a graphics context.
5. **Panels** — dense reusable components composed into symbol, surface, risk,
   trade, and replay workspaces.
6. **Execution boundary** — staged orders, validation, limits, routing adapter,
   acknowledgements/fills, cancel/replace, and an immutable audit trail. No panel
   may talk directly to a broker adapter.

## Source contract

`VolSurfaceSource` is the first implementation of the adapter boundary. It
publishes source metadata, all-expiry summaries, the selected curve and option
marks, fit diagnostics, and source-health counts. `OpraVolSurface` translates a
Databento replay snapshot and fitted `atx-vol` session into that contract.

The next source evolution should publish immutable generation-numbered snapshots
through a market-session service. Panels should never hold locks or observe a
partially updated chain. Historical replay, live OPRA, and scenario surfaces
must use the same read contract.

## Workspace modules

### Implemented foundation

- Dockable, persistent and layout-versioned application shell.
- Generic surface-source interface and OPRA adapter.
- Pure workspace interaction model and quote-edge calculations with tests.
- Symbol header, expiry navigation, vol slice, diagnostics, term structure, and
  price/IV/Greeks ladder.
- Headless real-data load/fit/Greeks acceptance path.

### Next build tranches

1. **Live market session and replay clock** — atomic snapshots, freshness and
   entitlement state, reconnect/backoff, time controls, and linked symbols.
2. **Full option chain** — paired calls/puts by strike, sizes, open interest,
   volume, IV/Greeks, configurable columns, row selection, and strategy legs.
3. **Trade construction** — multi-leg ticket, payoff/Greek preview, tick and
   credit/debit validation, staged orders, limit controls, and broker-neutral
   execution commands.
4. **Portfolio and risk** — positions, live P&L explain, Greek buckets by expiry
   and underlying, scenario grids, hedge suggestions, and account limits.
5. **Relative value** — current/previous/theoretical surfaces, term/skew/richness
   scans, event-aware comparisons, and alert rules.
6. **Historical and reference integration** — the PIT earnings database,
   corporate events, surface archives, replay sessions, and research annotations.
7. **Operations** — structured logs, feed/model health, latency/freshness
   telemetry, crash recovery, workspace serialization, and audit export.

## Non-negotiable design rules

- No symbol, expiry, data path, account, or model name is hard-coded in a panel.
- UI code consumes stable view models; it does not include feed-vendor schemas.
- Cross-panel actions are typed state transitions, not global variables.
- Market/model updates are immutable and generation-stamped.
- Pricing and risk calculations have headless acceptance tests.
- Any order-capable path includes validation, limits, and audit before routing.
- A new panel should be composable into another workspace without copying its
  state or renderer.


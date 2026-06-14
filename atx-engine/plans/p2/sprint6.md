# BYO-Data: driving the engine with your own data (Sprint 6)

Sprint 6 turns atx-engine into a **bring-your-own-data** pipeline. Instead of hand-building one
fixed `alpha::Panel` per run, you register your data as typed **Datasets** in a **`DatasetCatalog`**,
wrap it in a **`DataContext`**, and let `BookPipeline::run_with_context` read everything through that
facade. **Only a price dataset is required** — features, external signals, a factor model, and reference
data are all optional plug-ins. A price-only context reproduces today's path **bit-for-bit**.

All types live in `namespace atx::engine::data` (headers under `atx/engine/data/`).

## The four plugs

| Role | What it is | How the engine uses it |
|------|-----------|------------------------|
| `Role::Price` (**required**) | OHLCV (or `{"close","rev"}`) columns | lowered to the `alpha::Panel` the engine mines / combines / reports over |
| `Role::Feature` (optional) | named feature columns (e.g. `"sentiment"`) | merged into the panel as extra fields; referenceable by name in a `FeatureSpec` |
| `Role::Signal` (optional) | a precomputed external signal column | realized into pnl/positions and admitted through the **live** deflated-Sharpe / robustness gate |
| `FactorModelArtifact` (optional) | your own SPD `X`/`F`/`D` factor model | used as the risk model `V` for optimize instead of the price-derived one |

## Minimal price-only path

```cpp
using namespace atx::engine::data;

// 1. Build a price Dataset (date-major columns, ascending DateKeys).
DatasetSchema s;
s.columns = {"close", "rev"};                 // or full OHLCV: {"open","high","low","close","volume"}
s.dtypes  = {ColumnDType::F64, ColumnDType::F64};
s.role    = Role::Price;
auto price = Dataset::create(std::move(s), dates, instruments,
                             {close_col, rev_col}, /*mask=*/{},
                             DatasetProvenance{"my:prices", "daily bars"}).value();

// 2. Register it in a catalog under a name.
DatasetCatalog catalog;
catalog.register_dataset("prices", std::move(price));   // returns Status

// 3. Build a DataContext (empty adv_windows => RAW lowering; this is the boundary-pin path).
DataContext ctx = DataContext::create(catalog, "prices").value();

// 4. Run the book pipeline through the context.
BookPipeline pipe{lib, dsl, ctx.price_panel().value().get(), sim, policy, gate};
auto report = pipe.run_with_context(ctx, cfg).value();
```

This price-only run is **byte-identical** to the legacy `BookPipeline::run(cfg)` over the same hand-built
Panel (the S6.8 boundary pin). Pass a non-empty `adv_windows` to `DataContext::create` to take the
`with_datafields` OHLCV-augmentation path instead of raw lowering.

## Full BYO path (price + feature + signal + factor)

```cpp
DatasetCatalog catalog;
catalog.register_dataset("prices",    price_dataset);      // Role::Price   (required)
catalog.register_dataset("sentiment", feature_dataset);    // Role::Feature ("sentiment" column)
catalog.register_dataset("signal",    signal_dataset);     // Role::Signal  ("ext_sig" column)

DataContext ctx = DataContext::create(catalog, "prices").value();

// Optional BYO factor model (an SPD X/F/D block sized to the universe). Not a Dataset.
ctx.set_factor_model(FactorModelArtifact{X, F, D, /*fit_begin=*/0, /*fit_end=*/1});

BookPipeline pipe{lib, dsl, ctx.price_panel().value().get(), sim, policy, gate};
auto report = pipe.run_with_context(ctx, cfg).value();
```

`run_with_context` (1) admits the context's external-signal candidates into `lib` through the **same gate**
the mine path uses — they only land if they clear the live deflated-Sharpe / robustness battery — then
(2) runs the mine → combine → optimize → report flow, using the BYO factor override as `V` when present.

**Notes / gotchas**

- Feature/signal column names must **not** collide with price-derived field names (the merge errors on a
  clash). Pick distinct names like `"sentiment"`, `"ext_sig"`.
- External signals are gated, not assumed. Plant a genuinely predictive column if you want it admitted;
  check `lib.n_alphas()` (or the candidate count) to confirm.
- `DataContext` is **move-only** and **borrows** the catalog — the catalog must outlive the context, and
  every reference/span an accessor returns is valid only while the context lives.
- All entry points return `atx::core::Result<T>` / `Status` (`tl::expected`): check `.has_value()` and read
  `.error()` on failure.

## Provenance / lineage report

`write_catalog_report(catalog, out_dir)` (header `atx/engine/data/catalog_report.hpp`) emits a
**byte-reproducible** `<out_dir>/catalog_report.txt` listing every dataset (ascending name) with its role,
schema, provenance source, and `derive` lineage parents — no timestamps / RNG / absolute paths, so it
diffs cleanly across runs. Record lineage edges with `catalog.derive(child, {parents...})` before writing.

## Reference data (factor builder inputs)

A `Role::Reference` dataset carrying `"market_cap"` + `"group_id"` columns supplies the cross-section the
internal `FactorModelBuilder` consumes — fetch it as-of a date with `ctx.reference_spans_at(date, default_group)`.
Use this when you want the engine to *build* the factor model from your classifications rather than supplying
a finished `FactorModelArtifact`.

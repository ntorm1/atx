# Fundamental signal improvement loop 04 — total asset growth prototype

Date: 2026-08-09  
Prototype run: `fundamental-signals-loop4-asset-growth-prototype-20260809`

## Hypothesis

Cooper, Gulen, and Schill document a strong negative relation between annual
total-asset growth and subsequent U.S. stock returns, including among large-cap
stocks. Fama and French later include a conservative-minus-aggressive investment
factor in their five-factor model.

Primary sources:

- [Cooper, Gulen, and Schill, *Asset Growth and the Cross-Section of Stock
  Returns*](https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1540-6261.2008.01370.x)
- [Fama and French, *A Five-Factor Asset Pricing Model*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2287202)

The test used `-(total_assets_t / total_assets_t-1 - 1)`, so higher values mean
more conservative investment. It reused loop 03's revision-safe current/prior
annual joins and investability gates. The prototype had 69,226 observations,
777 securities, and 187 formation dates before forward-return availability.

## Result

| Horizon | Mean rank IC | HAC t | Top-minus-bottom decile | Hit rate |
|---:|---:|---:|---:|---:|
| 21d | -0.0175 | -2.06 | -0.04% | 44.2% |
| 63d | -0.0268 | -2.08 | -0.02% | 44.1% |
| 126d | -0.0293 | -1.62 | 0.52% | 46.7% |
| 252d | -0.0328 | -1.52 | 0.52% | 42.9% |

Decile monotonicity was negative at every horizon. Top- and bottom-decile
monthly turnover were 19.9% and 18.1%; rank autocorrelation was 0.946.

Decision: reject before production materialization. The sign is opposite the
historical prediction and the extreme-decile economics are weak/inconsistent.
The temporary evaluation rows were removed from governed metric tables because
the prototype has no production factor definition. Metrics were archived to:

- `data/signal_loop4_asset_growth_prototype_ic.parquet`
- `data/signal_loop4_asset_growth_prototype_deciles.parquet`
- `data/signal_loop4_asset_growth_prototype_turnover.parquet`

This quick rejection avoided expanding the production feature surface for a
candidate that failed both direction and monotonicity gates.

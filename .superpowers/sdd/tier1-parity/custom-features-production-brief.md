# Production custom features and forward-return research

User request, 2026-09-20: prioritize the complete US fundamentals production
warehouse and add custom TickerHistory features evaluated with forward-return
decile spreads. This work complements the activation critical path. Do not delay
bulk source reloads for this task or turn it into a broad testing/cleanup project.

## Task CF1: bounded daily feature and evaluation tables

Fresh Codex implementer; read program.md and this brief. Discover existing
signal_eval helpers first (graph unavailable, rg fallback is permitted). Own new
custom_features.py, a small explicit CLI, bodies_0313.py, focused tests and
docs/CUSTOM_FEATURE_RESEARCH.md. Reserve migration0313; do not edit registry.py
or migrations/__init__.py until root transfers ownership after integration0312.
No activation/jobs edits. Expose callable build/evaluate APIs and operator
commands so root can wire them after the existing activation ladder.

### Data and features

- Source is the corrected daily price warehouse, populated from the completed
  TickerHistory3.parquet (SHA256
  0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae).
  Prefer existing bars plus provenance over a second source ingestion path.
- Produce a WIDE daily custom-feature table, definition/version metadata, small
  per-date decile results, and evaluation summaries/run manifests. Pin exact
  formulas before seeing forward outcomes. Persist all tested features, including
  null and unsuccessful results; no selecting only winners.
- Start with eight fixed hypotheses using causal adjusted-price/raw-volume data:
  5-session reversal; 126-session momentum skipping latest21; volatility-scaled
  63-session momentum; 5-vs63 raw-dollar-volume shock; 21-session close-location
  pressure; 5-vs63 relative intraday-range compression; liquidity-conditioned
  reversal; compression-times-accumulation interaction. Specify exact windows,
  minimum observations, denominator/domain handling and fixed direction in the
  definition metadata. These are custom research candidates, not claimed novel
  or profitable signals. Propose a bounded change to root if source semantics
  make one of these unsuitable; no parameter/horizon grid search.
- Adjusted close is same-row raw close*cumulative return factor. NULL split-only
  factor is unknown; never infer no split. Raw dollar volume uses raw price.
  Unknown economic adjustment anomalies remain a disclosed input-quality limit.
- Work with calendar-session windows; do not let a missing security session
  shorten a stated calendar horizon. A compact calendar index plus range windows
  or exact lag-date joins is acceptable. Invalid observations remain visible in
  coverage counts, not silently filled. No current ticker/sector/classification
  backcast and no earnings proximity/forward-looking fields.
- Conservative execution contract: feature row at decision date T+22h uses data
  only through the previous market session, and enters at the NEXT session close.
  Join labels on that entry date, not the decision date. Max all selected input
  availability clocks and enforce known-by-decision eligibility. The vendor says
  full-history delivery is05:00CT T+1; use an explicit conservative noon-UTC
  next-calendar-day floor for the source observation, in addition to stored
  clocks. This is a modeled backfill clock, not proof of historical delivery.
  Record input end date, decision timestamp, entry date, source/version/hash.
- All source/feature work in SQL or bounded sequential security partitions.
  Never load the full daily panel or long feature cross-product into pandas.
  Default DuckDB1GB/1thread; root controls heavy workload slot. Stable scoped
  replacement and explicit as-of/run timestamps; no runtime now().

### Evaluation and promotion evidence

- Primary horizon20 market sessions; secondary5 and60 are sensitivity checks,
  not alternative horizons for choosing significance. Reuse the production
  survivorship forward panel, with adjusted basis and terminal provenance.
  Do not use the legacy raw-price/per-security-shift label helper.
- Fixed equal-weight daily deciles formed from available FEATURES BEFORE joining
  returns. Stable ties by security_id; minimum200 eligible names/20 per decile;
  exclude constant/degenerate cross-sections explicitly. Report eligible counts,
  labeled counts, missing/terminal/imputed counts per decile and endpoint.
  Missing labels must not silently change ranks or look like delisting-safe data.
- Historical cohort must be named honestly. The currently bar-observed liquid
  candidates lack certified historical US-common listing evidence. Use trailing
  raw dollar volume/price eligibility known at decision (pin thresholds e.g.
  price>=5 and 63-session mean dollar volume>=1m, >=50 valid sessions). Record
  this separate research cohort and block production promotion until actual
  listing/source/label prerequisites pass. Do not assert all observed IDs are US
  common stocks. No synthetic backdating of current directory membership.
- Chronological fixed partitions: train through2020, validation2021-2023,
  holdout2024 onward through explicit as-of. Purge labels crossing a split
  boundary; keep a60-session embargo after each new split begins. No holdout
  tuning, future ranks, direction selection on holdout or retrospective winners.
- Aggregate to date/feature/horizon/decile in SQL before Python statistics.
  Persist Q10-Q1 horizon spreads, means, robust uncertainty, counts, time
  stability, label coverage and gross cost-sensitivity. These overlapping
  horizon returns are NOT a daily portfolio PnL or annualized trading Sharpe.
- For the small spread series use Bartlett/Newey-West mean standard errors with
  lag at least horizon-1 in trading sessions. Inspect existing helper; do not
  reuse its monthly ceil(h/21) rule on daily observations. Preserve calendar gaps
  when computing dependence; do not compress missing dates into adjacent days.
  Use a stated large-sample two-sided p-value and95%interval; insufficient/
  degenerate samples have no significance result. Holm-adjust the complete eight
  primary tests within each split; do not shrink the family to successful ones.
- Report both gross spreads and explicit simple cost scenarios (e.g.10/25/50bp
  per side with clear two-leg entry/exit accounting); this is sensitivity, not
  a realized execution backtest or borrow/capacity verification. Holdout primary
  corrected p<=.05, positive prespecified direction, positive25bp/side scenario,
  >=252 holdout dates, annual stability, >=99% label coverage, and source/listing/
  terminal prerequisites are necessary to be a production candidate. Keep
  statistical pass separate from production eligibility. If none qualify,
  publish that result and retain the feature warehouse without manufacturing alpha.

### Delivery and checks

One compact focused package for causal windows/entry timing, ranks before label
attrition, purge/embargo, known synthetic decile spread/HAC and fixed-family Holm,
source-scoped idempotence/rollback. No full suite, no giant synthetic benchmark.
Root grants DB test slot, Windows guard2-3GiB as headroom permits. One independent
review; Important accepted on report, Critical rereview only. Pathspec-only commit
with the original required trailer; no forbidden git operations or live writes.
Write CF1 implementation report with APIs, formulas, row estimates, command and
actual validation. Live statistics are a later root-operated measured run.

### Primary references checked by root

- SpiderRock source definitions and delivery schedule:
  https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/
- Newey and West, original1987 HAC paper:
  https://users.ssc.wisc.edu/~behansen/718/NeweyWest1987.pdf
- Harvey, Liu and Zhu, multiple testing in expected-return research:
  https://people.duke.edu/~charvey/Research/Published_Papers/P118_and_the_cross.PDF

These justify the source interpretation and inference safeguards. The eight
feature formulas, splits and promotion thresholds are our predeclared research
design, not claims that the papers endorse or validate these signals.

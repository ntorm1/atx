# Finding Alphas Book Deep Dive

> **A book-derived research dossier for the ATX engine.**
> This report distills *Finding Alphas: A Quantitative Approach to Building Trading
> Strategies*, Second Edition, edited by Igor Tulchinsky et al. and WorldQuant
> Virtual Research Center, into an implementation-oriented reference for `atx-engine`.
> The emphasis is alpha design, data selection, validation, automated search, risk,
> WebSim-style simulation settings, and concrete engine design takeaways.

**Compiled:** 2026-06-13

**Source text:** `C:\Users\natha\OneDrive\Desktop\books\Finding Alphas - A Quantitative Approach - 2nd ed - extracted.txt`
**Source PDF:** `finding-alphas-a-quantitative-approach-to-building-trading-strategies-2ndnbsped-1119571219-9781119571216_compress.pdf`
**Extraction note:** the text file was produced locally with Python/pdfplumber from a 321-page PDF and contains roughly 522k extracted characters.
**Copyright note:** this is an analytical summary and implementation map. It paraphrases the book and avoids extended quotations.
**Method:** local book-text analysis plus alignment against the existing `atx-engine` research convention and local module layout. No web lookup was used.
**Provenance legend**

- **BOOK-VERIFIED** - stated or directly supported by the extracted book text.
- **ATX-LOCAL** - grounded in the current `atx-engine` module layout or existing research convention.
- **INFERENCE** - a design conclusion drawn from the book, not directly stated by the authors.
- **DOMAIN-KNOWLEDGE** - standard quant engineering context used to connect the book to implementation.

---

## Table of Contents
1. [Executive Summary](#1-executive-summary)
2. [Book Shape and Second-Edition Context](#2-book-shape-and-second-edition-context)
3. [Chapter Map](#3-chapter-map)
4. [Alpha Definition and Object Model](#4-alpha-definition-and-object-model)
5. [Alpha Design Loop](#5-alpha-design-loop)
6. [Data Layer](#6-data-layer)
7. [Evaluation and Admission](#7-evaluation-and-admission)
8. [Turnover and Costs](#8-turnover-and-costs)
9. [Correlation and Pool Value](#9-correlation-and-pool-value)
10. [Bias, Overfitting, and Robustness](#10-bias-overfitting-and-robustness)
11. [Risk and Drawdowns](#11-risk-and-drawdowns)
12. [Automated Search and Machine Learning](#12-automated-search-and-machine-learning)
13. [Domain-Specific Data Sources](#13-domain-specific-data-sources)
14. [WebSim as Product Spec](#14-websim-as-product-spec)
15. [Mapping to atx-engine](#15-mapping-to-atx-engine)
16. [Implementation Backlog](#16-implementation-backlog)
17. [Agent Indexing Notes](#17-agent-indexing-notes)
18. [Caveats and References](#18-caveats-and-references)

---

## 1. Executive Summary
**BOOK-VERIFIED:** The book defines a WorldQuant-style alpha as an automated predictive model that converts data into positions or trades.
**BOOK-VERIFIED:** In the book's usage, an alpha is an individual signal, not a full investment business.
**BOOK-VERIFIED:** An alpha is an idea about market behavior expressed through mathematics, source code, and configuration parameters.
**BOOK-VERIFIED:** The book treats alpha research as a repeatable process: find data, form a hypothesis, encode the signal, test it, reject most variants, and submit only those that survive.
**BOOK-VERIFIED:** The second-edition preface says WorldQuant had produced more than 20 million alphas by June 2019.
**BOOK-VERIFIED:** The same preface says WorldQuant operated through 28 offices and more than 2,000 research consultants at that time.
**BOOK-VERIFIED:** The second edition added new material on ETFs, index alphas, intraday data, event-driven investing, automated search, and machine learning.
**BOOK-VERIFIED:** The book's core quality traits are simplicity, elegant expression/code, good in-sample Sharpe, low parameter sensitivity, cross-universe performance, and cross-region performance.
**BOOK-VERIFIED:** The book treats data as both simulation fuel and idea source.
**BOOK-VERIFIED:** It repeatedly warns that poor data quality, historical bias, stale assumptions, and format changes can destroy predictive value.
**BOOK-VERIFIED:** Turnover is a first-class constraint because prediction quality without cost realism can be untradeable.
**BOOK-VERIFIED:** Alpha correlation is a first-class metric because a lower-correlation alpha can add more value than a stronger but redundant alpha.
**BOOK-VERIFIED:** Backtests are necessary but dangerous; the book repeatedly distinguishes in-sample appearance from out-of-sample predictive value.
**BOOK-VERIFIED:** The book names look-ahead bias, data mining, overfitting, and behavioral research bias as practical failure modes.
**BOOK-VERIFIED:** Risk is split into extrinsic risks, such as unwanted exposure to industries or known factors, and intrinsic risks, such as volatility, tail loss, and drawdown behavior.
**BOOK-VERIFIED:** The automated-search chapter is a direct blueprint for a scalable alpha factory, but it is also a warning about mass-producing noise.
**BOOK-VERIFIED:** Automated search should constrain input categories, prefer comparable variables, prune search space, reuse intermediates, run sensitivity tests, and judge batches as well as single alphas.
**BOOK-VERIFIED:** The WebSim chapter gives concrete simulation knobs: delay, decay, max stock weight, neutralization, lookback days, universe, book size, and result metrics.
**ATX-LOCAL:** `atx-engine` already has modules that align with the book's workflow: `alpha`, `factory`, `combine`, `eval`, `validation`, `risk`, `exec`, `loop`, `book`, `learn`, and `parallel`.
**ATX-LOCAL:** The `alpha` module already exposes a WorldQuant-like operator registry with cross-sectional, time-series, group, and stateful operations.
**ATX-LOCAL:** The `factory` layer already encodes pool-aware fitness, correlation-to-pool, deflated Sharpe, CPCV, weak-universe robustness, and trial-count awareness.
**ATX-LOCAL:** The `book` pipeline is already positioned as an end-to-end mine -> admit -> promote -> combine -> risk -> optimize -> monitor -> recycle -> report flow.
**INFERENCE:** The best way to use this book is to convert its research advice into executable engine contracts.
**INFERENCE:** A candidate alpha should carry more than an expression string; it should carry hypothesis, data provenance, search context, simulation settings, and validation evidence.
**INFERENCE:** A single-alpha dashboard is incomplete without turnover, cost, correlation-to-pool, risk exposure, robustness, and drawdown diagnostics.
**INFERENCE:** A WorldQuant-style engine needs both per-alpha quality control and pool-level marginal-contribution scoring.
**INFERENCE:** Automated search should optimize for robust search spaces and productive batches, not merely the best isolated backtest.
**Implementation thesis:** ATX should make alpha research auditable.
**Implementation thesis:** Every run should be reproducible from expression, data version, universe, delay, decay, neutralization, cost model, search seed, and trial count.
**Implementation thesis:** Every admitted alpha should explain why it may be real and why it may fail.
**Implementation thesis:** Every promoted alpha should be monitored for decay, changing risk, changing correlation, and live drawdown profile.

---

## 2. Book Shape and Second-Edition Context
**BOOK-VERIFIED:** The book is a multi-author WorldQuant collection, not a single-author textbook.
**BOOK-VERIFIED:** The contributors include WorldQuant founder, directors, managers, portfolio managers, technologists, and researchers.
**BOOK-VERIFIED:** The preface says the book summarizes lessons from 47 current and former WorldQuant staffers.
**BOOK-VERIFIED:** The book has five parts: introduction, design/evaluation, extended topics, WebSim, and final advice.
**BOOK-VERIFIED:** Part I defines alpha creation and research posture.
**BOOK-VERIFIED:** Part II focuses on design, evaluation, turnover, correlation, biases, robustness, risk, automated search, and machine learning.
**BOOK-VERIFIED:** Part III widens the discussion to data sources and asset-specific alpha families.
**BOOK-VERIFIED:** Part IV describes WebSim as a user-facing alpha research platform.
**BOOK-VERIFIED:** Part V emphasizes quant work habits, automation, and overfit discipline.
**BOOK-VERIFIED:** The preface says the second edition is substantially revised, not just repackaged.
**BOOK-VERIFIED:** It states that machine learning and automated search had become much more important by the time of revision.
**BOOK-VERIFIED:** The book is explicitly educational and informational, not investment advice.
**INFERENCE:** The sequence of chapters is factory-shaped.
**INFERENCE:** It begins with definition, proceeds through design and evaluation, adds risk and robustness, widens into data domains, then productizes the workflow in WebSim.
**INFERENCE:** The implied production path is idea -> data -> expression -> backtest -> robustness -> risk -> pool -> platform -> monitoring.
**ATX-LOCAL:** Existing research files in `atx-engine/research` use executive summary, scope note, confidence labels, detailed sections, open questions, and references.
**ATX-LOCAL:** This report follows that convention, but treats the extracted book as the primary source.
**Scope boundary:** This report does not reproduce the book's figures, tables, or full examples.
**Scope boundary:** It is intended as an engineering reference for agents and humans working on ATX.

---

## 3. Chapter Map

| Ch. | Book chapter | Core idea | ATX hook |
|---:|---|---|---|
| 1 | Introduction to Alpha Design | Alpha as data-driven predictive signal | `alpha` expression + run config |
| 2 | Perspectives on Alpha Research | Stat arb, prediction humility, OOS evaluation | validation policy |
| 3 | Cutting Losses | Rules fail; retire broken rules | lifecycle + decay monitor |
| 4 | Alpha Design | Inputs, universe, frequency, value | candidate metadata |
| 5 | How to Develop an Alpha | Convert idea to predictive formula | research card |
| 6 | Data and Alpha Design | Find, validate, understand data | data cards |
| 7 | Turnover | Horizon, cost, crossing, turnover control | exec sim + turnover gates |
| 8 | Alpha Correlation | Uniqueness relative to pool | `combine` and pool scoring |
| 9 | Backtest - Signal or Overfitting? | OOS, multiple testing, stat arb caution | CPCV/PBO/DSR |
| 10 | Controlling Biases | Look-ahead, data mining, behavioral bias | bias audit |
| 11 | Triple-Axis Plan | Search by ideas, datasets, parameters, regions | search-space design |
| 12 | Robustness Techniques | Outliers, robust stats, stability | robustness battery |
| 13 | Alpha and Risk Factors | Alphas can become factors | factor risk model |
| 14 | Risk and Drawdowns | Extrinsic/intrinsic risk and drawdowns | risk diagnostics |
| 15 | Automated Search | Scale, search constraints, batch quality | `factory` + `parallel` |
| 16 | Machine Learning | Prediction, complexity, loss design | `learn` layer |
| 17 | Thinking in Algorithms | Algorithms, shrinkage, optimization | compiler/VM discipline |
| 18 | Equity Price and Volume | Core market observables | OHLCV fields |
| 19 | Financial Statements | Accounting factors and screens | fundamentals adapter |
| 20 | Fundamental Analysis | Earnings, accruals, cash flow | slow PIT data |
| 21 | Momentum Alphas | Momentum as signal and risk factor | factor neutralization |
| 22 | News and Social Media | NLP sentiment, novelty, no-news effects | text/event features |
| 23 | Options Market | Skew, spread, volume, open interest | derivatives features |
| 24 | Analyst Reports | Recommendations, targets, estimates | analyst-data adapter |
| 25 | Event-Driven Investing | M&A, spin-offs, distress, index events | event calendars |
| 26 | Intraday Data | Microstructure, spread, liquidity | intraday panel |
| 27 | Intraday Trading | Interval alphas and intraday neutrality | high-frequency loop |
| 28 | Index Alpha | Index membership and rebalance anomalies | PIT index store |
| 29 | ETFs | ETF exposures, groups, tracking risks | grouped instruments |
| 30 | Futures and Forwards | Speculator flow and asset groups | multi-asset expansion |
| 31 | WebSim | Platform settings and results | WebSim preset/report |
| 32 | Seven Habits | Automation, goals, overfit caution | workflow automation |

**INFERENCE:** Chapters 4, 6, 7, 8, 9, 10, 14, 15, and 31 are the most directly actionable for engine design.
**INFERENCE:** Chapters 18-30 are best read as feature-adapter specifications, not as standalone strategy recipes.

---

## 4. Alpha Definition and Object Model
**BOOK-VERIFIED:** Chapter 1 distinguishes Jensen-style alpha from WorldQuant's internal alpha usage.
**BOOK-VERIFIED:** In the WorldQuant usage, an alpha is an individual trading signal that seeks to add value to a portfolio.
**BOOK-VERIFIED:** Chapter 1 describes an alpha as an automated predictive model that captures a market relation.
**BOOK-VERIFIED:** Chapter 1 says an alpha uses mathematical expressions, source code, and configuration parameters.
**BOOK-VERIFIED:** Chapter 1 says an alpha contains rules for converting input data to positions or trades.
**BOOK-VERIFIED:** Chapter 2 describes an alpha as a function from relevant data to forecasts for instruments in a prediction universe.
**BOOK-VERIFIED:** Chapter 2 explicitly names C++ and Python as possible implementation languages.
**BOOK-VERIFIED:** Chapter 4 treats data inputs, alpha universe, and prediction frequency as core design decisions.
**BOOK-VERIFIED:** Chapter 31 presents an alpha as expression plus simulation settings.
**INFERENCE:** An ATX alpha object should have at least five layers.
- Layer 1: hypothesis, meaning the market behavior being asserted.
- Layer 2: data dependency, meaning fields, categories, vendors, timestamps, and update clocks.
- Layer 3: expression, meaning the formula or compiled program that maps fields to scores.
- Layer 4: simulation configuration, meaning delay, decay, neutralization, lookback, universe, and name caps.
- Layer 5: production context, meaning pool correlation, risk exposure, turnover, cost, decay, and lifecycle state.
**ATX-LOCAL:** `alpha::Library`, `OpCode`, `Shape`, and `DType` already encode the expression vocabulary.
**ATX-LOCAL:** `loop::ISignalSource` and `combine::CombinedSignalSource` already model the signal-source boundary.
**ATX-LOCAL:** `WeightPolicy` and `ExecutionSimulator` already sit downstream of signal generation.
**ATX-LOCAL:** `factory::Genome` represents generated expression candidates.
**ATX-LOCAL:** `library::Library` and lifecycle state carry production-state concepts.
**Implementation contract:** Store an alpha as more than a string.
**Implementation contract:** Required metadata should include `expr`, `fields`, `data_categories`, `universe`, `region`, `delay`, `decay`, `neutralization`, `lookback`, `max_weight`, and `cost_model`.
**Implementation contract:** Required research metadata should include `hypothesis_note`, `search_run_id`, `trial_count`, `batch_id`, `source_family`, and `validation_summary`.
**Implementation contract:** Required risk metadata should include factor tags such as momentum, value, size, sector, event, intraday, fundamental, options, or liquidity.
**Implementation contract:** Post-hoc explanations should be tagged as post-hoc when they were written after seeing results.

---

## 5. Alpha Design Loop
**BOOK-VERIFIED:** Chapter 1 gives five broad construction steps.
- Analyze variables in the data.
- Form an idea about the price response to a modeled change.
- Translate the change into a mathematical expression for positions.
- Test the expression.
- Submit the alpha if the result is favorable.
**BOOK-VERIFIED:** Chapter 4 expands this into design decisions around raw data, universe, prediction frequency, evaluation, and production issues.
**BOOK-VERIFIED:** Chapter 5 shows how a simple idea becomes a predictive formula and then a portfolio allocation.
**BOOK-VERIFIED:** Chapter 11 introduces the Triple-Axis Plan as a structured way to navigate alpha space.
**BOOK-VERIFIED:** Chapter 11 frames the search along axes such as ideas, datasets, parameters, regions, and universes.
**BOOK-VERIFIED:** Chapter 32 says successful quants automate routine work and focus human time on higher-value research.
**INFERENCE:** ATX should treat alpha development as a workflow object.
**INFERENCE:** Manual and generated alphas should share the same validation standard.
**INFERENCE:** A candidate can be syntactically valid while still being economically invalid.
**INFERENCE:** A candidate can be statistically attractive while still being operationally untradeable.
**INFERENCE:** A candidate can be locally strong while adding little marginal value to the pool.
**Workflow stage:** Idea capture.
**Workflow stage:** Data-card selection.
**Workflow stage:** Expression generation or manual expression writing.
**Workflow stage:** Parser/typechecker/causality validation.
**Workflow stage:** Backtest with fixed run configuration.
**Workflow stage:** OOS and robustness validation.
**Workflow stage:** Turnover/cost/risk analysis.
**Workflow stage:** Correlation-to-pool analysis.
**Workflow stage:** Admission or rejection.
**Workflow stage:** Promotion to live pool.
**Workflow stage:** Decay monitoring and retirement.
**ATX-LOCAL:** `book::BookPipeline` already composes mine, admit, promote, combine, risk, optimize, monitor, recycle, and report.
**ATX-LOCAL:** `factory::ResearchDriver` is the natural owner of idea-to-candidate generation.
**ATX-LOCAL:** `combine::AlphaGate` is the natural owner of admission thresholds.
**ATX-LOCAL:** `library::Library` is the natural owner of lifecycle transitions.
**Recommendation:** Every research run should emit both Markdown and machine-readable JSON.
**Recommendation:** The Markdown should explain the candidate in human language.
**Recommendation:** The JSON should be stable for agents, dashboards, and regression tests.

---

## 6. Data Layer
**BOOK-VERIFIED:** Chapter 6 says data is central to alpha design.
**BOOK-VERIFIED:** Price and volume data are required for basic simulation and for metrics such as return, Sharpe, and turnover.
**BOOK-VERIFIED:** Data itself can inspire alpha ideas.
**BOOK-VERIFIED:** Chapter 6 presents finding data as the first step in alpha research.
**BOOK-VERIFIED:** The chapter emphasizes sanity checks before research begins.
**BOOK-VERIFIED:** New datasets can improve performance and reduce correlation.
**BOOK-VERIFIED:** Chapter 4 lists price/volume, fundamentals, macro data, text, and multimedia as possible inputs.
**BOOK-VERIFIED:** Chapter 4 notes that data can reduce noise even when it does not create a direct directional signal.
**BOOK-VERIFIED:** Chapter 15 warns automated search against mixing too many data categories at once.
**BOOK-VERIFIED:** Chapter 15 says data categories have different frequencies and structures.
**BOOK-VERIFIED:** Examples include quarterly fundamental data, uniform price-volume data, and irregular insider-trading filings.
**BOOK-VERIFIED:** Chapter 15 emphasizes unitless and comparable variables.
**BOOK-VERIFIED:** It warns that raw price or raw earnings may not be comparable across stocks.
**BOOK-VERIFIED:** It prefers ratios or current-versus-historical comparisons when stable.
**BOOK-VERIFIED:** It warns that common ratios can diverge when denominators approach zero.
**INFERENCE:** ATX needs a first-class `DataCard`.
**INFERENCE:** Search constraints should be derived from data cards, not hardcoded after the fact.
**INFERENCE:** Automated search should prefer dimensionless transforms and reject unstable divisions unless guarded.
**INFERENCE:** Data-category breadth should be visible in scoring because broad category mixing increases overfit risk.
**DataCard field:** Vendor/source.
**DataCard field:** Field names and units.
**DataCard field:** Data category.
**DataCard field:** Update frequency.
**DataCard field:** Point-in-time availability.
**DataCard field:** Revision/restatement policy.
**DataCard field:** Missing-value policy.
**DataCard field:** Survivorship policy.
**DataCard field:** Corporate-action policy.
**DataCard field:** Cross-sectional comparability.
**DataCard field:** Known bias risks.
**DataCard field:** Default delay requirement.
**DataCard field:** Recommended transforms.
**DataCard field:** Unsafe denominator warning.
**ATX-LOCAL:** The `alpha` module supports fields and panel shapes.
**ATX-LOCAL:** The `data` and `loop` layers are natural homes for point-in-time and universe handling.
**ATX-LOCAL:** The `factory` layer should enforce data-category and unitless-ratio constraints during generation.
**ATX-LOCAL:** The `validation` layer should audit causality and survivorship.

---

## 7. Evaluation and Admission
**BOOK-VERIFIED:** Chapter 1 lists simplicity, elegance, Sharpe, stability, cross-universe performance, and cross-region performance as alpha quality traits.
**BOOK-VERIFIED:** Chapter 7 discusses information ratio and information coefficient.
**BOOK-VERIFIED:** Information ratio is excess return over variability of excess return.
**BOOK-VERIFIED:** Information coefficient is correlation between predicted and actual values.
**BOOK-VERIFIED:** Chapter 8 names PnL-derived metrics such as information ratio, return, drawdown, turnover, and margin.
**BOOK-VERIFIED:** Chapter 8 says uniqueness is evaluated through correlation with other alphas.
**BOOK-VERIFIED:** Chapter 9 warns that good in-sample results do not guarantee good out-of-sample results.
**BOOK-VERIFIED:** Chapter 9 says an out-of-sample period is necessary for validation.
**BOOK-VERIFIED:** Chapter 9 warns that the more alternatives considered, the lower the confidence in the selected model.
**BOOK-VERIFIED:** Chapter 12 defines robust alphas through universe invariance and robustness to extreme market conditions.
**BOOK-VERIFIED:** Chapter 31 lists annual PnL, aggregate PnL, Sharpe, turnover, and related WebSim diagnostics.
**INFERENCE:** A strong admission gate is a vector of tests, not a single scalar.
**INFERENCE:** Standalone Sharpe is necessary but not sufficient.
**INFERENCE:** Standalone Sharpe should be deflated for search multiplicity.
**INFERENCE:** OOS performance should dominate IS performance.
**INFERENCE:** Robustness should be checked across weaker universes, periods, sectors, and parameter perturbations.
**INFERENCE:** A candidate should be penalized when performance comes from one sector, one tail, or one lucky time slice.
**ATX-LOCAL:** `combine::compute_metrics` already supports Sharpe, returns, turnover, margin, fitness, holding, and drawdown.
**ATX-LOCAL:** `factory::fitness` already combines OOS fitness, diversification discount, robustness ratio, deflated Sharpe, and trial count.
**ATX-LOCAL:** `eval::cpcv`, `eval::pbo`, and deflated Sharpe machinery align with the book's overfit warnings.
**ATX-LOCAL:** `validation::catches_overfit_synthetic` demonstrates that overfit gates can actually reject.
**Admission gate:** Parse and type-check.
**Admission gate:** Prove no look-ahead where possible.
**Admission gate:** Confirm point-in-time field availability for selected delay.
**Admission gate:** Confirm universe membership is date-correct.
**Admission gate:** Check IS and OOS metrics separately.
**Admission gate:** Check turnover and cost-adjusted PnL.
**Admission gate:** Check max drawdown and drawdown duration.
**Admission gate:** Check margin or return per turnover.
**Admission gate:** Check correlation to pool.
**Admission gate:** Check sub-universe robustness.
**Admission gate:** Check parameter sensitivity.
**Admission gate:** Check sector and capitalization distribution.
**Admission gate:** Check alpha-value quintile behavior.
**Admission gate:** Check known factor exposure.
**Admission gate:** Check deflated Sharpe after trial-count adjustment.
**Admission gate:** Check batch-level quality, not only singleton quality.

---

## 8. Turnover and Costs
**BOOK-VERIFIED:** Chapter 7 says prediction quality is often measured under unrealistic assumptions.
**BOOK-VERIFIED:** The unrealistic assumptions include unlimited liquidity, free trading, and no other market participants.
**BOOK-VERIFIED:** Chapter 7 defines turnover as total value traded divided by book size over a period.
**BOOK-VERIFIED:** Chapter 7 ties turnover to alpha horizon and changing information.
**BOOK-VERIFIED:** Chapter 7 discusses trading costs, crossing effects, turnover control, and tuning.
**BOOK-VERIFIED:** Chapter 31 says decay smooths alpha values through a linear weighted combination of current and prior values.
**BOOK-VERIFIED:** Chapter 31 says decay can lower turnover.
**BOOK-VERIFIED:** Chapter 31 says max stock weight caps individual stock exposure.
**BOOK-VERIFIED:** Chapter 31 recommends a max stock weight range around 5% to 10% for the selected universe.
**BOOK-VERIFIED:** Chapter 31 says neutralization demeans by market, industry, or subindustry.
**BOOK-VERIFIED:** Chapter 31 defines delay-0 as using today's price and delay-1 as using yesterday's price.
**BOOK-VERIFIED:** Chapter 31 says lookback days limit the amount of prior data used in each daily evaluation.
**BOOK-VERIFIED:** Chapter 31 describes a typical WebSim run as a fictitious 20 million dollar book redistributed daily across long and short positions.
**INFERENCE:** Turnover should be both a pre-trade estimate and a post-trade result.
**INFERENCE:** Delay should be an explicit simulation contract, not an implicit expression convention.
**INFERENCE:** Decay should be a stateful signal transform when applied to final alpha output.
**INFERENCE:** Max stock weight belongs in portfolio construction, not hidden inside formula code.
**INFERENCE:** Neutralization should be visible in run configuration and reports.
**ATX-LOCAL:** `exec::ExecutionSimulator` owns fills, slippage, impact, and commissions.
**ATX-LOCAL:** `WeightPolicy` owns transforms, neutralization, gross normalization, and name caps.
**ATX-LOCAL:** `risk::capacity_curve` and cost-aware knobs own capacity and cost-aware sizing.
**ATX-LOCAL:** The `book` pipeline already wires cost calibration, capacity gross, and multi-period optimization.
**Run setting:** `delay`.
**Run setting:** `decay_days`.
**Run setting:** `max_name_weight`.
**Run setting:** `neutralization`.
**Run setting:** `lookback_days`.
**Run setting:** `book_size`.
**Run setting:** `rebalance_frequency`.
**Run setting:** `cost_model`.
**Run setting:** `universe`.
**Run setting:** `data_version`.

---

## 9. Correlation and Pool Value
**BOOK-VERIFIED:** Chapter 8 says uniqueness is evaluated by correlation with existing alphas.
**BOOK-VERIFIED:** The importance of correlation grows as the alpha pool grows.
**BOOK-VERIFIED:** Portfolio managers prefer relatively uncorrelated alphas because diversification reduces risk.
**BOOK-VERIFIED:** Chapter 15 says automated search should find seas of alphas, not just a single best alpha.
**BOOK-VERIFIED:** Chapter 15 says batch diversity can reduce correlations and portfolio risk.
**INFERENCE:** Correlation-to-pool should be both an admission gate and a score component.
**INFERENCE:** The top standalone Sharpe alphas are not necessarily the best pool additions.
**INFERENCE:** Correlation should be measured over PnL, positions, and raw signals when possible.
**INFERENCE:** PnL correlation captures realized outcome similarity.
**INFERENCE:** Position correlation captures crowded book similarity.
**INFERENCE:** Raw-signal correlation captures formula-output similarity before weighting.
**INFERENCE:** Pool correlation should be paired with factor exposure checks.
**ATX-LOCAL:** `combine::AlphaStore`, `combine::AlphaGate`, `combine::AlphaCombiner`, and `combine::CombinedSignalSource` match this book requirement.
**ATX-LOCAL:** `factory::corr_to_pool` distinguishes max and mean reduction modes.
**ATX-LOCAL:** `factory::FitnessReport` already includes redundancy, diversify, raw score, DSR, and haircut Sharpe.
**Pool diagnostic:** Max absolute PnL correlation.
**Pool diagnostic:** Mean absolute PnL correlation.
**Pool diagnostic:** Nearest-neighbor alpha ID.
**Pool diagnostic:** Cluster assignment.
**Pool diagnostic:** Marginal combined Sharpe.
**Pool diagnostic:** Marginal drawdown improvement.
**Pool diagnostic:** Marginal turnover change.
**Pool diagnostic:** Marginal capacity impact.
**Pool diagnostic:** Marginal factor exposure.
**Pool diagnostic:** Redundancy by data source, operator family, universe, and horizon.

---

## 10. Bias, Overfitting, and Robustness
**BOOK-VERIFIED:** Chapter 9 asks whether a backtest is signal or overfitting.
**BOOK-VERIFIED:** Chapter 9 says stat arb works statistically over many predictions, not through high-confidence single predictions.
**BOOK-VERIFIED:** Chapter 9 warns that computational search can turn historical artifacts into attractive false signals.
**BOOK-VERIFIED:** Chapter 10 separates systematic bias from behavioral bias.
**BOOK-VERIFIED:** Chapter 10 names look-ahead bias and data mining as major systematic biases.
**BOOK-VERIFIED:** Look-ahead bias means using data in simulation that would not have been available at the simulated time.
**BOOK-VERIFIED:** Chapter 10 discusses holdout logic as a tool against data mining.
**BOOK-VERIFIED:** Behavioral bias comes from ad hoc researcher decisions.
**BOOK-VERIFIED:** Chapter 12 defines robustness through universe stability and resilience to extreme market conditions.
**BOOK-VERIFIED:** Chapter 12 discusses robust statistics, outlier control, and distribution-aware methods.
**BOOK-VERIFIED:** Chapter 15 says automated systems need additional testing because not every generated formula can be manually inspected.
**BOOK-VERIFIED:** Chapter 15 recommends sensitivity tests and significance tests.
**BOOK-VERIFIED:** Chapter 15 recommends cross-validation across periods, durations, random subsets, and sectors.
**BOOK-VERIFIED:** Chapter 15 recommends removing or replacing input variables with noise to test contribution.
**BOOK-VERIFIED:** Chapter 32 warns that overfitting is common among both new and experienced quants.
**INFERENCE:** Robustness should be a structured result object, not only prose.
**INFERENCE:** Every candidate should be evaluated under a fixed anti-overfit policy tied to trial count.
**INFERENCE:** Parameter tuning should leave a visible audit trail.
**INFERENCE:** If OOS results guide a formula edit, that OOS region is no longer clean.
**INFERENCE:** Automated search should treat a whole run as a statistical experiment.
**ATX-LOCAL:** `validation::check_no_lookahead` implements causal truncation invariance.
**ATX-LOCAL:** `validation::check_survivorship_frozen` targets survivorship leakage.
**ATX-LOCAL:** `validation::catches_overfit_synthetic` proves overfit gates are non-vacuous.
**ATX-LOCAL:** The `eval` module supports CPCV/PBO validation.
**ATX-LOCAL:** The `factory` module applies DSR through trial-count deflation.
**Robustness test:** Weak-universe rerun.
**Robustness test:** Alternate capitalization bucket.
**Robustness test:** Sector exclusion.
**Robustness test:** Time-slice rerun.
**Robustness test:** Parameter perturbation.
**Robustness test:** Input ablation.
**Robustness test:** Noise replacement.
**Robustness test:** Stricter cost model.
**Robustness test:** Alternate neutralization.
**Robustness test:** Stricter max-name cap.
**Rejection reason:** Look-ahead risk.
**Rejection reason:** Survivorship risk.
**Rejection reason:** Trial-count deflated score too low.
**Rejection reason:** OOS collapse.
**Rejection reason:** Weak-universe collapse.
**Rejection reason:** Sector-only performance.
**Rejection reason:** Excessive turnover.
**Rejection reason:** Cost-erased PnL.
**Rejection reason:** High pool redundancy.
**Rejection reason:** Unstable denominator.

---

## 11. Risk and Drawdowns
**BOOK-VERIFIED:** Chapter 14 begins from the idea that alpha research is about returns over risk.
**BOOK-VERIFIED:** Chapter 14 says different risks require different measurements and controls.
**BOOK-VERIFIED:** Chapter 14 classifies common risks as extrinsic or intrinsic.
**BOOK-VERIFIED:** Extrinsic risks are external exposures unrelated to the alpha's source of return.
**BOOK-VERIFIED:** Examples include industry behavior, broad market behavior, known factors, events, and crowded factors.
**BOOK-VERIFIED:** Intrinsic risks are inherent in the alpha's own behavior.
**BOOK-VERIFIED:** Examples include volatility, value at risk, expected tail loss, and drawdown risk.
**BOOK-VERIFIED:** Chapter 14 says drawdowns may matter more than historical volatility for many investors.
**BOOK-VERIFIED:** Chapter 14 says drawdowns are nonlinear and easy to overfit.
**BOOK-VERIFIED:** Position-based measures can reveal exposure to securities, groups, and factor quantiles.
**BOOK-VERIFIED:** Historical-PnL-based measures are smoother but may detect regime changes slowly.
**BOOK-VERIFIED:** Chapter 14 recommends sector PnL diagnostics.
**BOOK-VERIFIED:** Chapter 14 recommends performance diagnostics across alpha-value quintiles.
**BOOK-VERIFIED:** A desirable quintile profile has positive returns in high positive alpha values and negative returns in high negative alpha values.
**BOOK-VERIFIED:** Tail-only performance implies lower breadth and higher drawdown risk.
**BOOK-VERIFIED:** Chapter 14 recommends measuring drawdown depth and duration.
**BOOK-VERIFIED:** Chapter 14 discusses bootstrapping PnL snippets to estimate drawdown risk.
**BOOK-VERIFIED:** Chapter 14 recommends diversification when possible.
**BOOK-VERIFIED:** Hard neutralization can force a risk exposure to zero.
**BOOK-VERIFIED:** Dollar-neutral and industry-neutral positions are examples of hard neutralization.
**BOOK-VERIFIED:** Soft neutralization reduces exposure without forcing exact zero.
**BOOK-VERIFIED:** Chapter 14 recommends dynamic sizing when risk rises.
**BOOK-VERIFIED:** Chapter 13 explains that formerly profitable alphas can become hedge-fund betas or risk factors.
**BOOK-VERIFIED:** Chapter 21 notes momentum can be both alpha source and unwanted risk exposure.
**INFERENCE:** ATX should compute per-alpha risk diagnostics before combination and combined-book risk after combination.
**INFERENCE:** Factor neutralization should be available at both signal and portfolio levels.
**INFERENCE:** Event-risk calendars should be able to scale or block trades.
**INFERENCE:** Drawdown controls should be validated carefully because they are easy to overfit.
**ATX-LOCAL:** `risk::factor_model`, `risk::stat_factor_model`, `risk::specific_risk`, `risk::optimizer`, `risk::capacity`, and `risk::dead_factor` align with Chapters 13-14.
**ATX-LOCAL:** `book::decay_monitor` aligns with the book's rule-retirement and cutting-losses posture.
**ATX-LOCAL:** `WeightPolicy` group neutralization aligns with hard neutralization.
**Risk diagnostic:** Name concentration.
**Risk diagnostic:** Sector and industry concentration.
**Risk diagnostic:** Country and currency exposure.
**Risk diagnostic:** Factor beta exposure.
**Risk diagnostic:** Momentum, value, size, volatility, and liquidity exposure.
**Risk diagnostic:** PnL concentration by sector and instrument.
**Risk diagnostic:** Alpha-value quintile returns.
**Risk diagnostic:** Drawdown depth and duration.
**Risk diagnostic:** Bootstrap drawdown distribution.
**Risk diagnostic:** Capacity and impact sensitivity.
**Risk control:** Dollar neutralize.
**Risk control:** Group neutralize.
**Risk control:** Factor neutralize.
**Risk control:** Gross cap.
**Risk control:** Name cap.
**Risk control:** Volatility target.
**Risk control:** Turnover penalty.
**Risk control:** Event blackout.
**Risk control:** Dynamic de-risking.
**Risk control:** Lifecycle demotion.

---

## 12. Automated Search and Machine Learning
**BOOK-VERIFIED:** Chapter 15 says the explosion of data makes manual testing insufficient.
**BOOK-VERIFIED:** Automated alpha search combines input data and trial functions at large scale.
**BOOK-VERIFIED:** It can produce thousands of alphas in a day.
**BOOK-VERIFIED:** It can also produce many in-sample noise fits.
**BOOK-VERIFIED:** Avoiding overfitting is the central automated-search problem.
**BOOK-VERIFIED:** Chapter 15 decomposes search into input data, search algorithm, and signal testing.
**BOOK-VERIFIED:** Automated search faces computational load, lack of manual inspection, and lower confidence in each alpha.
**BOOK-VERIFIED:** The chapter recommends restricting input categories.
**BOOK-VERIFIED:** It recommends comparable and unitless inputs.
**BOOK-VERIFIED:** It recommends pruning unnecessary search space.
**BOOK-VERIFIED:** It recommends iterative search from coarse grid to finer grid.
**BOOK-VERIFIED:** It recommends recording and reusing intermediate variables.
**BOOK-VERIFIED:** It connects reusable intermediate variables to genetic algorithms.
**BOOK-VERIFIED:** It recommends seas of alphas, not one best alpha.
**BOOK-VERIFIED:** It warns against increasing expression depth just to get more in-sample winners.
**BOOK-VERIFIED:** It says good alphas are usually simple.
**BOOK-VERIFIED:** It recommends breadth-based expansion over depth-based complexity.
**BOOK-VERIFIED:** It discusses the trade-off between longer backtests and changing market dynamics.
**BOOK-VERIFIED:** It recommends incremental backtest periods during iterative search.
**BOOK-VERIFIED:** It says batch-level performance can matter more than individual alpha performance.
**BOOK-VERIFIED:** It recommends rejecting whole batches when average OOS performance is too weak.
**BOOK-VERIFIED:** It recommends yield tests against noisy search spaces.
**BOOK-VERIFIED:** Chapter 16 frames machine learning around regression, classification, and clustering-style problems.
**BOOK-VERIFIED:** Chapter 16 says alpha research cares about future prediction, not perfect past description.
**BOOK-VERIFIED:** Chapter 16 highlights the complexity dilemma: too complex overfits, too simple misses structure.
**INFERENCE:** Automated search should enforce grammar, depth, data-category, and parameter budgets.
**INFERENCE:** Search should store enough rejected-candidate metadata to reconstruct trial count and search pressure.
**INFERENCE:** Search should run negative controls with noise fields and randomized labels when feasible.
**INFERENCE:** Search should evaluate productive search spaces, not only lucky candidates.
**ATX-LOCAL:** `src/factory` contains genome, mutation, crossover, parameter search, operator catalog, canonicalization, pool view, research driver, and search driver.
**ATX-LOCAL:** `parallel` and batch evaluation are natural enablers of large search.
**ATX-LOCAL:** `learn` contains model components such as ensemble, elastic net, feature matrix, HMM, and GBT sources.
**ATX-LOCAL:** The alpha DAG and bytecode VM are natural homes for intermediate-expression reuse.
**Automated search rule:** Prefer shallow expressions.
**Automated search rule:** Prefer dimensionless ratios.
**Automated search rule:** Guard or reject unstable denominators.
**Automated search rule:** Limit data-category mixing.
**Automated search rule:** Track all trials.
**Automated search rule:** Track all parameter ranges.
**Automated search rule:** Track all data families.
**Automated search rule:** Track all operators.
**Automated search rule:** Track all folds.
**Automated search rule:** Track all rejection reasons.
**Automated search rule:** Reject batches whose OOS mean is weak.
**Automated search rule:** Reject batches whose yield matches noise controls.

---

## 13. Domain-Specific Data Sources
**BOOK-VERIFIED:** Part III expands the general alpha process into concrete data and asset domains.
**BOOK-VERIFIED:** Chapter 18 covers equity price and volume.
**BOOK-VERIFIED:** Chapter 19 covers balance sheets, income statements, cash-flow statements, growth, corporate governance, negative factors, screens, and conversion to alphas.
**BOOK-VERIFIED:** Chapter 20 covers earnings, accruals, cash flow, and fundamental information.
**BOOK-VERIFIED:** Chapter 21 treats momentum as both a signal and a risk factor.
**BOOK-VERIFIED:** Chapter 22 covers news novelty, relevance, categories, expected/unexpected news, headlines/full text, no-news effects, news momentum, and social media.
**BOOK-VERIFIED:** Chapter 22 says news sentiment is often generated through NLP and machine learning and normalized into cross-sectional scores.
**BOOK-VERIFIED:** Chapter 23 covers options-derived information such as volatility skew, volatility spread, options volume, and open interest.
**BOOK-VERIFIED:** Chapter 24 covers analyst recommendations, price targets, earnings estimates, and analyst-media interactions.
**BOOK-VERIFIED:** Chapter 25 covers mergers, spin-offs, distressed assets, index-rebalancing arbitrage, and capital-structure arbitrage.
**BOOK-VERIFIED:** Chapter 26 covers intraday spread, volume, volatility, liquidity patterns, and informed-trading probability.
**BOOK-VERIFIED:** Chapter 27 covers interval alphas, volatility scaling, and intraday dollar neutrality.
**BOOK-VERIFIED:** Chapter 28 covers index alphas and index-related anomalies.
**BOOK-VERIFIED:** Chapter 29 covers ETF structure, exposures, grouping effects, and alpha-testing checklists.
**BOOK-VERIFIED:** Chapter 30 covers futures and forwards, risk-on/risk-off assets, speculator flows, and asset-group-specific horizons.
**INFERENCE:** These chapters define feature adapters more than finished strategies.
**INFERENCE:** Each domain requires a specific point-in-time store and validity model.
**Feature adapter:** Price/volume fields need adjustment policy.
**Feature adapter:** Fundamentals need filing timestamp and restatement timestamp.
**Feature adapter:** News needs publication timestamp, entity link, sentiment model, and novelty score.
**Feature adapter:** Social media needs source, filtering policy, entity link, and aggregation window.
**Feature adapter:** Options need moneyness, expiry bucket, implied-vol model, and liquidity filters.
**Feature adapter:** Analyst data needs broker, analyst, report type, target horizon, recommendation scale, and revision semantics.
**Feature adapter:** Events need announcement time, expected date, actual date, status, and cancellation handling.
**Feature adapter:** Intraday data needs venue, timestamp convention, session, spread, and bar construction.
**Feature adapter:** Index data needs announcement date, effective date, membership, weights, and buffer rules.
**Feature adapter:** ETF data needs NAV timestamp, holdings timestamp, underlying market status, and creation status.
**Feature adapter:** Futures need contract ID, continuous-roll rule, expiry, open interest, and asset group.

---

## 14. WebSim as Product Spec
**BOOK-VERIFIED:** Chapter 31 introduces WebSim as a financial market simulation platform.
**BOOK-VERIFIED:** WebSim lets users implement and test ideas through simple expressions.
**BOOK-VERIFIED:** WebSim stores historical data and predefined mathematical operators.
**BOOK-VERIFIED:** WebSim is used by research consultants and as an educational tool.
**BOOK-VERIFIED:** Key settings include delay, decay, max stock weight, neutralization, and lookback days.
**BOOK-VERIFIED:** Delay controls whether today's or yesterday's prices are used.
**BOOK-VERIFIED:** Decay linearly combines current and previous alpha values.
**BOOK-VERIFIED:** Max stock weight caps individual stock position weights.
**BOOK-VERIFIED:** Neutralization groups and demeans by market, industry, or subindustry.
**BOOK-VERIFIED:** Lookback days controls how much prior data each daily evaluation uses.
**BOOK-VERIFIED:** WebSim performs a backtest after expression and parameters are entered.
**BOOK-VERIFIED:** WebSim redistributes capital daily to long and short positions according to processed alpha values.
**BOOK-VERIFIED:** Results include PnL charts and numeric metrics.
**BOOK-VERIFIED:** Users evaluate profitability, returns, Sharpe, turnover, and distribution charts.
**BOOK-VERIFIED:** Visualizations help confirm distributions by capitalization, industry, or sector.
**BOOK-VERIFIED:** Passing thresholds can lead to out-of-sample testing.
**INFERENCE:** WebSim is the clearest product specification in the book.
**INFERENCE:** ATX should expose a WebSim-like run preset or report mode.
**INFERENCE:** The platform value is not only fast computation; it is constrained, comparable research.
**ATX-LOCAL:** The local DSL operator set is already broad enough for a WebSim-style expression surface.
**ATX-LOCAL:** The `book` pipeline could generate WebSim-style summary tables from engine-native results.
**WebSim preset:** Daily frequency.
**WebSim preset:** Explicit region/universe.
**WebSim preset:** Explicit delay.
**WebSim preset:** Explicit decay.
**WebSim preset:** Explicit neutralization.
**WebSim preset:** Explicit max-name cap.
**WebSim preset:** Explicit lookback.
**WebSim preset:** Explicit book size.
**WebSim preset:** Explicit cost model.
**WebSim preset:** PnL chart data and summary metrics.
**WebSim preset:** Sector, industry, capitalization, and alpha-quintile distributions.
**WebSim preset:** OOS, pool-correlation, and risk-factor status.

---

## 15. Mapping to atx-engine
**ATX-LOCAL:** `include/atx/engine/alpha` contains the expression substrate.
**ATX-LOCAL:** `alpha/registry.hpp` defines the operator ISA, shape lattice, dtype lattice, and built-in signatures.
**ATX-LOCAL:** The registry includes element-wise arithmetic, comparisons, cross-sectional ops, group ops, time-series ops, stateful recurrence, filters, and multi-output ops.
**ATX-LOCAL:** `src/alpha` contains lexer, parser, typecheck, DAG, bytecode, VM/oracle, panel, and unparse components.
**ATX-LOCAL:** `include/atx/engine/factory` and `src/factory` contain search/generation.
**ATX-LOCAL:** The factory layer includes genome, mutation, crossover, canonicalization, parameter search, op catalog, research driver, and search driver.
**ATX-LOCAL:** `factory/fitness.hpp` encodes pool-aware fitness as marginal contribution rather than standalone Sharpe.
**ATX-LOCAL:** `include/atx/engine/combine` owns store, gate, metrics, combiner, and combined source.
**ATX-LOCAL:** `include/atx/engine/eval` owns evaluation methods such as CPCV/PBO/performance metrics.
**ATX-LOCAL:** `include/atx/engine/validation/bias_audit.hpp` provides no-look-ahead, survivorship, and overfit proof checks.
**ATX-LOCAL:** `include/atx/engine/loop` owns backtest loop interfaces and signal-source boundaries.
**ATX-LOCAL:** `include/atx/engine/exec` owns execution simulation.
**ATX-LOCAL:** `include/atx/engine/risk` owns covariance, factors, optimizer, capacity, dead factors, shrinkage, specific risk, and regimes.
**ATX-LOCAL:** `include/atx/engine/book` owns allocation, decay monitor, pipeline, and report.
**ATX-LOCAL:** `include/atx/engine/learn` and `src/learn` own machine-learning components.
**ATX-LOCAL:** `include/atx/engine/parallel` and batch evaluation map to Chapter 15's scale problem.

| Book concept | ATX home | Action |
|---|---|---|
| Alpha as data-to-forecast function | `alpha`, `loop` | keep expression and config together |
| Data sanity and data categories | `data`, `factory`, `validation` | add DataCard metadata |
| Turnover and cost realism | `exec`, `cost`, `risk` | gate on net metrics |
| Correlation-to-pool | `combine`, `factory` | keep as admission primitive |
| Overfit controls | `eval`, `validation` | expose in run reports |
| Robustness checks | `factory`, `validation` | standardize rerun battery |
| Factor and drawdown risk | `risk`, `book` | add diagnostics to reports |
| Automated search | `factory`, `parallel` | batch-level yield reporting |
| WebSim settings | `book`, `loop`, `exec` | add preset/report mode |
| Lifecycle/decay | `library`, `book` | monitor and retire alphas |

**Design principle:** Keep expression semantics separate from portfolio construction.
**Design principle:** Keep candidate generation separate from validation.
**Design principle:** Keep signal scoring separate from pool admission.
**Design principle:** Keep data provenance separate from formula syntax.
**Design principle:** Keep causal data availability explicit.
**Design principle:** Keep cost model configuration explicit.
**Design principle:** Keep risk controls visible in reports.
**Design principle:** Keep rejected-candidate evidence.
**Design principle:** Keep run hashes stable and reproducible.

---

## 16. Implementation Backlog

### 16.1 Research Cards
- Add a `ResearchCard` artifact for every admitted alpha.
- Include expression, hypothesis, data fields, data categories, and source.
- Include universe, region, delay, decay, neutralization, max name weight, book size, and cost model.
- Include IS/OOS periods, fold geometry, trial count, and batch ID.
- Include gates passed, gates failed, and rejection reasons.
- Include "why this may be real" and "why this may fail" notes.
- Include nearest pool neighbor and correlation values.
- Include risk exposures and drawdown diagnostics.

### 16.2 Data Cards
- Add a `DataCard` artifact for each field family.
- Include category, units, update frequency, point-in-time timestamp, vendor, and revision policy.
- Include missing-value, survivorship, and corporate-action policy.
- Include whether raw cross-sectional comparison is valid.
- Include default delay, default lookback, recommended transforms, and unsafe denominator warnings.
- Use DataCards to constrain automated search.

### 16.3 Search Governance
- Add data-category budget.
- Add expression-depth budget.
- Add free-parameter budget.
- Add unstable-division detection.
- Add unitless-ratio preference.
- Add intermediate-variable cache statistics.
- Add operator-family and data-family yield statistics.
- Add negative-control yield tests.
- Add randomized/noise input checks.

### 16.4 Batch Admission
- Add batch summary reports.
- Track candidate count, admitted count, rejected count, and rejection histogram.
- Track IS/OOS mean, median, dispersion, and best-vs-median gap.
- Track batch turnover, pool correlation, and internal redundancy.
- Track yield versus noise controls.
- Reject entire batches when batch OOS quality is too weak.

### 16.5 Robustness Battery
- Add weak-universe reruns.
- Add alternate-cap-bucket reruns.
- Add sector-exclusion reruns.
- Add period-slice reruns.
- Add parameter-perturbation reruns.
- Add input-ablation reruns.
- Add noise-replacement reruns.
- Add stricter-cost reruns.
- Add alternate-neutralization reruns.
- Add stricter-name-cap reruns.

### 16.6 WebSim Report Mode
- Serialize delay, decay, max stock weight, neutralization, universe, lookback, book size, and cost model.
- Show gross and net PnL.
- Show Sharpe, turnover, return, margin, drawdown, capacity, and cost.
- Show sector, industry, capitalization, and alpha-quintile distributions.
- Show OOS, pool-correlation, and factor-exposure status.
- Show promotion eligibility.

### 16.7 Risk Diagnostics
- Add per-alpha sector PnL contribution.
- Add factor-quantile contribution.
- Add alpha-value quintile returns.
- Add drawdown-duration table.
- Add bootstrap drawdown estimate.
- Add dynamic risk-size recommendation.
- Add event-risk exposure tags.
- Add post-neutralization residual exposure report.

### 16.8 Feature Adapter Roadmap
- Price/volume field cards.
- Fundamental PIT filing adapter.
- News/sentiment adapter.
- Analyst-data adapter.
- Event-calendar adapter.
- Options-derived feature adapter.
- Index-membership adapter.
- ETF holdings/NAV adapter.
- Futures continuous-contract and open-interest adapter.
- Intraday microstructure adapter after the daily path is fully audited.

---

## 17. Agent Indexing Notes
**Primary tags:** `finding-alphas`, `worldquant`, `alpha-factory`, `websim`, `alpha-design`, `automated-search`, `overfitting`, `turnover`, `risk`, `drawdown`, `correlation`, `atx-engine`.
**Best source chapters for alpha definition:** 1, 2, 4, and 31.
**Best source chapters for validation:** 7, 8, 9, 10, 12, 14, and 15.
**Best source chapters for automated research:** 11, 15, 16, 17, and 32.
**Best source chapters for data families:** 18 through 30.
**Best source chapter for WebSim settings:** 31.
**Best ATX modules for expression work:** `include/atx/engine/alpha`, `src/alpha`.
**Best ATX modules for factory work:** `include/atx/engine/factory`, `src/factory`.
**Best ATX modules for pool work:** `include/atx/engine/combine`.
**Best ATX modules for validation work:** `include/atx/engine/eval`, `include/atx/engine/validation`.
**Best ATX modules for risk work:** `include/atx/engine/risk`.
**Best ATX modules for end-to-end operation:** `include/atx/engine/book`, `include/atx/engine/loop`, `include/atx/engine/exec`.
**Search phrase:** `WebSim preset`.
**Search phrase:** `batch admission`.
**Search phrase:** `DataCard`.
**Search phrase:** `ResearchCard`.
**Search phrase:** `correlation-to-pool`.
**Search phrase:** `weak-universe robustness`.
**Search phrase:** `alpha-value quintile`.
**Search phrase:** `negative-control yield`.
**Index anchor:** `alpha-definition` -> Chapters 1, 2, 4, 31.
**Index anchor:** `data-provenance` -> Chapters 4, 6, 15, 18-30.
**Index anchor:** `turnover-cost` -> Chapters 7 and 31.
**Index anchor:** `pool-correlation` -> Chapters 8 and 15.
**Index anchor:** `overfit-control` -> Chapters 9, 10, 12, 15, 32.
**Index anchor:** `risk-drawdown` -> Chapters 13, 14, 21.
**Index anchor:** `automated-search` -> Chapters 11, 15, 16, 17.
**Index anchor:** `websim-settings` -> Chapter 31.
**Index anchor:** `feature-adapters` -> Chapters 18-30.
**Index anchor:** `lifecycle-decay` -> Chapters 3, 14, 32.
**Index anchor:** `atx-alpha` -> `include/atx/engine/alpha`.
**Index anchor:** `atx-factory` -> `include/atx/engine/factory`.
**Index anchor:** `atx-combine` -> `include/atx/engine/combine`.
**Index anchor:** `atx-validation` -> `include/atx/engine/validation`.
**Index anchor:** `atx-risk` -> `include/atx/engine/risk`.
**Index anchor:** `atx-book` -> `include/atx/engine/book`.

---

## 18. Caveats and References
1. The extracted text contains PDF/OCR artifacts, especially near figure-heavy sections.
2. PDF page markers do not always match printed book page numbers.
3. Chapter numbers are therefore safer references than extracted page numbers.
4. WebSim settings in the second edition may differ from current WorldQuant BRAIN/WebSim settings.
5. The report treats the book as historical source material and does not claim current WorldQuant operational facts.
6. ATX implementation mapping is engineering interpretation, not a claim made by the book.
**Reference:** Igor Tulchinsky et al., eds., *Finding Alphas: A Quantitative Approach to Building Trading Strategies*, Second Edition, WorldQuant Virtual Research Center / Wiley, local extracted text file: `C:\Users\natha\OneDrive\Desktop\books\Finding Alphas - A Quantitative Approach - 2nd ed - extracted.txt`.
**Reference:** Existing local `atx-engine` research convention observed from reports in `C:\Users\natha\OneDrive\Desktop\atx\atx-engine\research`.
**Reference:** Local `atx-engine` module layout observed under `include/atx/engine`, `src`, `tests`, `bench`, and `plans` on 2026-06-13.

# Wave 2 — toward real alpha generation

Integration branch `feat/quant-platform-swarm-20260922` @ 8c42689a (C:\atx-wt\pool-1) = Lane 0 + lanes 3,4,5,8 merged.
Every lane branch has been synced with it. Known pre-existing failure (fails on C:\atx main build too, not a merge regression):
`StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` — "invalid stod argument".

Real-data context (read-only, never modify): `C:\atx\data\` (14 GB of identified ORATS equity panels, yearly contexts
`equity_scorecard16_ctx_<year>_t1000|t3000_*`, IC/baseline/book artifacts through checkpoint 22), atx-db warehouse
(`C:\atx\atx-db`, point-in-time fundamentals: `fundamental_statement_points`, `fundamental_pit_snapshot`, EPS bridge).
History of prior real-data cycles and their discipline (2020-01-01 validation seal, declared-N trial accounting,
R16-8 clearance rule, liquidity floor, identity manifests): `atx-engine/docs/PLATFORM_PROGRESS.md` (checkpoints 16-22),
`atx-impl/docs/EQUITY_BOOK_BASELINE.md`, `atx-impl/src/stage_equity_*.cpp`, `atx-vol/docs/LEDGER.md` if present.
Prior finding: hand-picked single families (momentum, amihud, sector-neutral mom, etc.) did NOT clear R16-8 on 2013-2019.
Real alpha must come from breadth: a large DSL zoo mined with honest multiple-testing control, then combined.

## Lane 9 (l9-realmine) — real-data DSL alpha mining stage

Goal: `atx-impl` stage that mines the alpha DSL zoo on the REAL identified panel and emits an admitted alpha library
with honest statistics, using the new platform pieces:
- factory search driver with Lane 3 multi-fidelity racing + semantic canon + output fingerprint dedup + sketch novelty,
- Lane 4 `TrialRegistry` (every evaluated genome recorded; N_eff; deflated Sharpe from registry summary),
  Benjamini-Yekutieli / Romano-Wolf gate on the validation block,
- seed population = WQ101 fixture (`atx-impl/tests/fixtures/alpha101.txt`) + literature families + random genomes.
Protocol: train years (e.g. 2013-2016) for search fitness (net of a simple per-turnover cost), validation years
(2017-2018) for admission only, 2019 untouched holdout reported once at the end; never read >= 2020-01-01.
Universe: t1000 liquidity-floored first (memory! 16 GB machine shared with other lanes; stream by year, cap RSS,
use existing memory guards). Output: fresh `C:\atx\data\equity_mine_l9_<date>/` artifact dir with manifest (hash-bound,
published last), library of admitted expressions, per-alpha IC/ICIR/turnover/decay, trial registry, gate report.
Deliver: stage code + tests on a synthetic panel (planted alpha recovered, null panel admits ~0 after FDR) + ONE real
run with results table. Report honestly even if nothing clears.
Owned: new `atx-impl/src/stage_equity_mine.{hpp,cpp}`, its dispatch registration line, one append line in
atx-impl/CMakeLists.txt, new tests `atx-impl/tests/stage_equity_mine_*_test.cpp`. Read-only for everything else;
if an engine change is needed, make it header-only in a NEW engine header you own, or list it as integration note.

## Lane 10 (l10-fundzoo) — PIT fundamental fields + fundamental/quality alpha zoo

Goal: make point-in-time fundamentals usable as DSL `$fields` and seed a literature zoo over them.
- Engine-side: new header-only `atx-engine/include/atx/engine/data/fundamental_fields.hpp`: build aligned
  date x instrument panels from warehouse PIT points keyed by available-at (NOT period end) with explicit lag,
  forward-fill with staleness cap, security-id mapping to the identified panel axes; derived fields
  (book_to_price, earnings_yield, sales_yield, roe, roa, gross_profitability (Novy-Marx), accruals (Sloan),
  asset_growth (Cooper-Gulen-Schill), net_issuance, leverage, eps_surprise/SUE, eps_revision if available).
- Export tool (Python OK, under `atx-engine/tools/` or `atx-db` read-only query) that materializes those panels
  for 2012-2019 into a fresh `C:\atx\data\equity_fund_fields_l10_<date>/` with manifest + availability audit
  (coverage %, lag distribution, staleness).
- Zoo: `atx-impl/tests/fixtures/fundamental_zoo.txt` — 40-80 DSL expressions from literature (value, quality,
  profitability, investment, accruals, PEAD/SUE, composites like QMJ-style, sector-neutral variants) with citations
  in comments. Use WebSearch for exact definitions.
- Evaluate the zoo through the existing equity IC stage on real 2013-2018 (never >= 2020; 2019 held out) and report
  IC/ICIR/turnover table + correlation to momentum. Honest reporting.
Owned: the new header + its test `atx-engine/tests/data/fundamental_fields_test.cpp`, tool, fixture, report under
`atx-engine/reviews/2026-09-23-fundamental-zoo.md`. Do not edit CMakeLists (header-only; tests auto-globbed).

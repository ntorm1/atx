# Task P2: web literature review for the production L/S platform (read-only + web)

**Where:** read C:/atx-wt/pool-2 by absolute path (context only). No edits to code, no builds, no real data, no subagents,
never touch C:/atx. Write: C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/literature-v7.md (deliverable) and
task-P2-report.md (<= 15 lines). Reply in chat < 10 lines. Use WebSearch/WebFetch; cite author-year-venue and a URL per
claim; prefer peer-reviewed or SSRN working papers and primary sources; mark practitioner blogs as such.

**Context, read first:** .superpowers/sdd/mega-alpha-20260926/v6-literature.md (542 lines, our existing per-theme alpha
review; do NOT redo it -- extend), docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md (process and the final numbers:
S2 net Sharpe +1.239 on TRAIN 2020-2022, 32 admitted sub-alphas equal-weighted by theme, aim-partial-v5 heuristic
construction: partial aim theta .05, dust .1, delta orders, exit rate .05, cost model linear 6 bps + sqrt impact + 1% ADV cap
+ swap financing), docs/plans/2026-09-28-mega-alpha-data-request-atx-db.md (data we have asked for), and
.superpowers/sdd/mega-alpha-20260926/v4-prereg.md (our statistics protocol).

**Questions to answer, each with concrete "implement X because Y, expected effect Z" recommendations for a daily-rebalanced,
low/medium-frequency, high-capacity equity L/S book:**
1. Alpha combination beyond equal weight: linear stacking with shrinkage, IC-weighted, mean-variance of alpha returns
   (Grinold-Kahn, Qian-Hua-Sorensen), ML combination (Gu-Kelly-Xiu 2020 RFS; Kelly-Malamud-Zhou 2024 JF virtue of
   complexity; Chen-Pelger-Zhu 2024), DeMiguel-Garlappi-Uppal 2009 caveat, when does fitting beat 1/N at N~30 and T~750 days.
2. Portfolio construction with transaction costs and turnover control: Garleanu-Pedersen 2013 (aim portfolio, trading rate),
   Collin-Dufresne et al. liquidity-adjusted, multi-period MPC, cost-aware mean-variance with factor risk model, position
   limits, borrow constraints; how our aim-partial theta/exit-rate maps onto GP optimal trading rate; what a proper
   optimiser adds over the heuristic (cite evidence: Frazzini-Israel-Moskowitz 2018 Trading costs, Novy-Marx-Velikov 2016
   RFS cost mitigation, DeMiguel-Martin-Utrera-Uppal 2020 transaction-cost-aware factor investing).
3. Risk models: Barra-style fundamental factor models vs statistical (PCA/APCA) vs shrinkage (Ledoit-Wolf linear and nonlinear
   2020; Ledoit-Wolf 2022 quadratic shrinkage), Fan-Liao-Mincheva POET, Bun-Bouchaud-Potters RIE; what a small shop
   should build for ~3000 names daily; ex-ante risk vs realised; industry+style neutralisation vs full factor hedge.
4. Capacity and market impact: Almgren et al. 2005, Kyle-Obizhaeva 2016 invariance, Frazzini-Israel-Moskowitz 2018 realised
   costs, Bucci et al. 2019 crossover impact; how to estimate capacity of a book given ADV participation; where our 1% ADV cap
   and sqrt model sit relative to realised institutional costs.
5. Backtest integrity for a platform: Bailey-Borwein-Lopez de Prado-Zhu deflated Sharpe and PBO, CPCV (Lopez de Prado 2018),
   Harvey-Liu-Zhu 2016 t > 3, Harvey-Liu 2015 backtesting; multiple-testing accounting when the library grows to 100+; what
   an alpha research platform at scale does (WorldQuant 101 alphas Kakushadze 2016; alpha factory descriptions; Tulchinsky
   Finding Alphas 2nd ed.): signal standardisation, decay/turnover metrics, correlation-vs-pool admission thresholds,
   sub-universe tests, the alpha fitness metrics they report.
6. Alpha families we do not have that fit our data horizon and low turnover (extend v6-literature S4 missing families):
   13F-based (Cohen-Polk-Silli best ideas; Agarwal et al. confidential holdings; institutional demand changes), earnings
   calendar / expected-announcement drift (Savor-Wilson; Barber et al. 2013 announcement premium), options-implied (Bali-
   Hovakimian 2009 skew, Cremers-Weinbaum 2010 IV spread, An et al. 2014), borrow-cost/utilisation (Drechsler-Drechsler 2016
   shorting premium; Beneish et al. 2015), insider trading (Cohen-Malloy-Pomorski 2012 routine vs opportunistic), low-frequency
   fundamental quality/investment (Hou-Xue-Zhang q5; Asness-Frazzini-Pedersen QMJ), plus an assessment of which of the
   Chen-Zimmermann (2022) / Jensen-Kelly-Pedersen (2023) replicated anomalies survive post-2004 at monthly turnover and are
   buildable from SEC XBRL + CRSP-like bars.
7. Live operation: what production quants monitor daily (ex-ante vs realised risk, IC/turnover drift, fill quality, slippage
   vs model), and how they decide to retire or reweight alphas (alpha decay literature: McLean-Pontiff 2016; Chordia et
   al. 2014).

**Output format:** S0 one-page summary table (topic, top recommendation, expected effect on Sharpe/capacity/turnover,
confidence, effort); S1-7 per topic: 1 paragraph state of the art, citations, concrete recommendation(s) for our platform
with parameters; S8 ranked build list combining all topics. Under 500 lines.

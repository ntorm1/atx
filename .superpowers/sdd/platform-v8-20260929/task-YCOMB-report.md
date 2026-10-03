# Task YCOMB report: platform v8 Y, construction and combination rules

Lane YCOMB. Worktree `C:/atx-wt/pool-14`, branch `feat/platform-v8-ycomb-20261002`, base `798d3b23`.
**Status: DONE_WITH_CONCERNS.** Four rules are registered and committed: Y-1, Y-2, Y-3 and Y-5. Y-5 is wired end to
end after PM8-10 (section "Y-5 wiring"), with its core in C++ per PM8-12. Y-4 is deliberately left unused. None of the
C++ is built (lane rule). Everything here was registered blind: no 2020-2023
return, IC, Sharpe or NAV output was opened, and nothing dated 2024-01-01 or later was opened. The only results quoted
are the PM's public verdict lines in progress.md.

| commit | rule | files |
|---|---|---|
| `47d6afd9` | Y-1 vol-target-v1 | engine `book/vol_target.hpp` + test; impl `strategy_vol_target.{hpp,cpp}`, `strategy_risk_target.{hpp,cpp}`, `strategy_nav_v7.cpp`; test `strategy_vol_target_test.cpp`; template `y-vol-target.json` |
| `02633038` | Y-3 norm-score-v1 | engine `book/normal_score.hpp` + test; impl `strategy_target_replay.{hpp,cpp}`, `strategy_nav_replay.cpp`, `strategy_nav_v7.cpp`; test `strategy_norm_score_test.cpp`; template `y-norm-score.json` |
| `43745dd7` | Y-2 theme-tsmom-v1 | impl `strategy_ic_theme_tsmom.{hpp,cpp}`, `strategy_ic_composition.{hpp,cpp}`, `strategy_ic_admission.cpp`, `strategy_ic_runner.cpp`, `strategy_ic_detail.hpp`; tests `strategy_ic_theme_tsmom_test.cpp`, `strategy_ic_runner_test.cpp` (appended); fitter `composition_theme_tsmom.py` + test, `fit_composition_weights.py` hook; fixture `tests/fixtures/theme_tsmom_v1.json`; template `y-theme-tsmom.json` |
| `0fbcb230`, `e2ac7d63` | Y-5 two-speed-v1 (kernel, then wiring) | engine `book/two_speed.hpp` + `book_two_speed_test.cpp` |

## Ranking: prior of a gain in net Sharpe and gross return

1. **Y-3 norm-score-v1** (concentration). Under the 2.0 leverage cap, this is the only way the same gross can earn
   more. It adds no constant. The expected gross return per unit of gross rises. Net Sharpe is uncertain because the
   tails cost more to trade and to borrow.
2. **Y-2 theme-tsmom-v1** (theme weighting). Factor momentum is one of the strongest time-series effects in factor
   returns (Ehsani-Linnainmaa 2022). It uses expected-return information without fitting any mean. The doubt is the
   evidence base: 12 themes, about 36 blocks, and 2020 left to the parent.
3. **Y-5 two-speed-v1** (multi-horizon, PM8-5). Its prior is positive on gross alpha from the fast themes, but it pays
   for that with turnover. It cannot run until it is wired.
4. **Y-1 vol-target-v1** (book vol target). The Moreira-Muir form of X-10. The prior Sharpe gain is about 0 (R-8's
   public result: dSR -.011). Return is below X-10's and drawdown is lower.

Y-4 is unused (see the last section).

## Y-1 vol-target-v1 (book-level volatility target; Moreira and Muir 2017, Harvey et al. 2018, Cederburg et al. 2020)

- **Rule.** `L_t = clip(L x sigma_ref / sigma_hat, 1.0, L)`, re-estimated every 21 sessions.
  - `L` is the cap, which is the executable maximum `--aim-leverage`. Under X-10 that is 2.0.
  - `sigma_hat` is the book's ex-ante annualised vol from the risk store (`gross_one_vol` of the planned weights,
    atx-risk-v1, point in time).
  - `sigma_ref` is the running mean of the book's own `sigma_hat` estimates, current one included. This is the
    real-time normalisation of Cederburg et al. 2020, so there is no absolute vol constant to choose.
  - The floor is 1.0.
  - The cap holds until the first estimate. Smoothing is the cadence plus aim-partial-v5's theta; no other filter.
- **Wiring.** `nav --vol-target vol-target-v1 --risk-model <store> --risk-model-sha256 <pin>` runs as a second law on
  the R-8 scaler. It needs aim-partial-v5. It is refused with `--risk-target`, spo, v6, or no store.
- **Composition with X-10.** Y-1 is X-10's leverage, risk-managed. It uses the same parent (X-F0) and the same
  nominal L of 2.0. The template's nav.leverage is "2.0".
- **Gross matching (PM6-6) does not apply.** This is a leverage rule (PM7-20 reading B). The realised gross limit is
  [1.0, 2.0] x G_parent / L_parent, ±.005.
- **Acceptance.** Paired S2 net dSR > 0 against X-10, AND X-10's own leverage-cell rule against X-F0 (net annual
  return higher, and S2 net Sharpe not lower by more than .100), AND mechanics. If accepted, Y-1 replaces X-10 as X-F.
  Capacity is printed only.
- **Look-ahead surfaces.**
  - The risk store's point-in-time contract (lane RISK; the store's SHA is pinned).
  - `sigma_ref` averages only estimates made up to the current decision.
  - The 21-session clock is anchored to the first decision. It does not depend on any outcome.
- **Root verification.**
  - `VolTarget.*` and `BookVolTarget.*`.
  - `RiskTarget.*` unchanged, which shows flag absent is byte-identical: every risk-target spelling, CSV header, and
    rule id stays as it was.
  - Re-run the parent's NAV argv; the bytes must be identical.

## Y-2 theme-tsmom-v1 (theme weighting by factor momentum; Ehsani and Linnainmaa 2022, Gupta and Kelly 2019)

- **Not covered by R-10, R-11 or theme-erc-v1.** R-10 shrinks member ICs, R-11 orthogonalises themes, and theme-erc-v1
  sets static theme shares from second moments. All three fix weights once from TRAIN. Y-2 changes theme masses
  through time, walk-forward, from each theme's own trailing return.
- **Registration.** Every constant comes from the paper's 12-1 formation and one-month holding.
  - Theme sleeves: `r_t(d) = sum_{k in t} (w_k / W_t) s_k f_k(d)` over the parent fit's role decisions.
    - `w` is the parent's final weights: X-5's ERC weights after the cap.
    - `f_k(d)` is the fitter's factor series, formed at decision d and realised at d + 2; a flat decision counts as 0.
  - Block starts: decision `j = 252 + 3 - 1 + 21 b`.
  - Trailing sum: `T_t(j) = sum_{d = j-254}^{j-3} r_t(d)`.
  - On/off flag: `g_t = 1[T_t > 0]`.
  - Masses:
    - If every g is 1, or every g is 0, the block keeps the parent's `W_t` verbatim.
    - Otherwise `W'_t = g_t W_t S / K`, where S = sum W and K = sum g W, summed in ascending theme order.
    - Before decision 254 (about 2020), the parent's masses apply.
  - Unchanged from the parent: members, weights, signs, theme_standardise, provenance.rule, and the per-date re-rank.
- **Split of work.**
  - The fitter (`--theme-tsmom theme-tsmom-v1` on the parent's fit argv) writes `theme_schedule {rule, lookback 252,
    lag 3, step 21, themes, blocks: [{from_session, trailing}]}` and `provenance.theme_tsmom`. That provenance records
    theme_on_fraction, theme_blocks_off and per-block masses (report only).
  - The IC runner refuses any constant other than the registered ones. It recomputes the masses from the recorded
    sums, using the kernel `theme_tsmom_masses`. Python and C++ match bit for bit on the shared fixture.
  - Each block is placed at the first role session at or after `from_session`. A later role keeps the last block.
  - The composition multiplies the unchanged per-date re-rank by the mass in force. A zero mass adds nothing.
  - Recorded in: recipe `composition_schedule`, combined manifest `composition_schedule`, and summary
    `composition_weights.schedule`.
- **Python path.** `composition_theme_tsmom.py`.
  - Parents: ew-theme-std-v1 or ic-shrink-v1, with or without `--theme-erc`.
  - Refused with `--theme-resid` (residualised composites are not the measured sleeves) and under `--era`.
- **Template.** `y-theme-tsmom.json`, nominal parent `x-theme-erc.json`, parent = X-F0's fit. It renames the outputs
  downstream of the fit (fit, card, w, nav, monitor).
- **Gross matching (PM6-6) applies.** The rule changes theme shares, not scale.
- **Acceptance (PM7-34 default).** Paired S2 net dSR > 0 against the parent AND mechanics.
  - Printed only: theme_blocks_off, theme_on_fraction, turnover per unit gross, cost per traded dollar, and net Sharpe
    at 4x.
- **X-5's in-sample qualification is not removed.** X-5's ERC shares come from the TRAIN = S2 covariance. Y-2 inherits
  those shares, and its paired dSR against X-5 measures only the schedule's increment.
  - A walk-forward ERC was not proposed. Against X-5 its expected dSR is negative for a purely mechanical reason: X-5
    already uses the S2 covariance. A negative result would therefore say nothing new.
  - The OD-3 read settles X-5 out of sample.
- **Look-ahead surfaces.**
  1. `f_k(d)` is realised at d + 2. The window ends at j - 3, which leaves one session of slack.
     `Windows.test_no_term_realised_after_the_block` perturbs every term after j - 3 and shows the block does not
     change. It also shows the term at j - 3 is read.
  2. The weights file is written after the window closes, but each block reads only the sign of sums realised before
     it. There is no in-sample mean.
  3. The parent's weights, signs and admission are TRAIN in-sample. These are inherited, not added.
  4. No term reads past the TRAIN role. The last block reads at most decision n - 4.
- **Root verification.**
  - `ThemeTsmom.*`: kernel closed form, the shared fixture, a schedule repeating the parent's masses giving the same
    blend bit for bit, a block switching a theme off from its first date only, and refusals.
  - `ThemeTsmomRunner.*`:
    - A block whose sums are all positive gives the parent's combined bytes, targets and `__combined__` rows.
    - A block that switches value off gives the parent's rows before session 400 and the price_momentum-only rows
      from 400 on, bit for bit. Every validation row is the price_momentum-only one.
    - The recipe differs only by `composition_schedule`.
    - Refusals happen before any payload is read.
  - Regression: `CompositionV8.*`, `ThemeResidRunner.*`, `ThemeErcV1.*`, `StrategyIcRunner.*`.
  - Identity: re-run the parent's fit argv. composition_weights.json and admission.json must match except
    script_sha256. Re-run the parent's w argv; it must be byte-identical.

## Y-3 norm-score-v1 (concentration; Grinold and Kahn ch. 14, van der Waerden 1952)

- **Rule.** On each rebalance, each member's centred tied rank is replaced by `z = Phi^{-1}(u)` before the demean.
  `u = (b + e + 1) / (2(N + 1))` for the tie block [b, e). After that, the parent's demean, gross 1, locate zeroing,
  price-risk-v1 neutralisation and trading rule apply unchanged. No free constant.
  - At N = 1,850 the largest |z| is about 3.3, against a mean of .8. For the uniform rank the figures are .5 and .25.
- **Wiring.** `nav --rank-shape norm-score-v1` requires aim-partial-v5. It is refused with `--hold-band`,
  `--vol-scale`, and spo-v1/v2.
- **Gross matching (PM6-6) applies.** The rule changes shape, not scale.
- **Judging criterion of the hypothesis (printed, decides nothing).** S2 gross-of-cost annual return per unit of
  all-rows gross is above the parent's.
- **Acceptance (PM7-34 default).** Paired S2 net dSR > 0 AND mechanics.
- **Look-ahead surfaces.** None beyond the parent: the score is a deterministic map of the decision-date blend rank.
- **Root verification.**
  - `NormScore.*` and `BookNormalScore.*`.
  - Flag absent is byte-identical: `NormScore.DesiredIsTheDemeanedGrossOneNormalScore` checks the tied-rank target
    bits. Also run `StrategyNavReplay.*` and `StrategyTargetReplay.*`.

## Y-5 two-speed-v1 (PM8-5 multi-horizon; Garleanu-Pedersen 2013, Grinold 2010, Qian-Sorensen-Hua 2007, Boyd et al. 2017)

**R-3 already was this rule.** It put each signal's own decay into the aim:

- `task-R-3-brief.md` line 9: "Rule: `ew-theme-aim-v1` gains, g_k = theta x sum over j of (1 - theta)^j rho_k(j), from
  rank autocorrelation only, applied on top of the R-1 weights; theta .05".
- `fit_composition_weights.py` `aim_gain`: "g = theta * sum_{j=0..max_lag} (1-theta)^j rho(j)".
- Ruling E-27 (progress.md): "the persistence gains g_k multiply the member weights of the parent's composition".
- For an AR(1) signal with per-session decay phi_k, `g_k = theta / (theta + phi_k - theta phi_k)`, which is about
  `1 / (1 + phi_k / theta)`. That is GP's per-signal aim weight `1 / (1 + phi_k a / gamma)` with `a / gamma = 1 /
  theta`: a signal-specific forecast term structure entering the aim.
- Its verdict (progress.md): "R-3 (`ew-theme-std-aim-v1`, L 1.1264 after one correction): NOT accepted, N 43. dSR
  -.0118". A second version of it would count the same hypothesis twice.

**The nearest form that is genuinely different** is horizon-bucketed fast and slow sleeves, netted before trading. The
signal-specific decay decides how fast each bucket is traded, not how much of it is aimed at.

- **Rule.**
  - The parent's composition is split by the bucket of each theme, with the parent's per-date standardisation and theme
    masses kept.
  - Fast sleeve aim: `L x desired_fast`. Slow sleeve aim: `L x desired_slow`, each built from its own blend.
  - Each sleeve carries its own holdings: `fast += theta_f (aim_fast - fast)` and `slow += theta_s (aim_slow - slow)`.
  - The book holds `fast + slow` and trades only the net change.
  - `theta_s = .05`, the parent's aim-partial-v5 theta; the trade fraction must equal it (PM8-16 #3, enforced).
    `theta_f = 1 - 2^(-C/5)` per rebalance at the cadence C (PM8-16 #4): the fast aim gap closes at the rate the fast
    alpha decays, a 5-session half-life at any cadence (.12945 at the parent's C = 1).
- **How it differs from the earlier rules.**
  - R-3: one aim with one theta and changed member weights. Y-5 keeps the weights and uses two aims and two rates.
  - R-4: a band on the whole book's rank. Y-5 has no band.
  - R-9: a book-level theta. Y-5's slow sleeve keeps .05, and only the fast bucket's rate is set, from its registered
    half-life.
  - So the new content is per-bucket decay entering the trade path. It is not a second book-level rate.
- **Expected effect.** At theta .05 a 5-session alpha is captured at about .29, against about .53 at theta_f. The
  fast-sleeve gross alpha rises about 1.8x, and its turnover about 2.6x. Opposite sleeve trades net out before they are
  costed.
- **Gross matching (PM6-6) applies** once the rule is wired, because netting changes realised gross.
- **Acceptance (default).** dSR > 0 AND mechanics. Printed: turnover per unit gross, cost per traded dollar, net Sharpe
  at 4x, and sleeve versus net trade (`TwoSpeedTrade`).
- **Look-ahead surfaces.** The half-lives are registered from the literature below and read no data. The kernel reads
  no return.
- **Delivered.** The pure kernel `atx/engine/book/two_speed.hpp` and `BookTwoSpeed.*`. The tests cover the
  constants, the closed form, equal rates being one book toward the summed aim, netting, the 5-step half-life, and
  refusals.
- **Wiring.** Done after PM8-10. See "Y-5 wiring" below; it replaces the follow-up plan this section first carried.

**Per-theme alpha-decay half-life table (registration, blind, in sessions).** A theme is fast if its half-life is at
most 10.

| theme | half-life | bucket | basis |
|---|---|---|---|
| value | 252 | slow | Fama-French 1992; value spreads mean-revert over years |
| profitability_quality | 252 | slow | Novy-Marx 2013 (annual rebalancing keeps the premium) |
| investment_issuance | 252 | slow | Cooper-Gulen-Schill 2008 (asset growth premium over following years) |
| earnings_momentum | 63 | slow | Bernard-Thomas 1989 (drift concentrated in about 60 sessions) |
| price_momentum | 126 | slow | Jegadeesh-Titman 1993 (3-12 month holding) |
| low_risk | 252 | slow | Frazzini-Pedersen 2014 (slow betas, monthly rebalancing) |
| short_interest | 63 | slow | Boehmer-Huszar-Jordan 2010 (monthly horizon) |
| reversal_seasonality | 5 | fast | Lehmann 1990; Jegadeesh 1990 (weekly reversal). Its seasonality members (Heston-Sadka) are slow; the half-life is registered per theme, not per member. |
| options_implied | 21 | slow | Cremers-Weinbaum 2010; Xing-Zhang-Zhao 2010 (weekly to monthly) |
| ownership_flow | 63 | slow | Sias-Starks-Titman 2006; Cohen-Malloy-Pomorski 2012 (quarterly) |
| filing_events | 21 | slow | event drift of about one month |
| price_volume | 5 | fast | Kakushadze 2016 (101 alphas, average holding 0.6-6.4 days) |

## Y-5 wiring (PM8-10; core in C++ per PM8-12)

Rulings: PM8-10 accepts Y-5 as distinct from R-3 and counts it once. PM8-12 puts the rule in C++; Python only writes the
spec.

### Half-life table: mine against YSIG's proposal

YSIG's proposal is Appendix A of `task-YSIG-report.md` on `feat/platform-v8-ysig-20261002`. The two tables differ only
inside the slow bucket:

| theme | YCOMB (mine) | YSIG |
|---|---|---|
| earnings_momentum | 63 | 31 |
| low_risk | 252 | 126 |
| short_interest | 63 | 126 |
| filing_events | 21 | 126 |
| price_volume | 5 | 3 |

All other themes agree. In both tables the fast bucket (half-life at most 10) is exactly reversal_seasonality and
price_volume, and theta_f is fixed by the registered fast half-life of 5 sessions.

The rule reads only the bucket and that constant, so the two tables give the same rule bit for bit. Picked blind:
**mine**, because it is the registered one and switching changes nothing that runs. YSIG's suggested member-level
override for reversal_seasonality is not adopted: the sleeves split theme composites, and splitting a theme would
undo its standardisation. The PM pins the table.

### End to end, behind one flag pair

The cell sets the same flag on two steps: fit `--two-speed two-speed-v1` and nav `--two-speed two-speed-v1`.

1. **Fitter (spec only).** `composition_two_speed.py` adds `theme_sleeves: {"rule": "two-speed-v1"}` to the parent's
   document. Weights, signs, theme_standardise and provenance.rule do not change.
2. **IC runner.** `strategy_ic_two_speed.cpp` (`composition_sleeves`):
   - accepts exactly that block, and only beside a rerank-true theme_standardise block with no theme_residualise;
   - derives each weighted theme's half-life from the C++ table (`strategy_two_speed.hpp`) and refuses an
     unregistered theme;
   - requires at least one fast and one slow theme.
3. **IC composition.** `IcComposition::set_theme_sleeves`:
   - In `finish`, a second pass adds each theme's unchanged per-date re-rank, times the mass in force, to its
     sleeve's plane. The mass in force includes theme-tsmom-v1's schedule when the parent carries one.
   - It records the fast themes' mass share per date, over the themes with a present member at that date (0 when
     none has one; PM8-16 #9).
   - The blend itself is untouched bit for bit.
4. **w pass output.** The w pass writes:
   - `<role>_sleeve_fast.f64`, `<role>_sleeve_slow.f64` and `<role>_sleeve_fast_share.f64`;
   - `<role>_sleeves.json`, pinned by SHA-256 in the combined manifest as `composition_sleeves`. The combined file set
     stays the five files every consumer admits.
   - The recipe gains `composition_sleeves` and the summary gains `composition_weights.sleeves`.
   - Admission charges the two planes and the share row.
5. **NAV load and construction.**
   - `load_saved_blend` loads the pinned sleeves under the flag. It checks receipts, support equal to the blend's,
     and shares in [0, 1], and refuses a run without sleeves.
   - On each rebalance, each sleeve gets the parent's construction: ranks, demean, gross 1, locate zeroing,
     neutralization, and norm-score-v1 when the parent carries it.
6. **Netting (engine kernel).** `atx::engine::book::two_speed_aim`:
   - advances the virtual fast sleeve, `F_next = F + theta_f (L m_f d_f - F)`, with theta_f = 1 - 2^(-C/5) at the
     cadence C;
   - returns the netted aim, `desired = (1 - m_f) d_s + (F + (F_next - F) / theta_s) / L`.
   - aim-partial-v5's unchanged step on that aim is exactly `F_next - F` plus the remainder's `(current - F)` step
     toward `L (1 - m_f) d_s` at theta_s = .05. Only the net is traded.
   - The parent's dust band, exits, locate block, costs and capacity curve are unchanged.
   - F is book-independent (no drift, no fills), so it lives in the replay's shared construction state. A
     construction grid has one cadence and forms one lockstep group per aim leverage (review #2).
7. **Refusals.**
   - Refused with spo (any version), aim-partial-v6, a non-fixed rate (also by the NAV config itself, review #1), a
     trade fraction other than .05, `--hold-band`, `--vol-scale`, `--adv-hold-q` and a grid mixing cadences.
   - Composes with `--risk-target` and `--vol-target` since 3ff73201 (section "Y-5 / Y-1 composition").
   - Refused by `nav decide`, because the holdings file does not carry F.
   - Refused without a construction state (`form_desired` called bare). The target replay holds a state, so the
     construction also runs there when the sleeves are attached; it has no CLI flag.

### Template, gross matching and acceptance

- **Template.** `scripts/specs/v8/y-two-speed.json`: nominal parent `x-theme-erc.json`, parent = the last accepted book
  at Y-5's place in the order (Y-S -> Y-3 -> Y-2 -> Y-5).
- **Gross matching (PM6-6) applies.** The rule changes how two buckets trade, not the book's scale. Netting and the
  virtual fast sleeve still change the realised gross. So the cell is judged at the parent's dollar gross:
  1. a calibration run at L_parent, which reads G_cal only;
  2. the trial at `L_parent x G_parent / G_cal`.
- **Acceptance (PM7-34 default).** Paired S2 net dSR > 0 against the parent AND mechanics. Printed only: turnover per
  unit gross, cost per traded dollar, net Sharpe at 4x, net annual return, the fast mass share (read from the w
  pass's `<role>_sleeve_fast_share.f64`, pinned by `<role>_sleeves.json`; nothing writes `provenance.two_speed`), and
  the NAV summary's `construction.two_speed` block.

### Look-ahead surfaces

- The half-life table is registered from the literature and reads no data.
- The sleeves are the parent blend's own per-date theme parts. They carry no new information and no return.
- `m_f(d)` comes from the masses in force at d and the themes present at d. Under theme-tsmom-v1 those are the
  walk-forward schedule's (Y-2's surfaces).
- F uses only desired targets formed at or before the decision. The kernel reads no return. Under a scaler the carry
  reads L_t at d (the scaler's own surfaces) and the book's lambda at its previous rebalance.

### How root verifies flag absent

- The parent's fit argv must give the same weights and admission, except script_sha256.
- The parent's w argv on the new build must be byte-identical. With no theme_sleeves block nothing runs:
  `TwoSpeedRunner.SavesTheSleevesBesideAnUnchangedBlend` checks blend, targets and `__combined__` rows byte for byte
  even with the block present.
- The parent's NAV argv must be byte-identical. Without `--two-speed`, form_desired never reaches the branch:
  `TwoSpeed.ZeroFastShareIsTheParentRunBitForBit` and, under price-risk-v1 and locate-in-aim,
  `...UnderNeutralizationAndLocateInAimIsTheParentRunBitForBit` show even the on-path with m_f = 0 is the parent's
  run bit for bit.
- Not byte-identical, and not an output: with every flag absent `ConstructionDay` grows 16 B (Y-3's two fields; the
  two-speed flags sit in its padding, review #8), so the per-row budget charges (target replay `:198`, `:855`; NAV
  books x sessions) rise by 16 B a row. For the parent class (`--max-bytes 1073741824`, about 1,100 sessions with the
  warm start, at most 16 books in a pass) that is at most 16 x 1,100 x 16 = 0.28 MB, 0.03% of the cap; the
  integration log records the earlier `NavHolding` +16 B passing the same cap. Headroom is a bound, not a measured
  run (no real data here).

### Tests added

- `BookTwoSpeed.NettedAimClosedForm`, `.AimPartialStepOnTheAimIsTheNettedSleeveMove`, `.NettedAimRefusalsWriteNothing`.
- `TwoSpeed.*`:
  - the table, every entry spelled out;
  - composition sleeves equal to the single-theme blends bit for bit, and the share under a schedule;
  - the per-decision closed form;
  - zero-share identity;
  - the NAV run differing from the parent's;
  - refusals;
  - rule id, recipe and summary keys.
- `TwoSpeedRunner.*`: the end-to-end w pass, with sleeves equal to the fast-only and slow-only runs bit for bit, the
  replay loader reading them back, and refusals before any payload is read.
- `test_composition_two_speed.py`: the spec block only, flag-absent identity, refusals.

No Python mirror of the rule exists (PM8-12): the closed forms are in the gtests.

### Build targets and gtest filters added

- `atx-impl-strategy-target-tests` (now includes `strategy_two_speed_test.cpp`): `--gtest_filter=TwoSpeed.*:BookTwoSpeed.*`.
- `atx-impl-strategy-ic-tests`: `--gtest_filter=TwoSpeedRunner.*`, plus the regression filters
  `ThemeTsmomRunner.*:CompositionV8.*`.
- New source `atx-impl/src/strategy_ic_two_speed.cpp` in atx-impl-core and the Debug /O2 lists.
- The engine book group picks up the extended `book_two_speed_test.cpp`.

## Y-5 / Y-1 composition (registered order Y-5 -> X-10 -> Y-1; half-life table pinned by PM8-13)

Defined blind, one way, in C++.

**What composes.** `nav --two-speed two-speed-v1` now composes with `--vol-target vol-target-v1` and `--risk-target`.
X-10 inherits the parent's construction. The refusal stays only for spo, aim-partial-v6 and a non-fixed rate.

**How.**

- The netted aim from `engine::book::two_speed_aim` is the book target,
  `T = L m_s d_s + F + (F_next - F) / theta_s`, formed at the run's L.
- The scaler's L_t = lambda L replaces the run's L in the plan of that target.
- The book's fast holding follows its scale, lambda F (PM8-16 #10, registered by the review fixes). Each rebalance
  carries F from the book's lambda at its previous rebalance (`engine::book::two_speed_carry`), so the plan steps the
  remainder `R = current - lambda_prev F` at theta_s toward `lambda L m_s d_s` and the fast part to `lambda F_next`.
  Equal scales add nothing.
- The scaler's sigma reads the book's own planned (net) weights. The virtual fast sleeve F never enters it and keeps
  evolving at the run's L.
- Gross matching (PM6-6) is computed on the net book, which is the NAV's all-rows gross.

**Mechanics printed in the NAV summary.** `construction.two_speed` carries, printed only, the cadence C, theta_fast,
theta_slow and three counts:

- `rebalances_skipped_by_a_sleeve`: rebalances skipped because a sleeve's neutralization was skipped.
- `parent_rebalances_skipped`: per decision, whether the parent's construction of the full blend would have been
  skipped. This is computed in the same run; without a neutralization it is always 0. It costs a third construction
  and neutralization per decision (about 3x the parent's construction time).
- `parent_constructions_failed`: that diagnostic construction returned an error (recorded, never raised).

The registered skip behaviour is unchanged.

**Tests added.** All in `atx-impl-strategy-target-tests`, filter `TwoSpeed.*`:

- `TwoSpeed.UnderVolTargetZeroShareIsTheParentRunBitForBit`: every NAV day and every recorded L_t equal the parent
  under vol-target-v1.
- `TwoSpeed.UnderVolTargetTheHookPlansTheCarriedNettedAimAtLt` (was `...TheScalerScalesTheNettedTarget`): the hook's
  plan equals update_weights at the recorded L_t on the carried netted aim, bit for bit.
- `TwoSpeed.UnderARiskTargetTheBookIsTheScaledSleeveDecomposition`: the closed form through `replay_nav` with
  L_t != L (review #13).
- `TwoSpeed.ParseComposesWithTheScalersAndRefusesTheRest`.
- `TwoSpeed.SummaryPrintsTheSleeveSkipsBesideTheParents`.

## Core vs wrapper (PM8-12; for lane YARCH)

| Python path | What it computes | Duplicates C++? | Migration |
|---|---|---|---|
| `composition_theme_tsmom.py` (Y-2) | sleeve returns from the fitter's factor series, trailing sums, block starts, and the mass kernel (`masses`, also used to write the shared fixture) | the mass kernel duplicates `theme_tsmom_masses` (pinned by the fixture); the sleeve and trailing-sum computation has no C++ copy | move sleeves and trailing sums into an engine module that reads the factor series; the fitter would only write `theme_schedule {rule}`; retire `masses()` and the fixture writer |
| `composition_two_speed.py` (Y-5) | writes `{"rule": "two-speed-v1"}` | no (thin, per PM8-12) | none |
| `composition_theme_erc.py` (lane XCOMB; not mine) | ERC shares and member cap, mirrored by `strategy_ic_theme_erc.cpp` | yes | listed for YARCH only |
| Y-1 and Y-3 | no Python rule: C++ (`book/vol_target.hpp`, `book/normal_score.hpp`) driven by NAV flags | no | none |

## Y-4: left unused, on purpose

Each trial raises N and so raises the deflated-Sharpe hurdle for every later cell. No remaining candidate has a
positive prior that is not excluded or already covered:

- Per-theme vol management (Moreira-Muir per factor): Cederburg et al. 2020 find real-time factor vol timing loses
  out of sample after costs. X-5's ERC already equalises theme risk, and Y-1 tests the book-level form.
- A borrow-aware short-leg tilt would re-fit the cost model, which already charges borrow.
- Dispersion-conditioned leverage: there is no literature constant to register blind.

## Tests run (synthetic data only)

- `python -m pytest -q -p no:cacheprovider test_composition_theme_tsmom.py test_composition_theme_erc.py`: 26 passed.
- `test_fit_composition_weights.py test_composition_resid.py test_composition_ic_shrink.py test_composition_rules.py`:
  149 passed, 17 subtests passed.
- `scripts/tests/test_research_spec.py`: 74 passed. This registers y-vol-target, y-norm-score and y-theme-tsmom. After the Y-5 wiring it is 76 passed, with
  y-two-speed registered. The composition and fitter suites, two-speed included, then give 181 passed (+17 subtests)
  and 110 passed after the PM8-12 thinning.

## What root must build and verify

```powershell
powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests   # VolTarget.* BookVolTarget.* NormScore.* BookNormalScore.* BookTwoSpeed.* RiskTarget.* InvVol.* HoldBand.* StrategyNavReplay.* StrategyTargetReplay.*
powershell scripts\atx-build.ps1 build atx-impl-strategy-ic-tests       # ThemeTsmom.* ThemeTsmomRunner.* CompositionV8.* ThemeResidRunner.* ThemeErcV1.* StrategyIcRunner.*
powershell scripts\atx-build.ps1 build atx-impl-tests                   # globbed: the new strategy_ic_theme_tsmom_test.cpp
```

Also build the engine book test group (the unity batch picks up `book_two_speed_test.cpp`; named namespace) and the
binaries `atx-equity-strategy` and `atx-equity-strategy-ic`.

Identity runs:

- The parent's NAV argv. Its bytes must be unchanged for Y-1 and Y-3 with their flags absent.
- The parent's fit argv. The weights must be unchanged except script_sha256.
- The parent's w argv. The combined signal must be byte-identical.

## Cross-lane edits

R-8's `strategy_risk_target.{hpp,cpp}` gains a second law; the default is unchanged. Other files touched:

- `strategy_nav_v7.cpp`
- `strategy_target_replay.{hpp,cpp}` and `strategy_nav_replay.cpp`
- The IC runner: `strategy_ic_composition.{hpp,cpp}`, `strategy_ic_admission.cpp`, `strategy_ic_runner.cpp`,
  `strategy_ic_detail.hpp`
- `fit_composition_weights.py` (one hook and one argument)
- `scripts/tests/test_research_spec.py` (registries)
- `atx-impl/CMakeLists.txt` and `atx-impl/tests/CMakeLists.txt`
- `strategy_ic_runner_test.cpp` (appended only)

## Concerns

1. All four C++ commits are unbuilt and must compile first time under clang-cl `/W4 /WX`. The likeliest friction:
   aggregate init of `IcThemeBlock`, the new `score_role` and `method_recipe` parameters, and the `ConstructionDay`
   fields Y-3 adds (two per day).
2. Y-5 runs only with both flags (fit and nav). Under `--vol-target` or `--risk-target` the scaler scales the netted
   target and the book carries lambda F (section "Y-5 / Y-1 composition"); that composition is defined blind and is
   untested on real data.
3. Y-1 may be read under E-45 as a re-parameterisation of R-8. R-8 targets an absolute vol; Y-1 targets the book's own
   running mean, which is the Moreira-Muir form. The PM rules on that.
4. Y-3 pushes gross to the tails. Single-name weights rise about 4x the mean |w|. Borrow on hard-to-borrow tails and
   cost per traded dollar must be checked.
5. Y-2 starts in 2021 (2020 is the parent's), has 12 themes and about 36 blocks. The paired dSR SE will be large.
6. Decide and deploy carry no keys for `--vol-target`, `--rank-shape` or `composition_schedule`, so a manifest pinned to
   these runs is refused. That is intended; decide is out of scope.

# Task YCOMB report: platform v8 Y, construction and combination rules

Lane YCOMB. Worktree `C:/atx-wt/pool-14`, branch `feat/platform-v8-ycomb-20261002`, base `798d3b23`.
**Status: DONE_WITH_CONCERNS.** Four rules are registered and committed: Y-1, Y-2, Y-3, and the Y-5 kernel. Y-4 is
deliberately left unused. None of the C++ is built (lane rule). Y-5 is registered and has its engine kernel, but it is
not wired into the IC runner or NAV, so no cell can run it yet. Everything here was registered blind: no 2020-2023
return, IC, Sharpe or NAV output was opened, and nothing dated 2024-01-01 or later was opened. The only results quoted
are the PM's public verdict lines in progress.md.

| commit | rule | files |
|---|---|---|
| `47d6afd9` | Y-1 vol-target-v1 | engine `book/vol_target.hpp` + test; impl `strategy_vol_target.{hpp,cpp}`, `strategy_risk_target.{hpp,cpp}`, `strategy_nav_v7.cpp`; test `strategy_vol_target_test.cpp`; template `y-vol-target.json` |
| `02633038` | Y-3 norm-score-v1 | engine `book/normal_score.hpp` + test; impl `strategy_target_replay.{hpp,cpp}`, `strategy_nav_replay.cpp`, `strategy_nav_v7.cpp`; test `strategy_norm_score_test.cpp`; template `y-norm-score.json` |
| `43745dd7` | Y-2 theme-tsmom-v1 | impl `strategy_ic_theme_tsmom.{hpp,cpp}`, `strategy_ic_composition.{hpp,cpp}`, `strategy_ic_admission.cpp`, `strategy_ic_runner.cpp`, `strategy_ic_detail.hpp`; tests `strategy_ic_theme_tsmom_test.cpp`, `strategy_ic_runner_test.cpp` (appended); fitter `composition_theme_tsmom.py` + test, `fit_composition_weights.py` hook; fixture `tests/fixtures/theme_tsmom_v1.json`; template `y-theme-tsmom.json` |
| `0fbcb230` | Y-5 two-speed-v1 (kernel) | engine `book/two_speed.hpp` + `book_two_speed_test.cpp` |

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
  - `theta_s = .05`, the parent's aim-partial-v5 theta. `theta_f = 1 - 2^(-1/5) = .12945`: the fast aim gap closes at
    the rate the fast alpha decays.
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
- **Not delivered (follow-up, needs a PM ruling).**
  - Fitter `--sleeve fast|slow`: two weights files with the parent's weights restricted to one bucket, masses unchanged.
  - Two IC w passes, giving two combined artifacts.
  - `nav --two-speed two-speed-v1 --fast-blend <manifest>` in the v7 hook, with per-book fast and slow state as in the
    vol-target state.
  - Template `y-two-speed.json`.

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
- `scripts/tests/test_research_spec.py`: 74 passed. This registers y-vol-target, y-norm-score and y-theme-tsmom.

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
2. Y-5 cannot run until it is wired. That needs a PM ruling and a follow-up lane.
3. Y-1 may be read under E-45 as a re-parameterisation of R-8. R-8 targets an absolute vol; Y-1 targets the book's own
   running mean, which is the Moreira-Muir form. The PM rules on that.
4. Y-3 pushes gross to the tails. Single-name weights rise about 4x the mean |w|. Borrow on hard-to-borrow tails and
   cost per traded dollar must be checked.
5. Y-2 starts in 2021 (2020 is the parent's), has 12 themes and about 36 blocks. The paired dSR SE will be large.
6. Decide and deploy carry no keys for `--vol-target`, `--rank-shape` or `composition_schedule`, so a manifest pinned to
   these runs is refused. That is intended; decide is out of scope.

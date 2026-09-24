# cp16 first alpha scorecard (frozen recipe R16-7) -- ONE PAGE

Headline variant `IncludeAuditedTerminalV1`, restriction `full`. Net of cost. `*` = hole-flagged year, `~` = n_obs < 20 (wide interval -- indicative only). Sharpe = mean/sd(ddof=1) x sqrt(252/h) on the offset-0 stride-h sub-series; CI = circular block bootstrap (block 5, 2,000 draws, seed 20260920, nearest-rank 2.5/97.5).

## blend_equal -- cut 1000
| h | pooled Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | n_obs | sign stab |
|---|---|---|---|---|---|
| 1 | -0.12257 | -0.875015 | 0.978943 | 1675 | 2/7 |
| 5 | 0.131103 | -0.60136 | 0.941181 | 333 | 5/7 |
| 10 | 0.325935 | -0.456037 | 1.124747 | 166 | 5/7 |
| 21 | -0.230952 | -0.795527 | 0.655987 | 74 | 3/7 |
| 63 | 0.369562 | -0.668882 | 1.582516 | 20 | 0/0 |

- per-year Sharpe @ h=21: 2013~ 1.148376; 2014~ 0.486572; 2015~ 0.943898; 2016*~ -1.096701; 2017*~ -0.236339; 2018*~ -1.174502; 2019~ 0.521764
- rank-IC mean @ h=21: 0.031645 | implied turnover (1-rho_rank): 0.006777 | breadth (mean n_used): 998.011 | offsets [-0.25351, 0.299016]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_126 -- cut 1000
| h | pooled Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | n_obs | sign stab |
|---|---|---|---|---|---|
| 1 | -0.103651 | -0.864232 | 1.000318 | 1675 | 2/7 |
| 5 | 0.165698 | -0.559137 | 0.983672 | 333 | 5/7 |
| 10 | 0.344262 | -0.387935 | 1.068148 | 166 | 4/7 |
| 21 | -0.18667 | -0.699325 | 0.669565 | 74 | 3/7 |
| 63 | 0.582257 | -0.191605 | 1.394577 | 20 | 0/0 |

- per-year Sharpe @ h=21: 2013~ 1.330683; 2014~ 0.294587; 2015~ 0.605766; 2016*~ -0.410877; 2017*~ -0.050307; 2018~ -1.141974; 2019~ 0.3735
- rank-IC mean @ h=21: 0.028224 | implied turnover (1-rho_rank): 0.00279 | breadth (mean n_used): 998.011 | offsets [-0.199341, 0.400446]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_252 -- cut 1000
| h | pooled Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | n_obs | sign stab |
|---|---|---|---|---|---|
| 1 | -0.183517 | -0.908131 | 0.907334 | 1675 | 2/7 |
| 5 | 0.195578 | -0.547899 | 1.035681 | 333 | 4/7 |
| 10 | 0.422593 | -0.347605 | 1.182926 | 166 | 6/7 |
| 21 | -0.04277 | -0.74954 | 0.717964 | 74 | 2/7 |
| 63 | 0.232316 | -0.66036 | 1.706551 | 20 | 0/0 |

- per-year Sharpe @ h=21: 2013~ 1.172713; 2014~ 0.58364; 2015~ 1.216245; 2016*~ -1.487989; 2017*~ 0.27922; 2018*~ -1.45779; 2019~ 1.172784
- rank-IC mean @ h=21: 0.029364 | implied turnover (1-rho_rank): 0.002469 | breadth (mean n_used): 998.011 | offsets [-0.278117, 0.274602]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## blend_equal -- cut 3000
| h | pooled Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | n_obs | sign stab |
|---|---|---|---|---|---|
| 1 | -0.048893 | -0.911523 | 0.878362 | 1442 | 3/6 |
| 5 | -0.147588 | -0.856638 | 0.701667 | 286 | 3/6 |
| 10 | 0.010681 | -0.747311 | 0.932909 | 143 | 3/6 |
| 21 | -0.102215 | -0.687175 | 0.650463 | 63 | 2/6 |
| 63 | 0.211524 | -0.644529 | 1.793188 | 17 | 0/0 |

- per-year Sharpe @ h=21: 2013~ 0.421809; 2014~ 0.052247; 2015~ 1.119307; 2016*~ -1.185962; 2017 NOT FIT; 2018*~ -0.554244; 2019~ 0.046897
- rank-IC mean @ h=21: 0.023465 | implied turnover (1-rho_rank): 0.001877 | breadth (mean n_used): 2772.925 | offsets [-0.422596, -0.045135]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_126 -- cut 3000
| h | pooled Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | n_obs | sign stab |
|---|---|---|---|---|---|
| 1 | 0.134526 | -0.756279 | 1.092886 | 1442 | 3/6 |
| 5 | -0.045472 | -0.752833 | 0.798092 | 286 | 3/6 |
| 10 | 0.070236 | -0.687752 | 1.010323 | 143 | 3/6 |
| 21 | 0.077779 | -0.445676 | 0.701094 | 63 | 3/6 |
| 63 | 0.567436 | -0.199706 | 1.795432 | 17 | 0/0 |

- per-year Sharpe @ h=21: 2013~ 0.988315; 2014~ 0.126916; 2015~ 0.756906; 2016*~ -0.770913; 2017 NOT FIT; 2018~ -0.060079; 2019~ -0.088902
- rank-IC mean @ h=21: 0.025433 | implied turnover (1-rho_rank): 0.002865 | breadth (mean n_used): 2772.925 | offsets [-0.422304, 0.17629]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_252 -- cut 3000
| h | pooled Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | n_obs | sign stab |
|---|---|---|---|---|---|
| 1 | -0.191917 | -1.032709 | 0.720278 | 1442 | 3/6 |
| 5 | -0.274389 | -1.008247 | 0.590049 | 286 | 2/6 |
| 10 | -0.098145 | -0.864109 | 0.854759 | 143 | 2/6 |
| 21 | -0.262871 | -0.994993 | 0.685974 | 63 | 4/6 |
| 63 | -0.086495 | -0.996941 | 1.453149 | 17 | 0/0 |

- per-year Sharpe @ h=21: 2013~ -0.064557; 2014~ -0.151652; 2015~ 1.465746; 2016*~ -1.803395; 2017 NOT FIT; 2018*~ -0.994103; 2019~ 0.490557
- rank-IC mean @ h=21: 0.016955 | implied turnover (1-rho_rank): 0.001235 | breadth (mean n_used): 2772.925 | offsets [-0.485388, -0.154768]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## Acceptance bars (R16-8) -- passing = candidate, not tradeable
| signal | bar1 pooled ci_lo (2.5%) > 0 @ h=21 both cuts | bar2 sign stability n/n | bar3 turnover printed | verdict |
|---|---|---|---|---|
| blend_equal | FAIL | FAIL | PASS | FAIL |
| momentum_126 | FAIL | FAIL | PASS | FAIL |
| momentum_252 | FAIL | FAIL | PASS | FAIL |

Passing = candidate, not tradeable.

## cp14 2013 anchor (same recipe, cp14 context -- NOT a cp16 cell, R16-5)
| signal | h | year | Sharpe | ci_lo | ci_hi | n_obs |
|---|---|---|---|---|---|---|
| momentum_252 | 1 | 2013 | 1.633954 | -0.432809 | 3.707016 | 188 |
| momentum_252 | 5 | 2013 | 2.013834 | 0.057099 | 4.477771 | 37 |
| momentum_252 | 10 | 2013 | 2.199718 | 0.865491 | 4.786798 | 18 |
| momentum_252 | 21 | 2013 | 1.854848 | 0.94208 | 4.284247 | 8 |
| momentum_252 | 63 | 2013 | 24.81842 |  |  | 2 |
| momentum_126 | 1 | 2013 | 1.645096 | -0.524446 | 3.899355 | 188 |
| momentum_126 | 5 | 2013 | 1.994344 | -0.186898 | 5.07545 | 37 |
| momentum_126 | 10 | 2013 | 2.181992 | 0.296322 | 4.970139 | 18 |
| momentum_126 | 21 | 2013 | 2.042762 | 0.849049 | 6.937454 | 8 |
| momentum_126 | 63 | 2013 | 5.388239 |  |  | 2 |
| blend_equal | 1 | 2013 | 1.621549 | -0.450009 | 3.738297 | 188 |
| blend_equal | 5 | 2013 | 1.889496 | -0.076198 | 4.552705 | 37 |
| blend_equal | 10 | 2013 | 2.317021 | 0.852615 | 4.944133 | 18 |
| blend_equal | 21 | 2013 | 1.920265 | 0.940179 | 5.84321 | 8 |
| blend_equal | 63 | 2013 | 6.904032 |  |  | 2 |

## Caveats (all load-bearing)
- Membership is the YEAR UNION, not as-of: a name that joined mid-year is admitted for the whole year.
- 19 corrupted pre-holiday sessions (2016-01-15..2018-02-16) contaminate 2016 and 2017 (all signals) and momentum_252/blend_equal 2018; those rows carry hole_flag=1 and `*`, as does every pooled row (R16-9).
- 2013 is a PARTIAL year: evaluation starts 2013-04-04, not 2013-01-01.
- 2014-2019 IncludeAuditedTerminalV1 is expected to collapse onto DropMissingForward; _ex34 is 2013-only (R16-6).
- N_14 = 30 configurations; these year x cut cells are AR-7 restrictions in the sidecar ledger, not trials (R16-2).
- No capacity, borrow-availability or impact model: decile spreads at +/-1.0/n gross-2.0 weights only.
- `~` marks a per-year cell with n_obs < 20: wide interval -- indicative only.
- implied_turnover is 1 - rho_rank from signal_autocorr.csv (per signal, lag 1); that file has no horizon/variant/restriction columns, so one value serves every horizon of a signal.
- Sources: C:/atx/data/equity_ic_training_2013_20260920/ic.csv; C:/atx/data/equity_ic_training_2013_20260920/quantile_spread.csv; C:/atx/data/equity_ic_training_2013_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2013_t1000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2013_t1000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2013_t1000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2013_t3000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2013_t3000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2013_t3000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2014_t1000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2014_t1000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2014_t1000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2014_t3000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2014_t3000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2014_t3000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2015_t1000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2015_t1000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2015_t1000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2015_t3000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2015_t3000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2015_t3000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2016_t1000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2016_t1000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2016_t1000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2016_t3000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2016_t3000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2016_t3000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2017_t1000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2017_t1000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2017_t1000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2018_t1000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2018_t1000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2018_t1000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2018_t3000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2018_t3000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2018_t3000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2019_t1000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2019_t1000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2019_t1000_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard16_ic_2019_t3000_20260920/ic.csv; C:/atx/data/equity_scorecard16_ic_2019_t3000_20260920/quantile_spread.csv; C:/atx/data/equity_scorecard16_ic_2019_t3000_20260920/signal_autocorr.csv

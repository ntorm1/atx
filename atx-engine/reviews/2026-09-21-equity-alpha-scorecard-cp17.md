# cp17 Stage 3 batch 1 scorecard (frozen recipe R16-7; 7 families + cp14 momentum re-measured) -- ONE PAGE

Headline variant `IncludeAuditedTerminalV1`, restriction `full`. Net of cost. `*` = hole-flagged year, `~` = n_obs < 20 (wide interval -- indicative only). Sharpe = mean/sd(ddof=1) x sqrt(252/h) on the offset-0 stride-h sub-series; CI = circular block bootstrap (block 5, 2,000 draws, seed 20260920, nearest-rank 2.5/97.5).

## amihud_21 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.741283 | -0.106592 | 1.53019 | 1.123304 | 0.271165 | 1.934218 | 1675 | 4/7 | 8.3e-05 |
| 5 | 0.655777 | -0.145435 | 1.448657 | 0.961417 | 0.162297 | 1.735728 | 333 | 4/7 | 3.9e-05 |
| 10 | 0.748905 | -0.088689 | 1.523715 | 1.008451 | 0.177637 | 1.804176 | 166 | 4/7 | 6.9e-05 |
| 21 | 0.731977 | 0.231333 | 1.270155 | 0.868187 | 0.446361 | 1.483745 | 74 | 5/7 | 0.0 |
| 63 | 0.985284 | 0.396445 | 1.491033 | 1.177135 | 0.669198 | 1.704071 | 20 | 0/0 | 0.002947 |

- per-year Sharpe @ h=21: 2013~ 2.566472; 2014~ 0.178833; 2015~ -0.241547; 2016*~ 1.365721; 2017*~ -0.787756; 2018~ 1.067595; 2019~ 0.87688
- rank-IC mean @ h=21: 0.008577 | implied turnover (1-rho_rank): 0.001464 | breadth (mean n_used): 997.989 | offsets [0.520735, 0.916885]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## blend_equal -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.12257 | -0.875015 | 0.978943 | 0.049217 | -0.746119 | 1.120962 | 1675 | 2/7 | 0.0 |
| 5 | 0.131103 | -0.60136 | 0.941181 | 0.361569 | -0.382101 | 1.212087 | 333 | 5/7 | 0.0 |
| 10 | 0.325935 | -0.456037 | 1.124747 | 0.547207 | -0.215858 | 1.32764 | 166 | 5/7 | 1e-06 |
| 21 | -0.230952 | -0.795527 | 0.655987 | -0.11516 | -0.672544 | 0.820818 | 74 | 3/7 | 0.0 |
| 63 | 0.369562 | -0.668882 | 1.582516 | 0.585879 | -0.447821 | 1.918273 | 20 | 0/0 | 7.3e-05 |

- per-year Sharpe @ h=21: 2013~ 1.148376; 2014~ 0.486572; 2015~ 0.943898; 2016*~ -1.096701; 2017*~ -0.236339; 2018*~ -1.174502; 2019~ 0.521764
- rank-IC mean @ h=21: 0.031645 | implied turnover (1-rho_rank): 0.006777 | breadth (mean n_used): 998.011 | offsets [-0.25351, 0.299016]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## high52_proximity -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.690852 | -1.550508 | 0.235723 | -0.169803 | -1.078195 | 0.736562 | 1402 | 4/6 | 0.0 |
| 5 | -0.18278 | -0.981163 | 0.700708 | 0.090283 | -0.718926 | 0.965423 | 279 | 3/6 | 0.0 |
| 10 | -0.193967 | -1.042702 | 0.601824 | 0.023385 | -0.837487 | 0.837598 | 139 | 3/6 | 0.0 |
| 21 | -0.251109 | -1.015574 | 0.449946 | -0.066971 | -0.784363 | 0.711209 | 63 | 3/6 | 0.0 |
| 63 | -0.112167 | -1.004719 | 0.891126 | 0.135223 | -0.744281 | 1.185472 | 17 | 0/0 | 2e-06 |

- per-year Sharpe @ h=21: 2013~ 0.192816; 2014~ 0.565443; 2015~ 1.009598; 2016*~ -0.982271; 2017*~ -1.612263; 2019~ -0.400481
- rank-IC mean @ h=21: 0.014877 | implied turnover (1-rho_rank): 0.022956 | breadth (mean n_used): 770.59 | offsets [-0.375251, 0.140097]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## idio_vol_63 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.593436 | -1.273266 | 0.174239 | -0.264094 | -0.985686 | 0.437223 | 1620 | 5/7 | 0.0 |
| 5 | -0.63589 | -1.38673 | 0.216204 | -0.315875 | -1.06639 | 0.555155 | 322 | 4/7 | 0.0 |
| 10 | -0.448777 | -1.171875 | 0.36429 | -0.176651 | -0.913752 | 0.664112 | 160 | 4/7 | 0.0 |
| 21 | -0.731309 | -1.477388 | 0.024616 | -0.533746 | -1.26266 | 0.27727 | 71 | 4/7 | 0.0 |
| 63 | -1.116621 | -1.640185 | -0.689608 | -0.9143 | -1.443251 | -0.45947 | 19 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -1.66319; 2014~ 0.118501; 2015~ 0.219151; 2016*~ -1.378649; 2017*~ 1.023071; 2018~ -2.638184; 2019~ -0.43074
- rank-IC mean @ h=21: 0.027921 | implied turnover (1-rho_rank): 0.003164 | breadth (mean n_used): 888.649 | offsets [-0.731309, 0.029628]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## low_vol_63 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.75333 | -1.473945 | 0.061121 | -0.479059 | -1.23368 | 0.274025 | 1620 | 5/7 | 0.0 |
| 5 | -0.767821 | -1.500876 | 0.03913 | -0.517349 | -1.24696 | 0.248132 | 322 | 4/7 | 0.0 |
| 10 | -0.741228 | -1.534092 | 0.090078 | -0.513921 | -1.273724 | 0.325824 | 160 | 4/7 | 0.0 |
| 21 | -0.895298 | -1.65805 | -0.152878 | -0.72817 | -1.496672 | 0.07209 | 71 | 4/7 | 0.0 |
| 63 | -1.056719 | -1.613865 | -0.565304 | -0.909569 | -1.454668 | -0.390289 | 19 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -2.509044; 2014~ 0.047695; 2015~ 0.325729; 2016*~ -1.365143; 2017*~ 0.593003; 2018~ -2.374935; 2019~ -0.806533
- rank-IC mean @ h=21: -0.006189 | implied turnover (1-rho_rank): 0.002581 | breadth (mean n_used): 888.649 | offsets [-0.895298, -0.397427]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_126 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.103651 | -0.864232 | 1.000318 | 0.077323 | -0.726642 | 1.19342 | 1675 | 2/7 | 0.0 |
| 5 | 0.165698 | -0.559137 | 0.983672 | 0.409758 | -0.356458 | 1.226986 | 333 | 5/7 | 0.0 |
| 10 | 0.344262 | -0.387935 | 1.068148 | 0.584566 | -0.116346 | 1.316145 | 166 | 4/7 | 1e-06 |
| 21 | -0.18667 | -0.699325 | 0.669565 | -0.064046 | -0.603116 | 0.847331 | 74 | 3/7 | 0.0 |
| 63 | 0.582257 | -0.191605 | 1.394577 | 0.827036 | 0.075247 | 1.686358 | 20 | 0/0 | 0.000186 |

- per-year Sharpe @ h=21: 2013~ 1.330683; 2014~ 0.294587; 2015~ 0.605766; 2016*~ -0.410877; 2017*~ -0.050307; 2018~ -1.141974; 2019~ 0.3735
- rank-IC mean @ h=21: 0.028224 | implied turnover (1-rho_rank): 0.00279 | breadth (mean n_used): 998.011 | offsets [-0.199341, 0.400446]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_252 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.183517 | -0.908131 | 0.907334 | -0.020685 | -0.812947 | 1.011733 | 1675 | 2/7 | 0.0 |
| 5 | 0.195578 | -0.547899 | 1.035681 | 0.418944 | -0.354419 | 1.299438 | 333 | 4/7 | 0.0 |
| 10 | 0.422593 | -0.347605 | 1.182926 | 0.637127 | -0.103258 | 1.42083 | 166 | 6/7 | 3e-06 |
| 21 | -0.04277 | -0.74954 | 0.717964 | 0.109505 | -0.582345 | 0.883759 | 74 | 2/7 | 0.0 |
| 63 | 0.232316 | -0.66036 | 1.706551 | 0.418191 | -0.471864 | 2.11842 | 20 | 0/0 | 3.8e-05 |

- per-year Sharpe @ h=21: 2013~ 1.172713; 2014~ 0.58364; 2015~ 1.216245; 2016*~ -1.487989; 2017*~ 0.27922; 2018*~ -1.45779; 2019~ 1.172784
- rank-IC mean @ h=21: 0.029364 | implied turnover (1-rho_rank): 0.002469 | breadth (mean n_used): 998.011 | offsets [-0.278117, 0.274602]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## reversal_21 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.630271 | -1.278806 | 0.115018 | -0.242881 | -0.935666 | 0.603625 | 1675 | 5/7 | 0.0 |
| 5 | -0.59998 | -1.288194 | 0.137724 | -0.216898 | -0.943149 | 0.520857 | 333 | 5/7 | 0.0 |
| 10 | -0.751239 | -1.463511 | -0.0821 | -0.431507 | -1.088288 | 0.284809 | 166 | 7/7 | 0.0 |
| 21 | -0.420669 | -1.233304 | 0.273087 | -0.176399 | -0.925801 | 0.494123 | 74 | 5/7 | 0.0 |
| 63 | -0.603238 | -1.676739 | -0.032644 | -0.348761 | -1.342658 | 0.204183 | 20 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ 0.181949; 2014~ -1.668232; 2015~ -0.896937; 2016*~ 0.84642; 2017*~ -0.971376; 2018~ -0.408189; 2019~ -1.196911
- rank-IC mean @ h=21: 0.011324 | implied turnover (1-rho_rank): 0.066417 | breadth (mean n_used): 998.011 | offsets [-0.465359, 0.18754]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## reversal_5 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -1.125116 | -1.651851 | -0.505867 | -0.492907 | -1.094611 | 0.305482 | 1675 | 7/7 | 0.0 |
| 5 | -1.033569 | -1.725328 | -0.349718 | -0.475356 | -1.145372 | 0.186374 | 333 | 7/7 | 0.0 |
| 10 | -1.357455 | -2.018945 | -0.713627 | -0.972135 | -1.638654 | -0.287807 | 166 | 7/7 | 0.0 |
| 21 | -0.296502 | -1.499688 | 0.405583 | -0.130632 | -1.204608 | 0.494258 | 74 | 5/7 | 0.0 |
| 63 | -0.630776 | -1.211477 | -0.152429 | -0.422421 | -0.968793 | 0.051085 | 20 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ 0.019822; 2014~ -1.71397; 2015~ -1.134052; 2016*~ -0.097507; 2017*~ -0.538173; 2018~ 0.458272; 2019~ -1.806817
- rank-IC mean @ h=21: 0.004588 | implied turnover (1-rho_rank): 0.248163 | breadth (mean n_used): 998.011 | offsets [-0.725805, 0.149139]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## volume_shock_63 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -3.777633 | -4.679247 | -2.960461 | 0.151137 | -0.626867 | 0.955818 | 1621 | 7/7 | 0.0 |
| 5 | -1.559037 | -2.335985 | -0.766843 | -0.286164 | -1.018737 | 0.52422 | 322 | 7/7 | 0.0 |
| 10 | -0.948313 | -1.809078 | -0.052578 | -0.120845 | -0.93993 | 0.791053 | 160 | 5/7 | 0.0 |
| 21 | -1.084102 | -1.807049 | -0.382197 | -0.509658 | -1.217211 | 0.331108 | 71 | 6/7 | 0.0 |
| 63 | -1.821532 | -3.204264 | -0.964482 | -1.295686 | -2.664356 | -0.329314 | 19 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -1.033085; 2014~ -1.750442; 2015~ 0.467317; 2016*~ -1.061704; 2017*~ -0.955274; 2018~ -1.24375; 2019~ -1.499112
- rank-IC mean @ h=21: -0.006095 | implied turnover (1-rho_rank): 0.497245 | breadth (mean n_used): 889.823 | offsets [-1.084102, 0.231865]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## amihud_21 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.729965 | -0.215596 | 1.588928 | 1.106588 | 0.210413 | 1.991769 | 1442 | 4/6 | 0.007277 |
| 5 | 0.669961 | -0.207736 | 1.504336 | 0.945442 | 0.120424 | 1.726806 | 286 | 4/6 | 0.003563 |
| 10 | 0.614743 | -0.169047 | 1.306572 | 0.83361 | 0.07032 | 1.639721 | 143 | 4/6 | 0.000775 |
| 21 | 0.998631 | 0.028147 | 1.889823 | 1.216966 | 0.241145 | 2.175662 | 63 | 4/6 | 0.03399 |
| 63 | 1.037567 | 0.078648 | 2.055919 | 1.237455 | 0.339358 | 2.208039 | 17 | 0/0 | 0.088012 |

- per-year Sharpe @ h=21: 2013~ 2.6855; 2014~ -0.240303; 2015~ -0.23971; 2016*~ 1.673894; 2017 NOT FIT; 2018~ 1.690773; 2019~ 0.700181
- rank-IC mean @ h=21: -0.004891 | implied turnover (1-rho_rank): 0.000692 | breadth (mean n_used): 2771.798 | offsets [0.517634, 0.998631]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## blend_equal -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.048893 | -0.911523 | 0.878362 | 0.215386 | -0.68503 | 1.160235 | 1442 | 3/6 | 1e-05 |
| 5 | -0.147588 | -0.856638 | 0.701667 | 0.059685 | -0.6318 | 0.906617 | 286 | 3/6 | 2e-06 |
| 10 | 0.010681 | -0.747311 | 0.932909 | 0.214776 | -0.536503 | 1.150293 | 143 | 3/6 | 2.3e-05 |
| 21 | -0.102215 | -0.687175 | 0.650463 | 0.060546 | -0.590711 | 0.851529 | 63 | 2/6 | 1.3e-05 |
| 63 | 0.211524 | -0.644529 | 1.793188 | 0.440789 | -0.448245 | 2.138044 | 17 | 0/0 | 0.002419 |

- per-year Sharpe @ h=21: 2013~ 0.421809; 2014~ 0.052247; 2015~ 1.119307; 2016*~ -1.185962; 2017 NOT FIT; 2018*~ -0.554244; 2019~ 0.046897
- rank-IC mean @ h=21: 0.023465 | implied turnover (1-rho_rank): 0.001877 | breadth (mean n_used): 2772.925 | offsets [-0.422596, -0.045135]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## high52_proximity -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.659698 | -1.633869 | 0.341078 | -0.194031 | -1.162499 | 0.776466 | 1211 | 4/6 | 0.0 |
| 5 | -0.155099 | -1.083254 | 0.670924 | 0.095689 | -0.769413 | 0.921844 | 240 | 3/5 | 1.9e-05 |
| 10 | 0.036073 | -0.865236 | 0.897686 | 0.26502 | -0.654848 | 1.098581 | 119 | 2/5 | 0.000119 |
| 21 | -0.168065 | -1.010956 | 0.769401 | 0.015367 | -0.833467 | 0.901377 | 52 | 3/5 | 3.9e-05 |
| 63 | -0.086807 | -0.858769 | 1.259962 | 0.097543 | -0.666492 | 1.408088 | 14 | 0/0 | 0.000499 |

- per-year Sharpe @ h=21: 2013~ -0.904436; 2014~ 0.537709; 2015~ 1.019235; 2016*~ -0.968903; 2017 NOT FIT; 2019~ -0.203349
- rank-IC mean @ h=21: 0.016427 | implied turnover (1-rho_rank): 0.017654 | breadth (mean n_used): 2476.879 | offsets [-0.529421, -0.158225]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## idio_vol_63 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.858505 | -1.650554 | 0.014805 | -0.62369 | -1.482624 | 0.264954 | 1420 | 4/6 | 0.0 |
| 5 | -0.654845 | -1.459347 | 0.107936 | -0.426193 | -1.191936 | 0.329045 | 282 | 4/6 | 0.0 |
| 10 | -0.646324 | -1.310512 | 0.128697 | -0.451265 | -1.179886 | 0.378238 | 140 | 4/6 | 0.0 |
| 21 | -0.6102 | -1.145457 | 0.197539 | -0.525479 | -1.050952 | 0.419991 | 62 | 4/6 | 0.0 |
| 63 | -0.77002 | -1.639902 | 0.415011 | -0.616418 | -1.516469 | 0.692936 | 17 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -2.008495; 2014~ 0.509444; 2015~ 0.998369; 2016*~ -1.18067; 2017 NOT FIT; 2018~ -1.278424; 2019~ -0.018275
- rank-IC mean @ h=21: 0.032705 | implied turnover (1-rho_rank): 0.002671 | breadth (mean n_used): 2535.221 | offsets [-0.727006, -0.108438]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## low_vol_63 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.978071 | -1.801836 | -0.089652 | -0.774483 | -1.642681 | 0.147386 | 1420 | 4/6 | 0.0 |
| 5 | -0.784762 | -1.592089 | -0.001766 | -0.597615 | -1.393729 | 0.140923 | 282 | 4/6 | 0.0 |
| 10 | -0.833932 | -1.570641 | -0.039044 | -0.665774 | -1.443937 | 0.174345 | 140 | 4/6 | 0.0 |
| 21 | -0.701761 | -1.271714 | 0.006125 | -0.620209 | -1.202505 | 0.143931 | 62 | 4/6 | 0.0 |
| 63 | -0.926809 | -2.023118 | 0.21698 | -0.794526 | -1.910042 | 0.435926 | 17 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -2.986212; 2014~ 0.33006; 2015~ 0.682509; 2016*~ -1.133537; 2017 NOT FIT; 2018~ -1.191576; 2019~ -0.875113
- rank-IC mean @ h=21: 0.014539 | implied turnover (1-rho_rank): 0.00206 | breadth (mean n_used): 2535.221 | offsets [-0.86187, -0.46447]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_126 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.134526 | -0.756279 | 1.092886 | 0.424536 | -0.490159 | 1.403355 | 1442 | 3/6 | 7.2e-05 |
| 5 | -0.045472 | -0.752833 | 0.798092 | 0.18173 | -0.516104 | 1.066215 | 286 | 3/6 | 1.1e-05 |
| 10 | 0.070236 | -0.687752 | 1.010323 | 0.281636 | -0.442227 | 1.207113 | 143 | 3/6 | 4.9e-05 |
| 21 | 0.077779 | -0.445676 | 0.701094 | 0.241213 | -0.287476 | 0.89845 | 63 | 3/6 | 8.6e-05 |
| 63 | 0.567436 | -0.199706 | 1.795432 | 0.842399 | 0.027649 | 2.097287 | 17 | 0/0 | 0.020939 |

- per-year Sharpe @ h=21: 2013~ 0.988315; 2014~ 0.126916; 2015~ 0.756906; 2016*~ -0.770913; 2017 NOT FIT; 2018~ -0.060079; 2019~ -0.088902
- rank-IC mean @ h=21: 0.025433 | implied turnover (1-rho_rank): 0.002865 | breadth (mean n_used): 2772.925 | offsets [-0.422304, 0.17629]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_252 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.191917 | -1.032709 | 0.720278 | 0.056999 | -0.815048 | 0.962302 | 1442 | 3/6 | 2e-06 |
| 5 | -0.274389 | -1.008247 | 0.590049 | -0.067136 | -0.771117 | 0.783494 | 286 | 2/6 | 0.0 |
| 10 | -0.098145 | -0.864109 | 0.854759 | 0.105717 | -0.648651 | 1.050397 | 143 | 2/6 | 5e-06 |
| 21 | -0.262871 | -0.994993 | 0.685974 | -0.093332 | -0.871249 | 0.835001 | 63 | 4/6 | 1e-06 |
| 63 | -0.086495 | -0.996941 | 1.453149 | 0.143064 | -0.797748 | 1.785446 | 17 | 0/0 | 0.0001 |

- per-year Sharpe @ h=21: 2013~ -0.064557; 2014~ -0.151652; 2015~ 1.465746; 2016*~ -1.803395; 2017 NOT FIT; 2018*~ -0.994103; 2019~ 0.490557
- rank-IC mean @ h=21: 0.016955 | implied turnover (1-rho_rank): 0.001235 | breadth (mean n_used): 2772.925 | offsets [-0.485388, -0.154768]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## reversal_21 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -1.078987 | -1.923694 | -0.248398 | -0.476927 | -1.294713 | 0.399323 | 1442 | 5/6 | 0.0 |
| 5 | -0.859313 | -1.60197 | -0.126149 | -0.481024 | -1.176274 | 0.325552 | 286 | 5/6 | 0.0 |
| 10 | -0.990956 | -1.814084 | -0.199773 | -0.599913 | -1.386321 | 0.182577 | 143 | 5/6 | 0.0 |
| 21 | -0.562321 | -1.190998 | 0.150911 | -0.290053 | -0.937472 | 0.478293 | 63 | 5/6 | 0.0 |
| 63 | -0.379138 | -1.256417 | 0.174825 | -0.148172 | -0.924406 | 0.408384 | 17 | 0/0 | 5.5e-05 |

- per-year Sharpe @ h=21: 2013~ -0.184351; 2014~ -1.470465; 2015~ -0.310006; 2016*~ 0.536925; 2017 NOT FIT; 2018~ -1.288007; 2019~ -0.532695
- rank-IC mean @ h=21: 0.007045 | implied turnover (1-rho_rank): 0.065466 | breadth (mean n_used): 2772.925 | offsets [-0.72318, 0.011392]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## reversal_5 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -1.71677 | -2.471683 | -0.964478 | -0.709728 | -1.4648 | 0.119913 | 1442 | 6/6 | 0.0 |
| 5 | -1.378204 | -2.047082 | -0.722002 | -0.843671 | -1.503886 | -0.108873 | 286 | 6/6 | 0.0 |
| 10 | -1.718827 | -2.406105 | -1.18717 | -1.320408 | -1.924505 | -0.753416 | 143 | 6/6 | 0.0 |
| 21 | -1.05594 | -1.605827 | -0.45686 | -0.794745 | -1.361666 | -0.111533 | 63 | 6/6 | 0.0 |
| 63 | -1.035552 | -1.952996 | -0.37388 | -0.766525 | -1.460953 | -0.117873 | 17 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -0.347111; 2014~ -0.653531; 2015~ -0.344655; 2016*~ -0.386982; 2017 NOT FIT; 2018~ -1.745851; 2019~ -2.628734
- rank-IC mean @ h=21: 0.006012 | implied turnover (1-rho_rank): 0.250507 | breadth (mean n_used): 2772.925 | offsets [-1.05594, 0.430891]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## volume_shock_63 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -1.440062 | -2.386167 | -0.580862 | 1.262951 | 0.313178 | 2.364454 | 1420 | 6/6 | 0.0 |
| 5 | -0.393246 | -1.453616 | 0.339757 | 0.571099 | -0.134648 | 1.426874 | 282 | 4/6 | 1e-06 |
| 10 | -0.470815 | -1.241002 | 0.252115 | 0.145182 | -0.496561 | 1.096575 | 140 | 4/6 | 0.0 |
| 21 | 0.293104 | -1.468934 | 0.708992 | 0.4088 | -0.616143 | 0.785523 | 62 | 2/6 | 1e-06 |
| 63 | -0.13259 | -0.978101 | 0.675379 | 0.269225 | -0.563289 | 1.169832 | 17 | 0/0 | 9.5e-05 |

- per-year Sharpe @ h=21: 2013~ -0.347962; 2014~ -1.561924; 2015~ 0.086685; 2016*~ -0.562805; 2017 NOT FIT; 2018~ 0.999328; 2019~ -1.269041
- rank-IC mean @ h=21: 0.014145 | implied turnover (1-rho_rank): 0.509766 | breadth (mean n_used): 2539.569 | offsets [-0.746059, 0.709078]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## Acceptance bars (R16-8) -- passing = candidate, not tradeable
| signal | bar1 pooled ci_lo (2.5%) > 0 @ h=21 both cuts | bar2 sign stability n/n | bar3 turnover printed | verdict | DSR @ h=21 per cut (N=170, beside the bars, not a bar) |
|---|---|---|---|---|---|
| amihud_21 | PASS | FAIL | PASS | FAIL | t1000 0.0; t3000 0.03399 |
| blend_equal | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 1.3e-05 |
| high52_proximity | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 3.9e-05 |
| idio_vol_63 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |
| low_vol_63 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |
| momentum_126 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 8.6e-05 |
| momentum_252 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 1e-06 |
| reversal_21 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |
| reversal_5 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |
| volume_shock_63 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 1e-06 |

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
- N_14 = 30 configurations (cp14 canonical ledger); cp17 adds N_17 = 7 families x 5 x 2 x 2 = 140 in the cp17 sidecar ledger; year x cut cells are AR-7 restrictions, not trials (R16-2). Deflated Sharpe uses --declared-n.
- No capacity, borrow-availability or impact model: decile spreads at +/-1.0/n gross-2.0 weights only.
- `~` marks a per-year cell with n_obs < 20: wide interval -- indicative only.
- implied_turnover is 1 - rho_rank from signal_autocorr.csv (per signal, lag 1); that file has no horizon/variant/restriction columns, so one value serves every horizon of a signal.
- Deflated Sharpe N = 170 = N_14 30 (canonical trial-ledger.jsonl) + N_17 140 (7 families x 5 horizons x 2 variants x 2 restrictions, sidecar trial-ledger-cp17-families.jsonl); the 13 year x cut cells are AR-7 restrictions (R16-2) and momentum re-measurements are not re-declared.
- Sources: C:/atx/data/equity_ic_training_2013_20260920/ic.csv; C:/atx/data/equity_ic_training_2013_20260920/quantile_spread.csv; C:/atx/data/equity_ic_training_2013_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2013_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2013_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2013_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2013_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2013_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2013_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2014_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2014_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2014_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2014_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2014_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2014_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2015_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2015_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2015_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2015_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2015_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2015_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2016_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2016_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2016_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2016_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2016_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2016_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2017_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2017_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2017_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2018_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2018_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2018_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2018_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2018_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2018_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2019_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2019_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2019_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard17_ic_2019_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard17_ic_2019_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard17_ic_2019_t3000_20260921/signal_autocorr.csv

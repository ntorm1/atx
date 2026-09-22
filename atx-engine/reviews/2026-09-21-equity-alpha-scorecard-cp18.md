# cp18 Stage 3 batch 2 scorecard (frozen recipe R16-7, bars R16-8) -- ONE PAGE

Headline variant `IncludeAuditedTerminalV1`, restriction `full`. Net of cost. `*` = hole-flagged year, `~` = n_obs < 20 (wide interval -- indicative only). Sharpe = mean/sd(ddof=1) x sqrt(252/h) on the offset-0 stride-h sub-series; CI = circular block bootstrap (block 5, 2,000 draws, seed 20260920, nearest-rank 2.5/97.5).

## amihud_21 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.741283 | -0.106592 | 1.53019 | 1.123304 | 0.271165 | 1.934218 | 1675 | 4/7 | 9e-06 |
| 5 | 0.655777 | -0.145435 | 1.448657 | 0.961417 | 0.162297 | 1.735728 | 333 | 4/7 | 4e-06 |
| 10 | 0.748905 | -0.088689 | 1.523715 | 1.008451 | 0.177637 | 1.804176 | 166 | 4/7 | 7e-06 |
| 21 | 0.731977 | 0.231333 | 1.270155 | 0.868187 | 0.446361 | 1.483745 | 74 | 5/7 | 0.0 |
| 63 | 0.985284 | 0.396445 | 1.491033 | 1.177135 | 0.669198 | 1.704071 | 20 | 0/0 | 0.000642 |

- per-year Sharpe @ h=21: 2013~ 2.566472; 2014~ 0.178833; 2015~ -0.241547; 2016*~ 1.365721; 2017*~ -0.787756; 2018~ 1.067595; 2019~ 0.87688
- rank-IC mean @ h=21: 0.008577 | implied turnover (1-rho_rank): 0.001464 | breadth (mean n_used): 997.989 | offsets [0.520735, 0.916885]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## blend_equal -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.12257 | -0.875015 | 0.978943 | 0.049217 | -0.746119 | 1.120962 | 1675 | 2/7 | 0.0 |
| 5 | 0.131103 | -0.60136 | 0.941181 | 0.361569 | -0.382101 | 1.212087 | 333 | 5/7 | 0.0 |
| 10 | 0.325935 | -0.456037 | 1.124747 | 0.547207 | -0.215858 | 1.32764 | 166 | 5/7 | 0.0 |
| 21 | -0.230952 | -0.795527 | 0.655987 | -0.11516 | -0.672544 | 0.820818 | 74 | 3/7 | 0.0 |
| 63 | 0.369562 | -0.668882 | 1.582516 | 0.585879 | -0.447821 | 1.918273 | 20 | 0/0 | 1.2e-05 |

- per-year Sharpe @ h=21: 2013~ 1.148376; 2014~ 0.486572; 2015~ 0.943898; 2016*~ -1.096701; 2017*~ -0.236339; 2018*~ -1.174502; 2019~ 0.521764
- rank-IC mean @ h=21: 0.031645 | implied turnover (1-rho_rank): 0.006777 | breadth (mean n_used): 998.011 | offsets [-0.25351, 0.299016]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## continuation_5 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.20019 | -1.34879 | 0.599803 | 0.487807 | -0.360257 | 1.173265 | 1675 | 6/7 | 0.0 |
| 5 | -0.147641 | -0.868407 | 0.575571 | 0.427338 | -0.236087 | 1.108568 | 333 | 5/7 | 0.0 |
| 10 | 0.573345 | -0.146129 | 1.262013 | 0.950635 | 0.258628 | 1.632002 | 166 | 5/7 | 1e-06 |
| 21 | -0.043175 | -0.634577 | 0.998891 | 0.10736 | -0.504493 | 1.230345 | 74 | 4/7 | 0.0 |
| 63 | 0.147571 | -0.382351 | 0.669245 | 0.363123 | -0.139427 | 0.917834 | 20 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -0.557797; 2014~ 0.733697; 2015~ 0.621782; 2016*~ -0.53033; 2017*~ -0.243733; 2018~ -0.554607; 2019~ 1.412852
- rank-IC mean @ h=21: -0.004588 | implied turnover (1-rho_rank): 0.248163 | breadth (mean n_used): 998.011 | offsets [-0.714925, 0.095331]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## high_vol_63 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.170779 | -0.668658 | 0.918847 | 0.448667 | -0.312161 | 1.209102 | 1620 | 4/7 | 0.0 |
| 5 | 0.268041 | -0.556463 | 1.00502 | 0.523361 | -0.248551 | 1.267407 | 322 | 4/7 | 0.0 |
| 10 | 0.321722 | -0.553984 | 1.127196 | 0.555812 | -0.313711 | 1.356146 | 160 | 4/7 | 0.0 |
| 21 | 0.530429 | -0.286431 | 1.290958 | 0.703953 | -0.102001 | 1.483884 | 71 | 4/7 | 1e-06 |
| 63 | 0.751898 | 0.176158 | 1.337717 | 0.90909 | 0.373318 | 1.493875 | 19 | 0/0 | 0.000165 |

- per-year Sharpe @ h=21: 2013~ 2.148194; 2014~ -0.347969; 2015~ -1.011582; 2016*~ 1.058575; 2017*~ -1.269376; 2018~ 2.009623; 2019~ 0.555471
- rank-IC mean @ h=21: 0.006189 | implied turnover (1-rho_rank): 0.002581 | breadth (mean n_used): 888.649 | offsets [0.059384, 0.530429]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## low_dollar_volume_21 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1.005678 | 0.33923 | 1.678482 | 1.423082 | 0.823231 | 2.223194 | 1675 | 5/7 | 0.0 |
| 5 | 1.38698 | 0.560503 | 2.207344 | 1.920813 | 1.057898 | 2.75908 | 333 | 5/7 | 0.002936 |
| 10 | 1.321707 | 0.59405 | 2.00712 | 1.71546 | 0.982882 | 2.480372 | 166 | 5/7 | 0.000533 |
| 21 | 0.896795 | 0.57065 | 1.859499 | 1.054209 | 0.769692 | 2.285944 | 74 | 6/7 | 0.0 |
| 63 | 1.34521 | 0.535993 | 2.552727 | 1.661 | 0.895324 | 2.954603 | 20 | 0/0 | 0.010724 |

- per-year Sharpe @ h=21: 2013~ 2.525884; 2014~ 1.720891; 2015~ 0.274254; 2016*~ 2.134071; 2017*~ -1.225776; 2018~ 1.174749; 2019~ 1.540699
- rank-IC mean @ h=21: 0.013188 | implied turnover (1-rho_rank): 0.001407 | breadth (mean n_used): 998.011 | offsets [0.78084, 1.445001]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## max_ret_21 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.909168 | -1.512357 | -0.183718 | -0.583415 | -1.206252 | 0.170555 | 1675 | 7/7 | 0.0 |
| 5 | -0.935606 | -1.61016 | -0.199665 | -0.558881 | -1.236954 | 0.202167 | 333 | 7/7 | 0.0 |
| 10 | -1.101454 | -1.871862 | -0.328548 | -0.751107 | -1.472932 | 0.021147 | 166 | 7/7 | 0.0 |
| 21 | -0.925272 | -1.40576 | -0.44394 | -0.699192 | -1.189617 | -0.167144 | 74 | 7/7 | 0.0 |
| 63 | -1.034594 | -1.414173 | -0.682926 | -0.83078 | -1.208138 | -0.427912 | 20 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -2.515006; 2014~ -0.46149; 2015~ -0.8155; 2016*~ -0.895848; 2017*~ -0.326546; 2018~ -1.13619; 2019~ -1.101865
- rank-IC mean @ h=21: 0.001081 | implied turnover (1-rho_rank): 0.028453 | breadth (mean n_used): 998.011 | offsets [-0.954137, -0.469817]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_126 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.103651 | -0.864232 | 1.000318 | 0.077323 | -0.726642 | 1.19342 | 1675 | 2/7 | 0.0 |
| 5 | 0.165698 | -0.559137 | 0.983672 | 0.409758 | -0.356458 | 1.226986 | 333 | 5/7 | 0.0 |
| 10 | 0.344262 | -0.387935 | 1.068148 | 0.584566 | -0.116346 | 1.316145 | 166 | 4/7 | 0.0 |
| 21 | -0.18667 | -0.699325 | 0.669565 | -0.064046 | -0.603116 | 0.847331 | 74 | 3/7 | 0.0 |
| 63 | 0.582257 | -0.191605 | 1.394577 | 0.827036 | 0.075247 | 1.686358 | 20 | 0/0 | 3e-05 |

- per-year Sharpe @ h=21: 2013~ 1.330683; 2014~ 0.294587; 2015~ 0.605766; 2016*~ -0.410877; 2017*~ -0.050307; 2018~ -1.141974; 2019~ 0.3735
- rank-IC mean @ h=21: 0.028224 | implied turnover (1-rho_rank): 0.00279 | breadth (mean n_used): 998.011 | offsets [-0.199341, 0.400446]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_252 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.183517 | -0.908131 | 0.907334 | -0.020685 | -0.812947 | 1.011733 | 1675 | 2/7 | 0.0 |
| 5 | 0.195578 | -0.547899 | 1.035681 | 0.418944 | -0.354419 | 1.299438 | 333 | 4/7 | 0.0 |
| 10 | 0.422593 | -0.347605 | 1.182926 | 0.637127 | -0.103258 | 1.42083 | 166 | 6/7 | 0.0 |
| 21 | -0.04277 | -0.74954 | 0.717964 | 0.109505 | -0.582345 | 0.883759 | 74 | 2/7 | 0.0 |
| 63 | 0.232316 | -0.66036 | 1.706551 | 0.418191 | -0.471864 | 2.11842 | 20 | 0/0 | 6e-06 |

- per-year Sharpe @ h=21: 2013~ 1.172713; 2014~ 0.58364; 2015~ 1.216245; 2016*~ -1.487989; 2017*~ 0.27922; 2018*~ -1.45779; 2019~ 1.172784
- rank-IC mean @ h=21: 0.029364 | implied turnover (1-rho_rank): 0.002469 | breadth (mean n_used): 998.011 | offsets [-0.278117, 0.274602]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## momentum_volscaled_252 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.375456 | -0.520802 | 1.301319 | 0.695514 | -0.198887 | 1.612432 | 1402 | 5/6 | 1e-06 |
| 5 | 0.51185 | -0.254948 | 1.421038 | 0.741334 | -0.069671 | 1.663893 | 279 | 5/6 | 9e-06 |
| 10 | 0.513209 | -0.182249 | 1.315068 | 0.71323 | 0.02811 | 1.58243 | 139 | 5/6 | 1.7e-05 |
| 21 | 0.225583 | -0.459572 | 0.873467 | 0.40208 | -0.280841 | 1.038667 | 63 | 4/6 | 0.0 |
| 63 | 0.389074 | -0.345358 | 1.741496 | 0.606585 | -0.165506 | 1.99801 | 17 | 0/0 | 7.2e-05 |

- per-year Sharpe @ h=21: 2013~ 0.70406; 2014~ 0.823399; 2015~ 1.549231; 2016*~ -1.135795; 2017*~ -0.481247; 2019~ 0.026812
- rank-IC mean @ h=21: 0.021715 | implied turnover (1-rho_rank): 0.005714 | breadth (mean n_used): 770.59 | offsets [0.115286, 0.469518]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## residual_momentum_252 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.16678 | -0.677285 | 1.06673 | 0.497967 | -0.355145 | 1.380716 | 1402 | 4/6 | 0.0 |
| 5 | 0.340427 | -0.44565 | 1.262296 | 0.575979 | -0.235527 | 1.498429 | 279 | 5/6 | 1e-06 |
| 10 | 0.421674 | -0.304178 | 1.296276 | 0.62854 | -0.072522 | 1.497038 | 139 | 5/6 | 6e-06 |
| 21 | 0.378462 | -0.228815 | 1.043304 | 0.567487 | -0.072132 | 1.219513 | 63 | 5/6 | 3e-06 |
| 63 | 0.704309 | 0.040537 | 1.556272 | 0.982195 | 0.296913 | 1.919586 | 17 | 0/0 | 0.000988 |

- per-year Sharpe @ h=21: 2013~ 0.981657; 2014~ 0.4028; 2015~ 1.249596; 2016*~ -1.234476; 2017*~ 0.588422; 2019~ 0.403403
- rank-IC mean @ h=21: 0.031701 | implied turnover (1-rho_rank): 0.00983 | breadth (mean n_used): 770.59 | offsets [0.082771, 0.430151]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## volume_shock_neg_63 -- cut 1000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -4.038048 | -4.838125 | -3.282236 | -0.191443 | -0.964368 | 0.593901 | 1621 | 7/7 | 0.0 |
| 5 | -1.059812 | -2.008524 | -0.232903 | 0.240772 | -0.541023 | 0.963851 | 322 | 6/7 | 0.0 |
| 10 | -0.733973 | -1.672843 | 0.11773 | 0.083803 | -0.799434 | 0.911485 | 160 | 4/7 | 0.0 |
| 21 | -0.164553 | -1.026046 | 0.546116 | 0.425573 | -0.423871 | 1.131648 | 71 | 4/7 | 0.0 |
| 63 | 0.33684 | -0.511889 | 1.331664 | 0.782658 | 0.100747 | 1.881009 | 19 | 0/0 | 1.4e-05 |

- per-year Sharpe @ h=21: 2013~ -0.1334; 2014~ 0.732508; 2015~ -1.998917; 2016*~ 0.035711; 2017*~ -1.269251; 2018~ -1.472536; 2019~ 0.640147
- rank-IC mean @ h=21: 0.006095 | implied turnover (1-rho_rank): 0.497245 | breadth (mean n_used): 889.823 | offsets [-1.480388, 0.006245]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## amihud_21 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.729965 | -0.215596 | 1.588928 | 1.106588 | 0.210413 | 1.991769 | 1442 | 4/6 | 3.1e-05 |
| 5 | 0.669961 | -0.207736 | 1.504336 | 0.945442 | 0.120424 | 1.726806 | 286 | 4/6 | 8e-06 |
| 10 | 0.614743 | -0.169047 | 1.306572 | 0.83361 | 0.07032 | 1.639721 | 143 | 4/6 | 0.0 |
| 21 | 0.998631 | 0.028147 | 1.889823 | 1.216966 | 0.241145 | 2.175662 | 63 | 4/6 | 0.000313 |
| 63 | 1.037567 | 0.078648 | 2.055919 | 1.237455 | 0.339358 | 2.208039 | 17 | 0/0 | 0.00463 |

- per-year Sharpe @ h=21: 2013~ 2.6855; 2014~ -0.240303; 2015~ -0.23971; 2016*~ 1.673894; 2017 NOT FIT; 2018~ 1.690773; 2019~ 0.700181
- rank-IC mean @ h=21: -0.004891 | implied turnover (1-rho_rank): 0.000692 | breadth (mean n_used): 2771.798 | offsets [0.517634, 0.998631]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## blend_equal -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.048893 | -0.911523 | 0.878362 | 0.215386 | -0.68503 | 1.160235 | 1442 | 3/6 | 0.0 |
| 5 | -0.147588 | -0.856638 | 0.701667 | 0.059685 | -0.6318 | 0.906617 | 286 | 3/6 | 0.0 |
| 10 | 0.010681 | -0.747311 | 0.932909 | 0.214776 | -0.536503 | 1.150293 | 143 | 3/6 | 0.0 |
| 21 | -0.102215 | -0.687175 | 0.650463 | 0.060546 | -0.590711 | 0.851529 | 63 | 2/6 | 0.0 |
| 63 | 0.211524 | -0.644529 | 1.793188 | 0.440789 | -0.448245 | 2.138044 | 17 | 0/0 | 3.1e-05 |

- per-year Sharpe @ h=21: 2013~ 0.421809; 2014~ 0.052247; 2015~ 1.119307; 2016*~ -1.185962; 2017 NOT FIT; 2018*~ -0.554244; 2019~ 0.046897
- rank-IC mean @ h=21: 0.023465 | implied turnover (1-rho_rank): 0.001877 | breadth (mean n_used): 2772.925 | offsets [-0.422596, -0.045135]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## continuation_5 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.347131 | -1.186496 | 0.456636 | 0.675041 | -0.154473 | 1.435448 | 1442 | 5/6 | 0.0 |
| 5 | 0.252721 | -0.544758 | 0.931961 | 0.803066 | 0.053276 | 1.473377 | 286 | 2/6 | 0.0 |
| 10 | 0.908201 | 0.280783 | 1.505715 | 1.31967 | 0.736702 | 1.929153 | 143 | 6/6 | 1.6e-05 |
| 21 | 0.506741 | -0.305158 | 1.128212 | 0.781657 | 0.104 | 1.349619 | 63 | 2/6 | 1e-06 |
| 63 | 0.451563 | -0.289509 | 1.052224 | 0.716179 | 0.112378 | 1.341085 | 17 | 0/0 | 8e-06 |

- per-year Sharpe @ h=21: 2013~ -0.564186; 2014~ -0.322997; 2015~ -0.195858; 2016*~ -0.301427; 2017 NOT FIT; 2018~ 1.38471; 2019~ 1.951563
- rank-IC mean @ h=21: -0.006012 | implied turnover (1-rho_rank): 0.250507 | breadth (mean n_used): 2772.925 | offsets [-0.912135, 0.506741]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## high_vol_63 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.428778 | -0.462392 | 1.258207 | 0.656914 | -0.237105 | 1.529735 | 1420 | 4/6 | 2e-06 |
| 5 | 0.348266 | -0.408851 | 1.119631 | 0.550491 | -0.195354 | 1.347626 | 282 | 4/6 | 1e-06 |
| 10 | 0.341149 | -0.453494 | 1.1597 | 0.540894 | -0.254118 | 1.448366 | 140 | 4/6 | 2e-06 |
| 21 | 0.566883 | -0.308229 | 1.072439 | 0.648476 | -0.111764 | 1.231448 | 62 | 4/6 | 0.0 |
| 63 | 0.583526 | -0.607247 | 1.678735 | 0.729481 | -0.467893 | 1.884161 | 17 | 0/0 | 0.000132 |

- per-year Sharpe @ h=21: 2013~ 2.604863; 2014~ -0.631832; 2015~ -1.250549; 2016*~ 0.83961; 2017 NOT FIT; 2018~ 1.190967; 2019~ 0.496274
- rank-IC mean @ h=21: -0.014539 | implied turnover (1-rho_rank): 0.00206 | breadth (mean n_used): 2535.221 | offsets [0.166616, 0.594431]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## low_dollar_volume_21 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.654765 | -0.266535 | 1.571515 | 1.307183 | 0.426437 | 2.250615 | 1442 | 5/6 | 1.8e-05 |
| 5 | 0.777688 | -0.104134 | 1.447585 | 1.166562 | 0.436 | 1.844478 | 286 | 5/6 | 1e-06 |
| 10 | 0.802194 | 0.140792 | 1.447778 | 1.080717 | 0.55013 | 2.050877 | 143 | 5/6 | 0.0 |
| 21 | 1.142507 | 0.109978 | 1.950016 | 1.444384 | 0.579662 | 2.289332 | 63 | 5/6 | 0.000115 |
| 63 | 1.552209 | 0.843465 | 2.592505 | 1.88769 | 1.19773 | 3.007474 | 17 | 0/0 | 0.077761 |

- per-year Sharpe @ h=21: 2013~ 2.448622; 2014~ -0.044524; 2015~ 0.570117; 2016*~ 3.32222; 2017 NOT FIT; 2018~ 1.570518; 2019~ 0.084247
- rank-IC mean @ h=21: -0.000204 | implied turnover (1-rho_rank): 0.000767 | breadth (mean n_used): 2772.925 | offsets [0.528575, 1.213689]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## max_ret_21 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -1.25125 | -2.071958 | -0.359341 | -0.868069 | -1.732079 | 0.016055 | 1442 | 5/6 | 0.0 |
| 5 | -1.075357 | -1.880735 | -0.291911 | -0.773019 | -1.530502 | 0.025505 | 286 | 4/6 | 0.0 |
| 10 | -0.936131 | -1.671078 | -0.215936 | -0.682842 | -1.442404 | 0.071389 | 143 | 4/6 | 0.0 |
| 21 | -1.232729 | -2.119526 | -0.329408 | -1.010758 | -1.972202 | -0.051414 | 63 | 5/6 | 0.0 |
| 63 | -0.863545 | -1.731581 | 0.083723 | -0.679939 | -1.560912 | 0.334778 | 17 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -3.592985; 2014~ -0.009475; 2015~ 0.215133; 2016*~ -1.308263; 2017 NOT FIT; 2018~ -2.188573; 2019~ -1.116228
- rank-IC mean @ h=21: 0.010502 | implied turnover (1-rho_rank): 0.023906 | breadth (mean n_used): 2772.925 | offsets [-1.253808, -0.482105]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_126 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.134526 | -0.756279 | 1.092886 | 0.424536 | -0.490159 | 1.403355 | 1442 | 3/6 | 0.0 |
| 5 | -0.045472 | -0.752833 | 0.798092 | 0.18173 | -0.516104 | 1.066215 | 286 | 3/6 | 0.0 |
| 10 | 0.070236 | -0.687752 | 1.010323 | 0.281636 | -0.442227 | 1.207113 | 143 | 3/6 | 0.0 |
| 21 | 0.077779 | -0.445676 | 0.701094 | 0.241213 | -0.287476 | 0.89845 | 63 | 3/6 | 0.0 |
| 63 | 0.567436 | -0.199706 | 1.795432 | 0.842399 | 0.027649 | 2.097287 | 17 | 0/0 | 0.000807 |

- per-year Sharpe @ h=21: 2013~ 0.988315; 2014~ 0.126916; 2015~ 0.756906; 2016*~ -0.770913; 2017 NOT FIT; 2018~ -0.060079; 2019~ -0.088902
- rank-IC mean @ h=21: 0.025433 | implied turnover (1-rho_rank): 0.002865 | breadth (mean n_used): 2772.925 | offsets [-0.422304, 0.17629]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## momentum_252 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -0.191917 | -1.032709 | 0.720278 | 0.056999 | -0.815048 | 0.962302 | 1442 | 3/6 | 0.0 |
| 5 | -0.274389 | -1.008247 | 0.590049 | -0.067136 | -0.771117 | 0.783494 | 286 | 2/6 | 0.0 |
| 10 | -0.098145 | -0.864109 | 0.854759 | 0.105717 | -0.648651 | 1.050397 | 143 | 2/6 | 0.0 |
| 21 | -0.262871 | -0.994993 | 0.685974 | -0.093332 | -0.871249 | 0.835001 | 63 | 4/6 | 0.0 |
| 63 | -0.086495 | -0.996941 | 1.453149 | 0.143064 | -0.797748 | 1.785446 | 17 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -0.064557; 2014~ -0.151652; 2015~ 1.465746; 2016*~ -1.803395; 2017 NOT FIT; 2018*~ -0.994103; 2019~ 0.490557
- rank-IC mean @ h=21: 0.016955 | implied turnover (1-rho_rank): 0.001235 | breadth (mean n_used): 2772.925 | offsets [-0.485388, -0.154768]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## momentum_volscaled_252 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.038609 | -0.961659 | 1.045876 | 0.374644 | -0.621684 | 1.351168 | 1211 | 3/6 | 0.0 |
| 5 | 0.079116 | -0.829859 | 1.024949 | 0.319003 | -0.519669 | 1.283025 | 240 | 3/5 | 0.0 |
| 10 | 0.322033 | -0.580112 | 1.23505 | 0.5616 | -0.378658 | 1.554224 | 119 | 3/5 | 5e-06 |
| 21 | 0.154539 | -0.633948 | 1.082564 | 0.355163 | -0.494754 | 1.250244 | 52 | 3/5 | 3e-06 |
| 63 | 0.294283 | -0.352284 | 1.2926 | 0.50194 | 0.005391 | 1.544243 | 14 | 0/0 | 0.000124 |

- per-year Sharpe @ h=21: 2013~ -0.211037; 2014~ 0.25726; 2015~ 1.472932; 2016*~ -1.22355; 2017 NOT FIT; 2019~ 0.590394
- rank-IC mean @ h=21: 0.018447 | implied turnover (1-rho_rank): 0.004716 | breadth (mean n_used): 2476.879 | offsets [-0.249961, 0.154539]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## residual_momentum_252 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.118022 | -0.808754 | 1.08307 | 0.437009 | -0.472226 | 1.402088 | 1211 | 3/6 | 0.0 |
| 5 | 0.094623 | -0.827594 | 1.08888 | 0.336886 | -0.590521 | 1.375783 | 240 | 3/5 | 0.0 |
| 10 | 0.432485 | -0.451242 | 1.43763 | 0.685385 | -0.256226 | 1.660056 | 119 | 4/5 | 1.6e-05 |
| 21 | 0.362319 | -0.387517 | 1.221954 | 0.569197 | -0.230895 | 1.391139 | 52 | 3/5 | 2.2e-05 |
| 63 | 0.475629 | -0.054414 | 1.128095 | 0.645167 | 0.261313 | 1.606188 | 14 | 0/0 | 2.7e-05 |

- per-year Sharpe @ h=21: 2013~ 1.030914; 2014~ -0.027563; 2015~ 1.469507; 2016*~ -1.305155; 2017 NOT FIT; 2019~ 1.042032
- rank-IC mean @ h=21: 0.0253 | implied turnover (1-rho_rank): 0.005211 | breadth (mean n_used): 2476.879 | offsets [-0.04412, 0.362319]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): FIRES

## volume_shock_neg_63 -- cut 3000
| h | pooled net Sharpe | ci_lo (2.5%) | ci_hi (97.5%) | gross Sharpe | gross ci_lo | gross ci_hi | n_obs | sign stab | DSR |
|---|---|---|---|---|---|---|---|---|---|
| 1 | -3.892579 | -5.387578 | -2.965162 | -1.493404 | -2.317791 | -0.609824 | 1420 | 6/6 | 0.0 |
| 5 | -1.311273 | -2.884882 | -0.600646 | -0.538546 | -1.38603 | 0.159849 | 282 | 5/6 | 0.0 |
| 10 | -0.673285 | -1.872589 | -0.040656 | -0.183293 | -0.978899 | 0.462339 | 140 | 5/6 | 0.0 |
| 21 | -0.50875 | -1.192075 | -0.045907 | -0.393387 | -0.774443 | 0.620555 | 62 | 5/6 | 0.0 |
| 63 | -0.607101 | -1.565362 | 0.047432 | -0.405292 | -0.95935 | 0.59243 | 17 | 0/0 | 0.0 |

- per-year Sharpe @ h=21: 2013~ -1.409443; 2014~ 0.397092; 2015~ -2.525105; 2016*~ -0.46413; 2017 NOT FIT; 2018~ -1.053898; 2019~ -0.967729
- rank-IC mean @ h=21: -0.014145 | implied turnover (1-rho_rank): 0.509766 | breadth (mean n_used): 2539.569 | offsets [-1.906961, -0.50875]
- R16-9 materiality (2016/2017 vs union of the 2015 and 2019 CIs): does not fire

## Acceptance bars (R16-8) -- passing = candidate, not tradeable
| signal | bar1 pooled ci_lo (2.5%) > 0 @ h=21 both cuts | bar2 sign stability n/n | bar3 turnover printed | verdict | DSR @ h=21 per cut (N=310, beside the bars, not a bar) |
|---|---|---|---|---|---|
| amihud_21 | PASS | FAIL | PASS | FAIL | t1000 0.0; t3000 0.000313 |
| blend_equal | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |
| continuation_5 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 1e-06 |
| high_vol_63 | FAIL | FAIL | PASS | FAIL | t1000 1e-06; t3000 0.0 |
| low_dollar_volume_21 | PASS | FAIL | PASS | FAIL | t1000 0.0; t3000 0.000115 |
| max_ret_21 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |
| momentum_126 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |
| momentum_252 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |
| momentum_volscaled_252 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 3e-06 |
| residual_momentum_252 | FAIL | FAIL | PASS | FAIL | t1000 3e-06; t3000 2.2e-05 |
| volume_shock_neg_63 | FAIL | FAIL | PASS | FAIL | t1000 0.0; t3000 0.0 |

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

## Cost/capacity view (caveat, not a bar; R17-8)
| signal | cut | h | variant | restriction | n_cells | ADV top ($M) | ADV bottom ($M) | advrank top | advrank bottom | one-way turnover | Sharpe k=0 | k=1 bps | k=2 bps | k=5 bps |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| amihud_21 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 34.9 | 826.5 | 0.10 | 1.00 | 0.74 | 0.73 | 0.72 | 0.70 | 0.66 |
| amihud_21 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 3.5 | 466.8 | 0.10 | 1.00 | 0.64 | 1.00 | 0.98 | 0.96 | 0.90 |
| blend_equal | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 172.6 | 127.5 | 0.54 | 0.21 | 0.89 | -0.23 | -0.25 | -0.26 | -0.30 |
| blend_equal | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 76.1 | 44.0 | 0.55 | 0.12 | 0.83 | -0.10 | -0.12 | -0.14 | -0.21 |
| continuation_5 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 149.1 | 149.5 | 0.19 | 0.16 | 1.74 | -0.04 | -0.08 | -0.12 | -0.24 |
| continuation_5 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 58.5 | 57.2 | 0.15 | 0.15 | 1.72 | 0.51 | 0.43 | 0.36 | 0.13 |
| high_vol_63 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 128.2 | 281.6 | 0.17 | 0.93 | 0.67 | 0.53 | 0.52 | 0.50 | 0.46 |
| high_vol_63 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 41.0 | 100.1 | 0.13 | 0.92 | 0.60 | 0.57 | 0.56 | 0.55 | 0.54 |
| low_dollar_volume_21 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 25.3 | 916.3 | 0.10 | 1.00 | 0.87 | 0.90 | 0.88 | 0.86 | 0.80 |
| low_dollar_volume_21 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 2.4 | 493.9 | 0.10 | 1.00 | 0.72 | 1.14 | 1.11 | 1.08 | 0.98 |
| max_ret_21 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 246.7 | 153.2 | 0.94 | 0.31 | 1.34 | -0.93 | -0.94 | -0.96 | -1.01 |
| max_ret_21 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 89.0 | 53.5 | 0.75 | 0.12 | 1.26 | -1.23 | -1.27 | -1.30 | -1.41 |
| momentum_126 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 160.4 | 126.2 | 0.39 | 0.16 | 0.98 | -0.19 | -0.21 | -0.22 | -0.28 |
| momentum_126 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 65.6 | 43.7 | 0.33 | 0.12 | 0.92 | 0.08 | 0.05 | 0.03 | -0.05 |
| momentum_252 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 171.0 | 125.8 | 0.57 | 0.23 | 0.79 | -0.04 | -0.06 | -0.08 | -0.13 |
| momentum_252 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 74.2 | 42.7 | 0.55 | 0.13 | 0.73 | -0.26 | -0.28 | -0.30 | -0.36 |
| momentum_volscaled_252 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 151.6 | 115.1 | 0.52 | 0.22 | 0.68 | 0.23 | 0.21 | 0.19 | 0.13 |
| momentum_volscaled_252 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 5 | 81.1 | 46.0 | 0.76 | 0.20 | 0.68 | 0.15 | 0.14 | 0.12 | 0.06 |
| residual_momentum_252 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 155.0 | 184.3 | 0.58 | 0.40 | 0.71 | 0.38 | 0.36 | 0.34 | 0.28 |
| residual_momentum_252 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 5 | 83.6 | 64.8 | 0.84 | 0.36 | 0.69 | 0.36 | 0.35 | 0.33 | 0.28 |
| volume_shock_neg_63 | 1000 | 21 | IncludeAuditedTerminalV1 | full | 7 | 107.2 | 157.4 | 0.10 | 0.31 | 1.76 | -0.16 | -0.31 | -0.45 | -0.89 |
| volume_shock_neg_63 | 3000 | 21 | IncludeAuditedTerminalV1 | full | 6 | 29.1 | 61.2 | 0.10 | 0.33 | 1.72 | -0.51 | -0.54 | -0.56 | -0.64 |

- Surcharge: illiquidity surcharge per rebalance s_k = k*1e-4 * decile_one_way_turnover * 0.5 * (1/advrank_top + 1/advrank_bottom), advrank = rank of the decile's mean_dollar_adv among the block's 10 deciles (ascending, 1 = least liquid) / 10, decile 0 = long leg, decile 9 = short leg, computed PER CELL from that cell's own quantile_spread.csv and subtracted from every element of that cell's offset-0 stride-21 net sub-series before the score() pooling; a cell without the mean_dollar_adv column pools unsurcharged; k = 0 reproduces pooled sharpe_net.

## Caveats (all load-bearing)
- Membership is the YEAR UNION, not as-of: a name that joined mid-year is admitted for the whole year.
- 19 corrupted pre-holiday sessions (2016-01-15..2018-02-16) contaminate 2016 and 2017 (all signals) and momentum_252/blend_equal 2018; those rows carry hole_flag=1 and `*`, as does every pooled row (R16-9).
- 2013 is a PARTIAL year: evaluation starts 2013-04-04, not 2013-01-01.
- 2014-2019 IncludeAuditedTerminalV1 is expected to collapse onto DropMissingForward; _ex34 is 2013-only (R16-6).
- N_14 = 30 configurations (cp14 canonical ledger); cp17 adds N_17 = 7 families x 5 x 2 x 2 = 140 in the cp17 sidecar ledger; year x cut cells are AR-7 restrictions, not trials (R16-2). Deflated Sharpe uses --declared-n.
- No capacity, borrow-availability or impact model: decile spreads at +/-1.0/n gross-2.0 weights only.
- `~` marks a per-year cell with n_obs < 20: wide interval -- indicative only.
- implied_turnover is 1 - rho_rank from signal_autocorr.csv (per signal, lag 1); that file has no horizon/variant/restriction columns, so one value serves every horizon of a signal.
- N = 310 = N_14 30 (cp14 momentum) + N_17 140 (cp17 batch 1) + N_18 140 (cp18 batch 2: 7 new families x 5 horizons x 2 variants x 2 restrictions); amihud_21 retained from cp17, not re-declared; long-term reversal not declared (warmup); every configuration was declared in its ledger before it ran
- Sources: C:/atx/data/equity_ic_training_2013_20260920/ic.csv; C:/atx/data/equity_ic_training_2013_20260920/quantile_spread.csv; C:/atx/data/equity_ic_training_2013_20260920/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2013_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2013_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2013_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2013_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2013_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2013_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2014_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2014_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2014_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2014_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2014_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2014_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2015_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2015_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2015_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2015_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2015_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2015_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2016_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2016_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2016_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2016_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2016_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2016_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2017_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2017_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2017_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2018_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2018_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2018_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2018_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2018_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2018_t3000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2019_t1000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2019_t1000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2019_t1000_20260921/signal_autocorr.csv; C:/atx/data/equity_scorecard18_ic_2019_t3000_20260921/ic.csv; C:/atx/data/equity_scorecard18_ic_2019_t3000_20260921/quantile_spread.csv; C:/atx/data/equity_scorecard18_ic_2019_t3000_20260921/signal_autocorr.csv

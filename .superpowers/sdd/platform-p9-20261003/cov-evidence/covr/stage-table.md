| key | Debug | Release | Debug + ucrtbase exp2 | Release + ucrtbased exp2 |
|---|---|---|---|---|
| `const/exp2_arg/vol_hl84` | `bf88618618618618` | `bf88618618618618` | `bf88618618618618` | `bf88618618618618` |
| `const/exp2/vol_hl84` | `3fefbcae0646865c` | `3fefbcae0646865d` | `3fefbcae0646865d` | `3fefbcae0646865c` |
| `const/exp2_arg/nw_hl252` | `bf70410410410410` | `bf70410410410410` | `bf70410410410410` | `bf70410410410410` |
| `const/exp2/nw_hl252` | `3fefe97f887a34a8` | `3fefe97f887a34a8` | `3fefe97f887a34a8` | `3fefe97f887a34a8` |
| `const/exp2_arg/corr_hl504` | `bf60410410410410` | `bf60410410410410` | `bf60410410410410` | `bf60410410410410` |
| `const/exp2/corr_hl504` | `3feff4bdc935c3a0` | `3feff4bdc935c39f` | `3feff4bdc935c39f` | `3feff4bdc935c3a0` |
| `const/exp2_arg/specific_hl84` | `bf88618618618618` | `bf88618618618618` | `bf88618618618618` | `bf88618618618618` |
| `const/exp2/specific_hl84` | `3fefbcae0646865c` | `3fefbcae0646865d` | `3fefbcae0646865d` | `3fefbcae0646865c` |
| `const/exp2_arg/vra_hl42` | `bf98618618618618` | `bf98618618618618` | `bf98618618618618` | `bf98618618618618` |
| `const/exp2/vra_hl42` | `3fef79e9aca4c53e` | `3fef79e9aca4c53e` | `3fef79e9aca4c53e` | `3fef79e9aca4c53e` |
| `iso/eigen_input_m10` | `d1c67087aff77a0b` | `d1c67087aff77a0b` | `d1c67087aff77a0b` | `d1c67087aff77a0b` |
| `iso/eigen_output_m10` | `c940b51c1146bf33` | `c940b51c1146bf33` | `c940b51c1146bf33` | `c940b51c1146bf33` |
| `iso/eigen_input_m30` | `bea18180f50c6f7b` | `bea18180f50c6f7b` | `bea18180f50c6f7b` | `bea18180f50c6f7b` |
| `iso/eigen_output_m30` | `c936300a6a700442` | `c936300a6a700442` | `c936300a6a700442` | `c936300a6a700442` |
| `iso/ewma_hl21_lags5` | `c6c5755cb5d8dd58` | `b09525074ce7660c` | `b09525074ce7660c` | `c6c5755cb5d8dd58` |
| `iso/ewma_hl42_lags5` | `542652bdce2387f1` | `542652bdce2387f1` | `542652bdce2387f1` | `542652bdce2387f1` |
| `iso/ewma_hl84_lags5` | `da59e49368446a41` | `4fd4e65e0bdafb6b` | `4fd4e65e0bdafb6b` | `da59e49368446a41` |
| `iso/ewma_hl252_lags5` | `57294d463f5be93d` | `57294d463f5be93d` | `57294d463f5be93d` | `57294d463f5be93d` |
| `iso/ewma_hl504_lags5` | `91479cc0d3ce5853` | `024198147ac98b2e` | `024198147ac98b2e` | `91479cc0d3ce5853` |

**v11** (fold of 300 per-session digests; sessions Debug != Release; first)

| stage | Debug | Release | Debug + ucrtbase exp2 | Release + ucrtbased exp2 | differ | first |
|---|---|---|---|---|---|---|
| x.slot | `d050025d0d3c` | `d050025d0d3c` | `d050025d0d3c` | `d050025d0d3c` | 0 | - |
| x.eligible | `40bb503d9c8b` | `40bb503d9c8b` | `40bb503d9c8b` | `40bb503d9c8b` | 0 | - |
| x.styles | `449d8bad2bbd` | `449d8bad2bbd` | `449d8bad2bbd` | `449d8bad2bbd` | 0 | - |
| f.return | `b62e566e96bc` | `b62e566e96bc` | `b62e566e96bc` | `b62e566e96bc` | 0 | - |
| ewma.fast_var | `242217d46ae3` | `1e8fe10b226a` | `1e8fe10b226a` | `242217d46ae3` | 298 | 2 |
| ewma.nw_zero_lag | `e2c95a759868` | `e2c95a759868` | `e2c95a759868` | `e2c95a759868` | 0 | - |
| ewma.nw_hac | `2e2d0291ade7` | `2e2d0291ade7` | `2e2d0291ade7` | `2e2d0291ade7` | 0 | - |
| ewma.corr | `b38277ddcd43` | `c17acf1a671c` | `c17acf1a671c` | `b38277ddcd43` | 298 | 2 |
| vra.kish_n_eff | `7a513d6765f3` | `6cbba7c7a9cf` | `6cbba7c7a9cf` | `7a513d6765f3` | 280 | 2 |
| vra.bias_factor | `2830d829e322` | `5e9d9253d6d8` | `5e9d9253d6d8` | `2830d829e322` | 201 | 65 |
| vra.lambda2_factor | `aa94cec95f2a` | `249b6d2c2fad` | `249b6d2c2fad` | `aa94cec95f2a` | 204 | 84 |
| F.serial_var | `af5e9d6f0e9e` | `b24a6cc8853b` | `b24a6cc8853b` | `af5e9d6f0e9e` | 237 | 63 |
| F.raw | `36ec4f741693` | `73ada72f6792` | `73ada72f6792` | `36ec4f741693` | 237 | 63 |
| F.floor | `0ba0dbea7a9c` | `ce72acf520ec` | `ce72acf520ec` | `0ba0dbea7a9c` | 237 | 63 |
| F.eigen | `0ba0dbea7a9c` | `ce72acf520ec` | `ce72acf520ec` | `0ba0dbea7a9c` | 237 | 63 |
| F.eigen_gamma | `aefcd155000e` | `aefcd155000e` | `aefcd155000e` | `aefcd155000e` | 0 | - |
| F.final | `7db2a323c3c6` | `f0844a5aaf3d` | `f0844a5aaf3d` | `7db2a323c3c6` | 237 | 63 |
| F.structural_border | `adc051a417e0` | `67ecd10ce4f2` | `67ecd10ce4f2` | `adc051a417e0` | 173 | 63 |
| D.bias_specific | `4a06cb758834` | `3f46f6cf6469` | `3f46f6cf6469` | `4a06cb758834` | 28 | 254 |
| D.lambda2_specific | `11a746030c7a` | `154b4855ac69` | `154b4855ac69` | `11a746030c7a` | 8 | 273 |
| D.structural_bounds | `1fb54b5735d3` | `9b8f24ea1c4d` | `9b8f24ea1c4d` | `1fb54b5735d3` | 47 | 252 |
| D.specific_variance | `3fdc7d6b0145` | `c98f5f825ee6` | `c98f5f825ee6` | `3fdc7d6b0145` | 48 | 252 |
| D.flags | `d5dda953736e` | `d5dda953736e` | `d5dda953736e` | `d5dda953736e` | 0 | - |
| block | `63af8bcc63cf` | `76347f2af0a7` | `76347f2af0a7` | `63af8bcc63cf` | 293 | 2 |

**covv1** (fold of 300 per-session digests; sessions Debug != Release; first)

| stage | Debug | Release | Debug + ucrtbase exp2 | Release + ucrtbased exp2 | differ | first |
|---|---|---|---|---|---|---|
| x.slot | `d050025d0d3c` | `d050025d0d3c` | `d050025d0d3c` | `d050025d0d3c` | 0 | - |
| x.eligible | `40bb503d9c8b` | `40bb503d9c8b` | `40bb503d9c8b` | `40bb503d9c8b` | 0 | - |
| x.styles | `449d8bad2bbd` | `449d8bad2bbd` | `449d8bad2bbd` | `449d8bad2bbd` | 0 | - |
| f.return | `b62e566e96bc` | `b62e566e96bc` | `b62e566e96bc` | `b62e566e96bc` | 0 | - |
| ewma.fast_var | `242217d46ae3` | `1e8fe10b226a` | `1e8fe10b226a` | `242217d46ae3` | 298 | 2 |
| ewma.nw_zero_lag | `e2c95a759868` | `e2c95a759868` | `e2c95a759868` | `e2c95a759868` | 0 | - |
| ewma.nw_hac | `2e2d0291ade7` | `2e2d0291ade7` | `2e2d0291ade7` | `2e2d0291ade7` | 0 | - |
| ewma.corr | `b38277ddcd43` | `c17acf1a671c` | `c17acf1a671c` | `b38277ddcd43` | 298 | 2 |
| vra.kish_n_eff | `7a513d6765f3` | `6cbba7c7a9cf` | `6cbba7c7a9cf` | `7a513d6765f3` | 280 | 2 |
| vra.bias_factor | `265380dad8bd` | `8f2e1dc1ac1a` | `8f2e1dc1ac1a` | `265380dad8bd` | 201 | 64 |
| vra.lambda2_factor | `0623e7d3788e` | `5fd58b89ae04` | `5fd58b89ae04` | `0623e7d3788e` | 150 | 84 |
| F.serial_var | `af5e9d6f0e9e` | `b24a6cc8853b` | `b24a6cc8853b` | `af5e9d6f0e9e` | 237 | 63 |
| F.raw | `36ec4f741693` | `73ada72f6792` | `73ada72f6792` | `36ec4f741693` | 237 | 63 |
| F.floor | `0ba0dbea7a9c` | `ce72acf520ec` | `ce72acf520ec` | `0ba0dbea7a9c` | 237 | 63 |
| F.eigen | `42566ee1dd7d` | `700578bfe46b` | `700578bfe46b` | `42566ee1dd7d` | 237 | 63 |
| F.eigen_gamma | `9cd2eb8c588c` | `3ea31214ba0d` | `3ea31214ba0d` | `9cd2eb8c588c` | 237 | 63 |
| F.final | `b416ef8cc674` | `5e5d714c0cc3` | `5e5d714c0cc3` | `b416ef8cc674` | 237 | 63 |
| F.structural_border | `8a809585fd3d` | `44473562b0a4` | `44473562b0a4` | `8a809585fd3d` | 173 | 63 |
| D.bias_specific | `4a06cb758834` | `3f46f6cf6469` | `3f46f6cf6469` | `4a06cb758834` | 28 | 254 |
| D.lambda2_specific | `11a746030c7a` | `154b4855ac69` | `154b4855ac69` | `11a746030c7a` | 8 | 273 |
| D.structural_bounds | `1fb54b5735d3` | `9b8f24ea1c4d` | `9b8f24ea1c4d` | `1fb54b5735d3` | 47 | 252 |
| D.specific_variance | `3fdc7d6b0145` | `c98f5f825ee6` | `c98f5f825ee6` | `3fdc7d6b0145` | 48 | 252 |
| D.flags | `d5dda953736e` | `d5dda953736e` | `d5dda953736e` | `d5dda953736e` | 0 | - |
| block | `170a839f49d4` | `d2f0b3c846e1` | `d2f0b3c846e1` | `170a839f49d4` | 293 | 2 |

## 11. Review Focus (failure modes no task's acceptance run exercises; each pinned to a fixture in the owning task)

1. **v5 with θ = 1, dust 0, L = 1 must reproduce `baseline-v1` band 0 fraction 1 bit-for-bit** (weights, turnover, recipe minus rule name). → T30 fixture `AimPartialV5_ThetaOne_MatchesBaseline`.
2. **Dust band must never block entry:** a member with current 0 and |L·desired| > dust/N_d trades on the first rebalance. → T30 fixture `AimPartialV5_DustDoesNotBlockEntry`.
3. **Per-name rate on names without liquidity (ADV 0 / absent / < 20 vol pairs) uses rate_min and never NaN.** → T36 fixture `PerNameRate_NoLiquidity_UsesMin`.
4. **Autocorrelation on a candidate that is flat or all-NaN for a stretch** yields finite ρ̄ and g ∈ [0.05, 1]; a constant candidate is `degenerate-zero-variance` with weight 0. → T31 fixture `test_aim_gain_degenerate_and_gappy`.
5. **A terminal event dated after a reprint, or a terminal event on a name never held, must not touch NAV;** a terminal event on a short must apply the *short* haircut (+0.30 loss convention as S3). → T32 fixture `TerminalReturn_ReprintAndShortSide`.
6. **`ew-theme-v1` bytes unchanged** after the fitter edit. → T31 existing `byte_stability` test + T38 SHA equality of a re-fit (`9a9c949a…`).


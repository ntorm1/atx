# P9-X own mechanics derivation before return reads

R15-X-LAUNCH, registration ac60bd39 and prelaunch d964df86.
Own derivation only; fresh DERIV must use raw mechanics.json, not this record.
Source scan and frozen/input/CRT/reference/ledger checks passed before these reads.
The four H phases were reused; one NAV run stopped after nav.

| Measure | Observed | Registered limit | Holds |
|---|---|---|---|
| mean_gross_leverage_all_rows | 1.5745869901490488 | [0.8398191654289909, 1.6946383308579818] | True |
| mean_net_leverage_all_rows | 0.009567296396714624 | 0.03426417680315231 | True |
| tau_gmv_mean | 0.027435286052218615 | 0.3426417680315231 | True |
| tau_gmv_p95 | 0.034127886700244255 | 0.5139626520472845 | True |
| max_return_identity_error | 3.5735303605122226e-16 | 1e-09 | True |
| max_cash_book_relative_error | 7.921937179839092e-14 | 1e-09 | True |

All six scaled checks hold. Native primary exposure mean/max are
1.5743764230832007 / 1.8619722830838408; all five main maxima are below 2.0.
The separate primary L_t finite/[1,2] check holds for 1004 CSV rows. No cap/schema
conflict arose; no new deciding inequality was added. Mean L_t is deferred to
registered prints. Full exposure values and binding hashes are in mechanics-first.

No return, Sharpe, paired statistic or book field has been read. Commit this
derivation before running the sole paired producer; rho-only evidence will be
committed before paired scalars. Ledger stays 138 / fe1bc97ecf9ccef7f042d637730581c3410792798ef2ab0a4a7a7fa2bfbc05f8.

The initial post-NAV metadata regex omitted the native NAV attempt suffix;
it was corrected without changing source/scientific fields or rerunning NAV.

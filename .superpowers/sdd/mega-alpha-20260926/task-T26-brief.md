# T26 — v4 construction study: industry-neutral and liquidity-aware paper books (TRAIN only; root runs)
Context: v4 TRAIN NAV (S2 x swap-fin) best net .687 (band 2, fraction .25), gross ~.8-1.1; price-risk-v1 neutralizer
= cross-sectional QR residual on [1, beta252, vol63, ladv63] (see NAV/fitter "context" strings and studies/postmortem_v3.py
section E, which already reproduces the neutral paper book of a saved combined to corr .94 with NAV gross).
Deliver studies/v4_construct_study.py (+ synthetic pytest), git add -f; the ROOT runs it (<=180 s, <=1536 MiB per call).
Reuse postmortem_v3.py loaders/paper-book code (import it). Inputs (args): v4 combined build-equity/mega-v4w-train-1/
train_combined.* (TRAIN), role recent-fast-train-2020-2022-v2, fields build-equity/recent-fast-train-2020-2022-v2-fields-v6
(grp_ff12, grp_ff49, me_company), NAV daily CSV build-equity/mega-nav-v4-train-b1/ for alignment checks.
Paper books (gross, fwd r[d+2] like the NAV; daily; report SR, vol, mean, by-year SR, mean daily turnover of the
paper weights, corr with NAV gross for the baseline):
  P0 price-risk-v1 (baseline; must match postmortem E's method);
  P1 price-risk-v1 + FF12 industry dummies in the residual basis (names with NaN grp_ff12 = their own "unknown" group);
  P2 price-risk-v1 + FF49 dummies (groups < 5 members merged into their FF12 parent or "unknown"; state the rule);
  P3 = P1 with liquidity-scaled weights w_i * min(1, ADV63_i / median ADV63)^0.5 then re-neutralized (impact proxy);
  P4 = P1 with a partial-adjustment book (w_t = w_{t-1} + 0.25 (target_t - w_{t-1})) to show tracking loss vs turnover.
Also report the S2-like cost proxy per book: sum over days of sum_i |dw_i| * 20 bps (declared constant) -> net-proxy SR.
Hard rules: TRAIN only (refuse any role/date past 2022-12-31); no per-candidate or per-theme performance output (book
level only). Report task-T26-report.md with exact root commands.

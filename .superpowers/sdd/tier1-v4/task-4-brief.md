### Task 4: Financial ratio suite `ratios/` (WRDS WFRS analog)

Owner coverage steering: read `owner-coverage-addendum.md`; ratio definitions and measured coverage must support the profitability/growth requirements matrix later owned by Task 5 and consumed by Tasks 7/16.

Spec: gap-analysis §2 item 3 (binding). Inputs: `compustat_std/` (Task 3) and `market/shares_daily` + prices. Output
monthly (month-end, GC1 monthly rule) and a daily as-of view (catalog macro). Add Piotroski F-score, Altman Z,
Ohlson O, Beneish M as composite scores (documented formulas). Files: new `alpha_panel/ratios.py`, tests. Report:
`task-4-report.md`.

R8 formula identity: use the WRDS-authored Financial Ratio Manual definitions for mandated WFRS names (https://people.tamu.edu/~cschulman/ECMT461/Termp/WRDS_Industry_Financial_Ratio_Manual.pdf, appendix pp5-7). The older INDRATIOS macro is a different specification. Economically valid alternatives need distinct extension names, not just prose disclaimers under a conflicting WFRS mnemonic. Unsupported exact inputs remain NULL with a reason and a Task5 primitive gap; do not synthesize diluted EPS from outstanding cover shares. Preserve independently recomputed issuer-cap/earnings variants as separately named extensions.


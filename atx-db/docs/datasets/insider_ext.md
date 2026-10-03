# Insider price qualification

Schema: `atx.alpha-panel.insider-ext/v2`. The native `insider_measures` build
requires published insider and panel inputs and records both manifest hashes.

The existing accession/trade keys, issuer CIK, transaction date and source
acceptance clock are preserved. `value` is USD and equals shares times source
price only when price and raw panel close are finite/positive and price is within
the inclusive 0.2–5 times close band. Multiple issuer share classes on the date
are ambiguous without a dated security link: value stays NULL. `panel_close` is
unadjusted USD/share; `price_sanity` explains qualification or missingness.
Shares and trade counts survive value qualification failure.

Row availability is the later of the original filing clock and conservative
22:00 UTC of transaction date, preventing same-day close evidence from becoming
visible before close. Daily use requires available_at strictly before the prior
session's 22:00 UTC cutoff. Monthly event vintages are the existing stage contract.

Coverage and monthly net-value p99.9 remain UNMEASURED for this revision; no real
build is authorized yet. Fixture evidence covers inclusive boundaries,
out-of-range prices, ambiguous classes and the conservative close clock.
Stage acceptance additionally needs pinned stable input files, final universe
bindings, real rebuild and independent review. Existing monthly aggregation
reports observed sums; incomplete price coverage must be evaluated separately.

# Stock-transition source review

Independent source and fixture approval, 2026-09-26. No build, native test,
strategy run, warehouse access or source-Parquet scan was performed by this
reviewer. Runtime qualification belongs to the root's separate receipts.

| Scope | Reviewed commits |
|---|---|
| Engine API | `40d4298ad4aabffcc0547678addf7979acac02a1` |
| Engine production | `cec8b9bdb39cf6e96d9b7746866c5230cdf1b93c` |
| Nine engine fixtures | `53b135221165ca1af788c16822c1ee9ab25fdabf` |
| Runner production | `dea0b92292de6fbe04101f68c5f9b11d77857808` |
| Runner accumulated-total overflow checks | `1072123ba32cc888e5bfe420912f7bb5874f914d` |
| Four runner fixtures | `d2ce1b6ef2b30cab05371935e706dc8927e3029a` |

No remaining source blocker was found in the bounded review. The engine marks
the existing successor position before adding signed raw-share delivery. It
cancels predecessor queued targets, preserves other pending targets, and nets
ordinary successor fills against the delivered book. Fixed cash remains a signed
nonspendable entitlement; short liabilities retain reserves and modeled borrow.
Strict effective/public clocks, prior raw/adjusted basis, successor observation
and source pins are checked. Pre-role extinction creates no opening entitlement;
inactive events need no successor axis, although scalar event fields remain
validated. Active chains and terminal successors are explicitly unsupported.

The independent fixture ledger agrees: predecessor 375/raw50 at ratio2 produces
330 successor dollars at raw22; the existing successor 125 marks to137.5, giving
NAV967.5 and successor holding467.5. A delayed target125 then sells342.5. The
inverse fixed-cash case yields NAV957 and next interval borrow0.5425. Fixtures
also cover axis permutation, additive predecessors, absent-but-finite successor
prices, unrelated missing marks, future-prefix invariance, separate cash/stock
record indexes and empty-stock cash-route hash/array parity.

The runner checks external document pins and archive identity before rebinding
events to a verified role manifest. Every sign and role receives the same event
policy. Historical VM/blend support retires only the predecessor under strict
decision clocks; successor membership is unchanged. Cash and stock diagnostics
remain separate, combined event/report allocations are admitted, and accumulated
diagnostic overflow refuses publication. Empty-stock legacy/cash routes retain
their recipe branches. Four owning fixtures exercise these actual caller paths.

Already-completed evidence checks corroborated JAG's 447/1000 PE Class A terms
and WCG's 338/100 CNC plus USD120 terms. Public completion precedes the respective
Jan10 and Jan23 22UTC recognition marks. The WCG wire's displayed Jan23 11:04ET
supports the conservative 16:05UTC known-by clock; its final Jan23 print does not
move completion to the first missing session. Primary pages inspected:
[JAG issuer release](https://www.prnewswire.com/news-releases/parsley-energy-completes-acquisition-of-jagged-peak-energy-300984916.html),
[JAG successor 8-K](https://www.sec.gov/Archives/edgar/data/1685715/000119312520005402/d864557d8k.htm),
[WCG issuer release](https://www.prnewswire.com/news-releases/centene-completes-acquisition-of-wellcare-creating-a-leading-healthcare-enterprise-focused-on-government-sponsored-healthcare-programs-300992269.html),
[Centene 8-K](https://www.sec.gov/Archives/edgar/data/1071739/000114036120001310/form8k.htm),
[WellCare 8-K](https://www.sec.gov/Archives/edgar/data/1279363/000114036120001340/form8k.htm).

Read-only checks of the already-selected records verified exact IEEE values,
raw-times-factor adjusted values and these artifact hashes under
`atx-impl/strategies/` in pool2:

| Artifact | SHA-256 |
|---|---|
| `evidence/jag_20200110.research.json` | `785d6bda81d4e8ce9aaa288df588935f4c097606fabe0798f7fa3c5ed1da9648` |
| `evidence/jag_pe_20200110.source-audit.json` | `7d873526fa69428ce51fd67272e510fba3e5c8c0dec4776b0abfb50ea156c63f` |
| `evidence/jag_pe_20200110.role-basis.json` | `daf2ff3d3e2d3172e33ba94e48eecaf12ff12373534e1d8c6dae5c33ba2c6831` |
| `evidence/wcg_20200123.research.json` | `b1b4bad2022603c48be68ae123ce1b41dd173b281abd10c49d2247cee2626bc1` |
| `evidence/wcg_cnc_20200123.source-audit.json` | `d109e8139f8b23c848acbbad20d2b36f949521186bfe89ecad94b373eae48740` |
| `evidence/wcg_cnc_20200123.role-basis.json` | `8408b353c3f4f6ccabef05a4fb7dd5f29e0ec20f7da3dc487554cf36d2325793` |
| Two-event `slow_price_volume_24_v1.stock_transitions.json` | `2fc4cc34103cb7cf591b7a27e4b8da58f749d76193e36fe474014c66f3d76691` |

This is research-book arithmetic with continuous fractional units. Account
delivery, physical fractional settlement and stock-loan transfer are unverified;
fixed-cash settlement remains unknown. There is no strategy performance claim.
Per the user's latest direction, no further action research or coverage work is
part of this review.

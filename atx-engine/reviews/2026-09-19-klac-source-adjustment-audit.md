# KLAC source adjustment exception — 2026-09-19

The new holdings replay's independent decimal oracle cannot complete the existing
31-date native diagnostic: a held position in `spiderrock.securityID=38946`
requires a missing mark on 2026-06-12 (research index 29). The archive's display
ticker is KLAC. This is a source-quality exception, not permission to replace its
return with zero or to forward-fill a split-adjusted price.

An additional streaming read of the original supplied ZIP checked all 31,598,499
rows and completed the ZIP member CRC verification in 113.09 seconds. File size
and modification time were unchanged during the read. The original compressed
SHA256 remains the earlier preparation measurement; this inspection did not
recompute it. Exact extracted rows and their byte hashes are retained in
`build-equity/audits/iteration5-source-gap.json` with the inspection script hash.

| Source date | Open | High | Low | Close | Cumulative factor | Reported shares |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 2026-06-11 | 2213.37 | 2431.29 | 2206.28 | 2411.64 | 1 | 130627521 |
| 2026-06-12 | 241 | 254.54 | 254.41 | 254.41 | 1 | 130627521 |
| 2026-06-15 | 264.99 | 264.99 | 256.39 | 256.39 | 1 | 130627521 |

The June 12 low exceeds the open. The existing preparation policy therefore
correctly excludes the whole row from the accepted ZIP. June 11 and June 15 remain
accepted; their cumulative factors are also 1. The original June 12 `totalReturn`
is `-0.894507465876202`, consistent with the raw price ratio rather than removal
of the split discontinuity. Reported shares do not change over these rows.

KLA announced a ten-for-one forward split on May 7, with split-adjusted trading
starting June 12. Its June 12 filing confirms the corporate action. These are
primary issuer/regulatory references:

- [KLA announcement and effective date](https://ir.kla.com/sec-filings/all-sec-filings/content/0001193125-26-212093/d116682dex991.htm)
- [KLA June 12 Form 8-K](https://www.sec.gov/Archives/edgar/data/319201/000119312526269375/d144278d8k.htm)

It follows that multiplying these particular raw closes by the supplied factor
does not remove this confirmed split. Checkpoint 3 proved exact implementation
of the declared price transformation; it did not certify every source factor.
This exception prevents describing the current adjusted-close field as a verified
total-return series across all securities and dates. Merely restoring the missing
June 12 row would still leave an economic adjustment error.

Keep the full-window diagnostic as an expected missing-held-mark rejection, with
no completed report manifest. A separately declared prefix ending June 11 may be
used to check native accounting against the decimal oracle; it cannot establish
strategy performance or erase this exception from the dataset's acceptance record.

The next data correction needs a versioned corporate-action reference, exact
security/date matching, reconciliation of source factors and return identities,
and a separately recorded adjustment or rejection decision. A hard-coded symbol
patch, guessed low, or inferred split from a large return alone is insufficient.
Preserve original prices, source factors, reported shares, and the exception
evidence. Source vintage and common-stock eligibility remain separate open issues.

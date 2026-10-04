# P9 source clocks and resource continuation

The complete P9 request remains included in goal section8. This checkpoint adds
source evidence and cache maintenance, not financial admissions or release gates.
Readinessv10 remains the latest independently approved matrix: eight financial
units retained, seven independently audited, no release-ready gold panel.

## Distributed NYSE originals

`p9-original-source-intake-v2.py` captured four exact non-SEC URLs, with durable
reservation/outcome records, no redirect/retry and a2MiB response bound. Guard
completed0 in7s at37,982,208 peak bytes under0.2GiB. Two Nasdaq-distributed original
NYSE releases returned200, totalling337,373bytes. The BLS CPI archive index and
January15,2010 release each returned403; both1323byte responses are preserved,
marked unsuccessful/unqualified and are not CPI source captures. No BLS retry
or access-control workaround followed. Raw originals remain under
`atx-db/data/raw/source_intake/p9_original_v2`, outside Git.

The actual [2020 distributed release](https://www.nasdaq.com/press-release/nyse-group-announces-2021-2022-and-2023-holiday-and-early-closings-calendar-2020-12)
states a December28,2020 3:00pm EST publication time. The parsed value is
2020-12-28T20:00Z. The [2021 distributed release](https://www.nasdaq.com/press-release/nyse-group-announces-2022-2023-and-2024-holiday-and-early-closings-calendar-2021-12)
states December27,2021 9:30am EST, parsed as2021-12-27T14:30Z. These publisher
labels have minute precision; the conversion does not establish second precision.

All10/11 factual holiday-table rows respectively match the previously pinned ICE
originals, as do both/all3 complete early-close paragraphs. The2020 pair has the
same explicit BusinessWire source URL20201228005203. The2021 Nasdaq copy links
20211227005062, but the ICE capture has no BusinessWire link. The latter relation
is therefore explicitly false, not silently inferred from matching tables.

`nyse-distributed-clock-preflight-v1.py` stopped at this2021 missing-link condition:
failed1/1s/30,396,416bytes. Its original code and failure are preserved. V2 is an
explicit comparison diagnostic that emits that false relation and the separate
true table/paragraph comparisons; completed0/1s/31,137,792bytes. It does not
weaken a product gate or certify the source. A future original-format parser
must establish the actual provenance of its admitted source through its fixed
contract. Both ICE publication clocks remainNULL. Calendar admission remains
false. Complete weekday authority, amendment corpus, source rights and independent
product validation remain open. T17's independent metadata review is PASS/PASS
in `review-p9-source-intake-v2.md`; that review grants no calendar admission.

The [BLS archived CPI directory](https://www.bls.gov/bls/news-release/cpi.htm) and
[January2010 release](https://www.bls.gov/news.release/archives/cpi_01152010.htm)
were readable through the research tool and identify a possible original-release
route, but direct capture failed. The local configured warehouse probe remains
absent-only evidence; a scoped filename inspection of `data/raw/fred` found
40 retained files, comprising rates/FX/VIX and H10 metadata/receipts, with no CPI
filename. This is not an exhaustive in-house CPI absence proof. No CPI row is
published or admitted, and no macro value is used for evaluation.

## Disk and cache maintenance

`disk-space-preflight-v5.py` completed metadata-only in2s/25,755,648bytes. It
inventoried named database cache/generated roots, skipped reparses and deleted
nothing. The17,966,114,530bytes in generic `_tmp` do not establish disposable
ownership. Original FSDS/companyfacts staging and foreign or interrupted outputs
remain intact. Active editor type-checker processes also ruled out deleting
the111,374,768byte `.mypy_cache`. The already-blocked pytest233/234 directories
were not targeted or retried.

Ruling: the user's explicit unnecessary-cache cleanup request authorizes the
package manager's normal **unreachable-object prune**, using an exact checked
cache root, offline mode and in-use protection. This is materially different
from deleting unknown session scratch or installed environments. No `--force`,
`--ci`, environment deletion, install or download was used.

`prune-unreachable-uv-cache-v1.ps1` validated the exact resolved cache root and
all ancestors, the standard cache signature and executable identity before
invoking native `uv cache prune`. Completed0/5s/129,912,832bytes under0.3GiB.
The package manager reports202files/4.1MiB removed. Observed drive-free change
over the operation was1,875,968bytes; concurrent activity can differ from logical
removal. Combined with the previous five cleanup observations, the six observed
changes sum6,680,559,616bytes, not a claim of current total free space.

Read-only package-manager inventories found208.2MB of pip HTTP cache,890kB of
locally built wheels, and953,279,499bytes in the remaining uv cache. Those retained
entries were not broadly purged. Original sources, installed environments,
native admission payloads and failure evidence were not cleanup targets.

The drive still remains below the unchanged40GiB admission floor. No financial
audit7v2 or creator8 was launched. Recoveryv5 remains already complete and must
not be repeated. T16's family fixtures are still refused before child. T17's new
French source fixture reached the original internal40GiB check and failed
without native replay/materialization; its next attempt must also include the
original floor at the outer guard. The main implementation agents continue
source/publication work within the existing repair counters.

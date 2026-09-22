# AR6 price-source correction: independent review

Reviewed once by a Codex agent on 2026-09-20: implementation `dc130c53`, its
brief/report, the current two adapters, source QA, focused test source, related
documentation, and the controller memory guard. No tests were rerun and no
warehouse was opened. This is a static review, not a new production measurement.

**Disposition: two Important findings; no Critical findings.** Fixes may be
accepted on the implementer's report under the program's review ruling.

## Important AR6-R1: quarantined keys do not remove an earlier plain-loader bar

`atx-db/src/atx_db/ticker_history.py:401` removes all repeated positive vendor/date
rows from the canonical frame. At `:890`, replacement deletes existing bars only
by joining that already-filtered `equity_daily_bars_load` frame. Consequently, if
the warehouse already contains one accepted bar for vendor 101/date D, reimporting
two conflicting source rows for 101/D produces no incoming canonical key and
leaves the old accepted bar visible. The final duplicate cleanup cannot detect
this: only one old row remains. The fresh-empty-store cross-chunk fixture does
not exercise replacement. This also matters when correcting rows loaded by the
older volume-winner behavior.

Drive canonical invalidation from the selected incoming original vendor/date
keys, including quarantined keys, before inserting accepted rows, with source
and selected-extent isolation. Preserve other vendors/sources/dates. Add one
focused previously-loaded-then-quarantined replacement case, including canonical
identity changes if the fix can encounter them. The bulk publisher's complete
source DELETE/INSERT does not have this particular defect.

## Important AR6-R2: plain identity selection depends on load order/arbitrary ticker

The newly added identity reuse at `ticker_history.py:385` selects
`arg_max(security_id, source_loaded_at)` for an identifier. Two equivalent link
sets loaded in opposite orders can therefore assign a positive vendor ID to
different canonical securities. The timestamp is a warehouse-load artifact,
not identity evidence; equal timestamps also have no stable tie-break.

Additionally, the revised collision grouping at `:545` selects
`any_value(symbol)` across all historical display symbols for a positive vendor
ID. The chosen symbol is then included in `new_security_id` at `:579`. A renamed
non-primary vendor line can receive `...-OLD` or `...-NEW` depending on the
aggregate's chosen representative. Grouping by vendor ID correctly avoids
splitting a rename within the input, but an unordered display value must not
determine the surviving key.

Use an explicit deterministic identity policy. Prefer a stable positive vendor
identifier for collision keys and resolve existing competing links without
warehouse load timestamps; conflicts can remain explicitly unresolved. This
does not require authenticating historical CIK links or broadening AR6 into an
identifier-master redesign. Cover a renamed colliding line and reversed link
load order with focused evidence.

## Accepted scope and evidence

The prior/current-close correction agrees with the authoritative field
definitions: `closePr` is the previous session's adjusted close, while
`todayTicker` is the latest display symbol. The current vendor page states the
full-history delivery schedule separately; it does not establish vintages of
this local archive. [SpiderRock TickerHistory3 dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/).

Both adapters calculate same-row close times cumulative factor, reject invalid
products without a fallback, preserve raw quantities, and leave canonical
split_factor unknown. The bulk collision quarantine operates before canonical
filtering; original-dn diagnostics block duplicate predecessors and gaps. The
report correctly distinguishes internal source consistency from economic
validation and corrects the old original-row count using the hash-bound receipt.

Historical current-ticker/CIK shortcuts, modeled date-22 availability, source
vintages, genuine split evidence and the enumerated downstream return/share
readers remain explicit prerequisites. They are not claimed resolved by this
review. In particular, the queued downstream work must finish or its outputs
must be excluded before certification. The AR6 operator example still needs
the controller's lower memory/thread settings in place of 6 GB/eight threads;
the T11 review records the corresponding runbook issue.

## Controller memory guard

No blocking finding in `.superpowers/sdd/tier1-parity/run_memory_guarded.py`.
The supervisor enters the bounded job before Popen; the configuration enables
aggregate job memory and kill-on-close, allows no breakaway, verifies the set
limit, checks physical and commit headroom before launch, and stops only its own
job on low headroom. This matches Microsoft's documented committed-memory job
limit and normal CreateProcess child inheritance. [Job memory limit](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information),
[job inheritance and cleanup](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects).

The existing incremental 128 MiB probe receipt reports successful completion;
the controller reports allocation rejection after 104 MiB of chunks. I read the
receipt and did not rerun it. The native peak field is correctly named as an OS
accounting value, not an independently measured resident-set ceiling. Continue
one heavy workload at a time, with a DuckDB budget below the aggregate job cap;
this guard does not make unconstrained pandas materialization safe to complete.

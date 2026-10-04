# Original NYSE calendar source leads for Task1 and the market helper

Root captured five exact public ICE/NYSE publisher pages into
`C:/atx/atx-db/data/raw/source_intake/nyse_calendar_v1`. All returned HTTP200;
the source bodies total373,063B. The guarded intake completed5s with37,662,720B
peak under0.2GiB. Exactly five non-SEC attempts were durably reserved before
network access; redirects/retries were disabled, requests spaced at least1.05s,
each response bounded to2MiB. No SEC request or shared-budget grant occurred.
Raw source pages remain in the data landing; only metadata evidence is committed.

The pinned source inventory and actual hashes are in
nyse-calendar-source-intake-v1.json, with before/after captured-byte checks.
This is source discovery, not a calendar/source-clock admission.

| Capture | Publisher date | Announced years | Original publisher page |
| --- | --- | --- | --- |
|calendar-2017.html|2017-11-27|2018-2020|[ICE announcement](https://ir.theice.com/press/news-details/2017/NYSE-Group-Announces-2018-2019-and-2020-Holiday-and-Early-Closings-Calendar/default.aspx)|
|calendar-2019.html|2019-12-09|2020-2022|[ICE announcement](https://ir.theice.com/press/news-details/2019/NYSE-Group-Announces-2020-2021-and-2022-Holiday-and-Early-Closings-Calendar/default.aspx)|
|calendar-2020.html|2020-12-28|2021-2023|[ICE announcement](https://ir.theice.com/press/news-details/2020/NYSE-Group-Announces-2021-2022-and-2023-Holiday-and-Early-Closings-Calendar/default.aspx)|
|calendar-2021.html|2021-12-27|2022-2024|[ICE announcement](https://ir.theice.com/press/news-details/2021/NYSE-Group-Announces-2022-2023-and-2024-Holiday-and-Early-Closings-Calendar/default.aspx)|
|mourning-2018.html|2018-12-01|Closure2018-12-05|[ICE closure announcement](https://ir.theice.com/press/news-details/2018/New-York-Stock-Exchange-to-Honor-President-George-H-W-Bush/default.aspx)|

Two observed real changes support immutable planned versus realized vintages.
The December2020 table has no Juneteenth holiday; the December2021 table adds
Juneteenth for2022-2024. The separate December2018 mourning notice announces
a December5 closure absent from the earlier annual schedule. Earlier rank-known
plans cannot silently inherit either later change. This is an inference about
the required version model from those original announcements, not proof of a
complete update inventory or an exact earliest announcement clock.

Root's guarded clock preflight completed1s/25,595,904B under0.15GiB. The captured
pages expose date-only publication labels, no time/datetime attributes, no
recognized JSON-LD publication timestamps and no ISO datetime strings. HTTPDate
is the current2026 capture time. available_at_utc remainsNULL and clock_qualified
false for all five; do not manufacture midnight,22UTC or next-day publication.
The rendered release dates, title/year coverage and current captured hashes do
not establish the complete original publication UTC or original archive vintage.

Next qualification requires exact source publication-time evidence, complete
weekday/trading rule and covered-period authority, all applicable calendar
amendments with their own clocks, immutable vintage lineage, actual source rights
and full parser/typed-axis checks. Cash-equity scope must remain explicit; options,
bonds and late sessions must not be conflated with regular equity sessions.
Source-shaped positive fixtures must match supported original document formats;
an author-generated CSV/JSON with caller-supplied completeness dates cannot
replace the original covered-period evidence. No author source job was granted.

Root's subsequent structure preflight completed1s/26,402,816B and records
original table start tags/cells in nyse-calendar-structure-preflight-v1.json.
The four calendar tables have12/11/10/11rows respectively, including original
spacers. The mourning notice has one extracted short closure clause and a
digest of its complete paragraph. No standard Monday-through-Friday authority
was established. The original source remains necessary for full parser replay.

Primary-source research used the official publisher pages above. Business Wire
original-link opens for2017/2020 returned tool errors, and a primary-domain
search did not recover publication times. Those failures confer no clock.
Current schedule pages must not rewrite past vintages. All Task1/P9/source gates
remain open; strict pre2023 return evaluation/complete-forward-label limits stand.

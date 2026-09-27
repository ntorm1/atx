# Qualified filing-clock exporter and admission: bounded runtime evidence

2026-09-26. C++ compiled/tested source `08240b13`; Python owning checks ran at
`f0ffc66d`, with existing exporter checks at `08240b13` (documentation-only
difference). **25 C++ and 19 Python checks pass**, no failures or skips.
All 16 new owning cases pass: six FundamentalClockV4 and ten
FundamentalAcceptanceV2. Root source review is `08240b13`.

| Cohort | Passes | Native seconds |
|---|---:|---:|
| V4 decoder and existing identity/fundamental consumers | 25 | 0.004 |
| New acceptance-clock exporter | 10 | 0.194 |
| Existing exporter fiscal arithmetic | 9 | 0.002 |

The actual exporter is exercised against a synthetic sealed CompanyFacts ZIP
and hash-bound filing-clock projection. The fixture verifies emitted V4 rows,
separate true DEI shares, recipe/output hashes and refusal of a mismatched fact
vintage. Context and D1 identity acquisition are mocked; no new claim about
those loaders follows. Other cases cover independent same-day accession
publication, exact +46h fallback, modeled clock versus fact-vintage admission,
inherited qualification, conflict/correction, finite SUE arithmetic and legacy
+24h reproduction.

The C++ checks exercise actual V4 dispatch and alignment: strict equality
withholding, identity clocks, acceptance delays, revision availability,
malformed and absent-axis rows, modeled-clock override, resource budgets and
audit commit. A withheld snapshot retains an earlier qualified value only
under its old availability/period staleness clocks. It does not refresh or
revoke that historical value. Existing V2/V3 identity and fundamental arithmetic
checks also pass.

Build passed on its first attempt in **35.1366575 s**, Jobs2, including CMake's
reported17.7s configuration and0.9s generation. Exactly three CPPs compiled
(new decoder, new fixture, affected existing interval fixture), followed by
two links. PCHs and dependencies were retained. Available physical memory at
launch was1,274 MiB with3,381 MiB commit headroom; no other process was stopped.
This is a shared-host iteration measurement, not a controlled speed benchmark.

The companion JSON binds source identities, binary/XML/log/receipt hashes and
all 44 distinct case names across the two languages. Local index:
`build-equity/w2-d3-clock-qualification-index.json`, SHA256
`5297d42ef4cae72142b340a15a83a20cd56d8f1ecd6d1ad85de2bd457c22fe71`.
Counts overlap older packets and must not be added to historical qualifications.
The previous exposure-data executable has now been relinked; its earlier
12-check packet retains historical binary attribution.

This qualifies the explicit source-declared clock/true-share producer and
decoder slice. It does not authenticate real publication/vintage evidence,
produce split/class-qualified market cap, change legacy defaults, complete
short-interest identity, establish real coverage or close D3's full gate.
No real market payload, warehouse write, long benchmark or alpha admission.

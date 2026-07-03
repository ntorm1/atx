# SEC EDGAR Loader — Backfill & Parser Design Research

**Audience:** ats-eqt engineering. Pre-implementation research for `src/ats_eqt/edgar/`.
**Date:** 2026-05-09 (information current as of mid-2026 unless flagged `[unverified]`).
**Scope:** rate-limit reality, backfill strategy, URL conventions, caching, community-library survey, XBRL specifics, 13F gotchas, retry policy, on-disk layout, and concrete recommendations.

---

## 0. Executive summary — top 5 design decisions

1. **Default to 5 req/s with a 9 req/s ceiling, token-bucket-shaped, single-process global limiter.** SEC's documented hard cap is 10 req/s per IP; community libraries (`edgartools`, `sec-edgar-downloader`) converged on 9 req/s as the safe operational ceiling. We pick 5 req/s default to leave headroom for retries, with config to lift to 9. Token bucket so bursts of small JSON fetches don't punish us. (source: https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data ; https://deepwiki.com/dgunning/edgartools/8.1-http-client-and-rate-limiting)
2. **Use DERA Financial Statement Data Sets as the spine for fundamentals backfill, then enrich with companyfacts.json per-CIK; only walk full-index for forms DERA does not cover (e.g. 13F).** DERA is pre-flattened, quarterly, ~50 MB/qtr — one zip beats hundreds of thousands of HTTP calls. (source: https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets)
3. **Treat `Archives/edgar/data/{cik}/{accession}/...` as content-addressed and immutable; cache the bytes by `(URL, sha256)` forever.** Filings are append-only; amendments arrive as new accessions, never rewrite old URLs. ETag/If-Modified-Since add little once you've successfully fetched once. (source: https://deepwiki.com/dgunning/edgartools/7.3-http-client-and-caching)
4. **Per-filer iteration only via `data.sec.gov/submissions/CIK{10-digit}.json` with explicit overflow walking; do not assume `recent` covers history.** Large filers (Berkshire, Vanguard) have decades of filings and require following `files[].name` overflow JSONs. Hard limit is ~1000 records or last year per page. (source: https://sec-edgar-api.readthedocs.io/)
5. **Use Arelle as a subprocess (not a library import) for canonical XBRL validation; lean on companyfacts.json for everyday concept lookups.** Arelle is the SEC's reference processor but heavy and Python-2-flavored in places; subprocessing isolates it and lets us swap if we find a faster path. companyfacts gives us 80% of the value for ~1% of the cost. (source: https://arelle.org/arelle/ ; https://www.sec.gov/files/edgar/filer-information/specifications/xbrl-guide.pdf)

---

## 1. Rate-limit + access-policy current state (2026)

**Hard cap:** 10 requests per second per source IP (or per logical user, whichever is stricter). The SEC's announcement establishing this cap took effect **2021-07-27** and remains the operative policy in 2026. If you exceed it, the IP is throttled / blocked for **roughly 10 minutes** before traffic resumes. (source: https://www.sec.gov/filergroup/announcements-old/new-rate-control-limits ; https://dealcharts.org/blog/edgar-scraping-rate-limits-explained)

**Aggregation:** the limit is "regardless of the number of machines used to submit requests" — they consider the actor, not the box. Spreading across machines on the same egress NAT will not help and may hurt. (source: https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data)

**Required headers (paraphrased from SEC fair-access guidance):**

- `User-Agent`: must identify the requester. The community-canonical format used by `sec-edgar-api` and `sec-edgar-downloader` is `<Sample Company Name> <Admin Contact>@<Sample Company Domain>`, e.g. `ats-eqt research@ats-eqt.local`. The SEC asks specifically for **name + email**. (source: https://sec-edgar-api.readthedocs.io/ ; https://github.com/jadchaar/sec-edgar-downloader)
- `Accept-Encoding: gzip, deflate` — recommended; reduces bandwidth, recommended by SEC fair-access notes. (source: https://github.com/jadchaar/sec-edgar-downloader)
- `Host: www.sec.gov` (or `data.sec.gov`) — recommended explicitly per `sec-edgar-downloader`. (source: https://github.com/jadchaar/sec-edgar-downloader)

**Hosts and what they serve:**

| Host                   | Purpose                                                   |
|------------------------|-----------------------------------------------------------|
| `www.sec.gov`          | Filings, archives, full/daily indexes, raw documents      |
| `data.sec.gov`         | RESTful JSON APIs: submissions, companyfacts, companyconcept, frames |
| `efts.sec.gov`         | Full-text search backend (used by sec.gov UI)             |
| `api.edgarfiling.sec.gov` | Filer-side EDGAR Filing Toolkit (separate, not relevant for read access) |

(source: https://www.sec.gov/about/developer-resources ; https://data.sec.gov/)

**Block signals (in order of severity):**

- `403 Forbidden` — typically the IP-level block. The SEC returns 403 when an IP is rate-limited or when User-Agent is missing/malformed. This is a hard signal. (source: https://blog.finxter.com/solving-response-403-http-forbidden-error-scraping-sec-edgar/)
- `429 Too Many Requests` — softer rate-limit signal; less common than 403 from EDGAR specifically.
- `503 Service Unavailable` — server-side overload; respect `Retry-After` if present.

**No developer keys / authenticated paths.** As of 2026 the public EDGAR APIs and archives remain unauthenticated — no API keys, no OAuth, no registered-developer program. Identification is by User-Agent only. (source: https://sec-edgar-api.readthedocs.io/) `[unverified]` on whether SEC has hinted at one in 2026 — none in the cited resources.

**Fair-access principle (SEC language paraphrase):** "users should use efficient scripting, download only what you need, and moderate requests to minimize server load." Translating: bulk endpoints (DERA, full-index zips) are preferred over per-filing scrapes. (source: https://dealcharts.org/blog/edgar-scraping-rate-limits-explained)

---

## 2. Optimal backfill strategies

### 2.1 Decision matrix — when to use which source

| Source | Best for | Pros | Cons |
|---|---|---|---|
| `data.sec.gov/api/xbrl/companyfacts/CIK*.json` | Per-issuer fundamentals lookup | One JSON, all concepts, all history; cheap | One issuer at a time; lags filing by hours-to-days; only XBRL-tagged facts |
| `data.sec.gov/submissions/CIK*.json` | Per-issuer filing inventory | Authoritative list of every filing for an issuer | 1000-record pages; overflow file walk needed |
| `Archives/edgar/full-index/YYYY/QTRN/master.idx` | Cross-issuer historical sweep | Single download = all filings of a quarter | TSV needs parsing; stops at quarter granularity |
| `Archives/edgar/daily-index/YYYY/QTRN/master.YYYYMMDD.idx` | Catching today's tape | Hourly-ish freshness during business day | Many files (one per business day) |
| `dera/data/financial-statement-data-sets/{YYYY}q{N}.zip` | XBRL fundamentals at scale | Pre-flattened CSVs (sub/num/tag/pre); ~quarterly; one zip | Quarterly cadence only — won't catch this-quarter filings until next release |
| `Archives/edgar/Feed/YYYYMMDD.nc.tar.gz` | Same-day full-fidelity drop | Whole-day .nc archives complete with headers | Big files; need .tar.gz extraction; per-day |
| `Archives/edgar/Oldloads/` | Historical (~pre-2002) replay | Day-concatenated archives | Older format conventions, less consistent |
| `data.sec.gov/api/xbrl/frames/...` | Cross-sectional snapshot of one concept at one period | Comparable peer slices | Single concept × period only |

(sources: https://www.sec.gov/search-filings/edgar-application-programming-interfaces ; https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets ; https://www.sec.gov/Archives/edgar/full-index/ ; https://www.sec.gov/Archives/edgar/Feed/ ; https://www.sec.gov/Archives/edgar/Oldloads/)

### 2.2 Recommended backfill recipe for ats-eqt

1. **Fundamentals (10-K/10-Q/8-K XBRL facts), historical (1995→present):**
   - Step 1: download every `dera/.../{YYYY}q{N}.zip` from earliest available through latest. Each zip ~30–80 MB. ~120 quarterly zips for 30 years. **Total HTTP calls ≈ 120**, vs. millions if you walked each filing. (source: https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets)
   - Step 2: load `sub.txt`/`num.txt`/`tag.txt`/`pre.txt` into Polars. This is the spine. It's already pre-flattened; no XBRL parsing needed for the fact table.
   - Step 3: for the *current* (incomplete) quarter not yet in DERA, fall back to per-CIK companyfacts.json scrapes for the universe you care about.
   - Step 4: incremental — re-pull `companyfacts.json` for issuers with fresh filings detected via `submissions/CIK*.json` (which is updated within minutes of acceptance per SEC API docs). (source: https://www.sec.gov/search-filings/edgar-application-programming-interfaces)

2. **13F holdings, historical:**
   - DERA also publishes the **Form 13F Data Sets** quarterly (`/data-research/sec-markets-data/form-13f-data-sets`). **As of March 2024 the cadence shifted to running for the prior three months following the end of February, May, August, November.** Use these for bulk historical. (source: https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets)
   - For current-quarter / not-yet-bulked: full-index iteration filtering for `13F-HR`, `13F-HR/A`, `13F-NT`, `13F-NT/A`, then fetch each accession's `infotable.xml` (or whatever the manager named it — see §4).

3. **Universe enumeration:** start from `https://www.sec.gov/files/company_tickers.json` (CIK→ticker→title) and `https://www.sec.gov/files/company_tickers_exchange.json` (adds exchange), both small JSON, stable URLs. (source: https://www.sec.gov/about/developer-resources)

### 2.3 Is there a faster "everything filed in this range" endpoint?

No officially. The best approximations are:

- `Archives/edgar/Feed/YYYYMMDD.nc.tar.gz` — whole-day archives. Schema: each file inside is the original `.nc` (NC = "named container", the EDGAR submission package), tar+gzipped per day. This is the closest thing to a firehose dump. (source: https://www.sec.gov/Archives/edgar/Feed/)
- Full-index `master.idx` per quarter then expand inline.
- Third-party (`sec-api.io` etc.) wrap this for a fee — not a candidate for an open competitor.

Retention of `Feed/` and `Oldloads/`: search results don't state an explicit retention window — `[unverified]`, but community usage indicates Feed goes back to at least ~2001 and Oldloads covers pre-2001 historical. Confirm before relying on it for very old years. (source: https://www.sec.gov/Archives/edgar/Feed/ ; https://www.sec.gov/Archives/edgar/Oldloads/)

---

## 3. Submissions endpoint mechanics

**URL:** `https://data.sec.gov/submissions/CIK{cik_zero_padded_to_10}.json` — e.g. `CIK0000320193.json` for Apple. (source: https://www.sec.gov/search-filings/edgar-application-programming-interfaces)

**Response shape:**
```
{
  "cik": "320193",
  "name": "Apple Inc.",
  "tickers": [...],
  "exchanges": [...],
  "sic": "...",
  "filings": {
    "recent": {                       // columnar arrays, parallel-indexed
      "accessionNumber": [...],
      "filingDate": [...],
      "reportDate": [...],
      "form": [...],
      "primaryDocument": [...],
      "primaryDocDescription": [...],
      ...
    },
    "files": [                        // overflow pointers
      {
        "name": "CIK0000320193-submissions-001.json",
        "filingCount": 1000,
        "filingFrom": "YYYY-MM-DD",
        "filingTo":   "YYYY-MM-DD"
      },
      ...
    ]
  }
}
```
(source: https://sec-edgar-api.readthedocs.io/ ; https://tldrfiling.com/blog/sec-edgar-api-guide/)

**Pagination semantics:**
- `recent` holds **at least one year OR up to ~1,000 most recent filings, whichever is more** — the SEC docs are slightly ambiguous; community libraries (`sec-edgar-api`) treat 1,000 as the soft ceiling. (source: https://sec-edgar-api.readthedocs.io/)
- `files[]` lists overflow JSONs. **Each overflow file is fetched by appending its `name` to `https://data.sec.gov/submissions/`**, e.g. `https://data.sec.gov/submissions/CIK0000320193-submissions-001.json`. (source: https://sec-edgar-api.readthedocs.io/)
- Overflow filename convention verified: `CIK{10-digit-padded}-submissions-{NNN}.json` with `NNN` zero-padded 3-digit sequence. (source: https://sec-edgar-api.readthedocs.io/)
- Each overflow file has the same columnar schema as `recent` (no nested `files[]` again).
- **Number of overflow files for big filers:** Berkshire (CIK 1067983), Vanguard family CIKs, BlackRock, etc. typically have 1–5 overflow JSONs depending on filing volume × tenure. We don't have an exact count from cited sources — `[unverified]` for specific issuers; build the loader to walk to exhaustion regardless.
- **Renumbering:** the cited materials don't address whether SEC ever renumbers / rewrites overflow files. Community libraries assume content can shift; treat overflow JSON URLs as **freshness-mutable** (re-fetch periodically) but the underlying filings as immutable. `[unverified]` on the SEC's exact contract here.

**Cadence:** `submissions/CIK*.json` updates within minutes of EDGAR accepting a new filing — fast enough for "did issuer X just file?" polling at low frequency. (source: https://www.sec.gov/search-filings/edgar-application-programming-interfaces)

---

## 4. Filing-archive URL conventions

### 4.1 Accession number format

Accession number canonical form: `{filer_cik_10}-{YY}-{seq6}` — e.g. `0000320193-25-000123`. (source: https://www.sec.gov/submit-filings/filer-support-resources/edgar-glossary)

In **directory paths** the dashes are stripped, yielding an 18-digit run: `000032019325000123`. (source: https://www.sec.gov/info/edgar/pdsdissemspec910.pdf)

### 4.2 Per-filing directory

```
https://www.sec.gov/Archives/edgar/data/{cik_int_no_pad}/{accession_no_dashes}/
```
- The CIK in the **path** is the integer form (leading zeros stripped) — note this differs from the JSON API which uses zero-padded.
- The accession in the path has dashes removed.

(source: https://www.sec.gov/Archives/edgar/data/1067983/000119312525282901/0001193125-25-282901-index.html)

### 4.3 The per-accession `index.json`

Every accession folder publishes `index.json` (also `index.html` and `index.xml` for the same content):

```
{
  "directory": {
    "name": "/Archives/edgar/data/{cik}/{acc}",
    "parent-dir": "...",
    "item": [
      { "name": "primary_doc.xml", "type": "text.xml", "size": "12345", "last-modified": "..." },
      { "name": "infotable.xml",   "type": "text.xml", "size": "...",   "last-modified": "..." },
      ...
    ]
  }
}
```
(source: https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data)

This gives us a per-filing manifest without HTML parsing. **Always prefer `index.json` over scraping `index.htm`.**

### 4.4 Primary document determination

- **13F-HR / 13F-NT:** primary doc is `primary_doc.xml` (cover-page fields). The **information table** is a *separate XML attachment* whose name varies (`infotable.xml` is conventional but not guaranteed — examples include `4Q2011_13F_HR.XML`, `june30.xml`). Use `index.json` to locate it: the file with `type` indicating XML and `name != primary_doc.xml` is normally the info table; or use the sequence/description from the primary doc / EDGAR header. (source: https://www.sec.gov/Archives/edgar/data/1143565/000114356515000012/0001143565-15-000012-index-headers.html ; https://www.sec.gov/Archives/edgar/data/1140334/000114033415000003/0001140334-15-000003-index-headers.html)
- **10-K/10-Q (inline XBRL):** primary doc is the `*.htm` filing itself with iXBRL embedded; no separate `instance.xml` since iXBRL mandate. Companion files: schema (`*-{date}.xsd`), label/calc/def linkbases (`*-{date}_lab.xml` etc.), and a financial reports `*_FilingSummary.xml`. (source: https://www.sec.gov/files/edgar/filer-information/specifications/xbrl-guide.pdf)
- **8-K:** primary doc is the `*.htm` named by the filer; iXBRL only for cover-page since 2021.
- **Authoritative pointer:** the `submissions/CIK*.json` row's `primaryDocument` field gives the filename for that accession.

---

## 5. Caching, ETags, immutability semantics

**Filings are immutable once accepted.** The SEC publishes amendments as **new accession numbers** (`/A` suffix on form types: `10-K/A`, `13F-HR/A`). The original URL never gets rewritten.

This means for `Archives/edgar/data/...` payloads our cache key is just **the URL**, with a permanent TTL, plus a content sha256 for integrity. We do **not** need ETag round-trips for archive bytes — the immutability contract is stronger than ETag would give us.

**ETag / If-Modified-Since support:** SEC's static archive servers (CloudFront-fronted) do return `ETag` and `Last-Modified` headers and honor `If-None-Match` / `If-Modified-Since` (returning `304 Not Modified` when matching). This is useful for the **dynamic** endpoints whose content can change:
- `submissions/CIK*.json` (changes whenever issuer files)
- `companyfacts/CIK*.json` (changes within hours of new filing)
- `frames/.../*.json` (changes when filings backfill into the period)
- `full-index/.../master.idx` for the **current quarter** (changes daily)

Treat these as `(URL, last_etag) → (body, fetched_at, expiry_hint)`. Use `If-None-Match` on refetch; on 304, bump `fetched_at`. (source: https://deepwiki.com/dgunning/edgartools/7.3-http-client-and-caching)

**edgartools' specific cache rules (a useful model):**
- `/submissions/...` → 10 minutes
- `/...index/...` → 30 minutes
- `/Archives/edgar/data/...` → forever

(source: https://deepwiki.com/dgunning/edgartools/7.3-http-client-and-caching)

We should adopt the same shape, with our own TTLs.

**Recommended cache key for ats-eqt:** `(url_path, content_sha256)` for archived bytes; `(url, etag, fetched_at)` for dynamic JSON. Store on disk with content-addressed naming so dedup is automatic across CIKs/accessions.

---

## 6. Community-library pattern survey

### 6.1 edgartools (https://github.com/dgunning/edgartools)

- **HTTP:** `httpxthrottlecache` wrapping `httpx`; global `HTTP_MGR` instance.
- **Rate limit:** `pyrate-limiter` token bucket, default 9 req/s, configurable up to 50 for authorized mirrors.
- **Caching:** `hishel`-backed `FileCache` under `~/.edgar/`; tiered TTLs by URL family.
- **Retry:** exponential backoff `1s → 2s → 4s → 8s → 16s` with jitter, up to 8 attempts.
- **Concurrency:** sync + async clients; relies on `httpx` connection pooling, no explicit concurrency primitive surfaced (token bucket gates everything).
- **Strengths:** strong typed-object model for 20+ form types; zero-infra; MIT licensed.
- **Weaknesses:** README under-documents the concurrency story; large monolithic API surface; entangles fetch + parse + analytics.
- **Pattern to copy:** the cache-rules-by-URL-family approach. The 9 req/s default. The `httpxthrottlecache` decomposition (we should keep transport / cache / limiter as composable layers).
- **Pitfall to avoid:** monolithic config object; entangled async + sync. We want a thin `EdgarClient` with optional cache decorator and limiter decorator.

(sources: https://github.com/dgunning/edgartools ; https://deepwiki.com/dgunning/edgartools/8.1-http-client-and-rate-limiting ; https://deepwiki.com/dgunning/edgartools/7.3-http-client-and-caching)

### 6.2 sec-edgar-downloader (https://github.com/jadchaar/sec-edgar-downloader)

- **HTTP:** `requests` (switched from `httpx` to enable retry-with-backoff). Synchronous.
- **Rate limit:** `pyrate-limiter`; ≤10 req/s; max queue delay 60s before raising.
- **Retry:** up to **10 retries** with exponential backoff, mainly to dodge intermittent 403s.
- **Headers:** randomized User-Agent **per request** (not per session), `Accept-Encoding: gzip, deflate`, `Host: www.sec.gov`.
- **Storage:** `sec-edgar-filings/{ticker_or_cik}/{form_type}/{accession}/...` on disk.
- **Strengths:** very battle-tested; clean retry; simple storage layout.
- **Weaknesses:** sync-only; per-request UA randomization is unusual and arguably violates the spirit of fair-access (SEC wants stable identification).
- **Pattern to copy:** retry budget and backoff curve. Storage layout shape.
- **Pitfall to avoid:** randomizing UA per request — for ats-eqt, a stable, project-identifying UA is correct.

(source: https://github.com/jadchaar/sec-edgar-downloader)

### 6.3 sec-edgar-api (https://github.com/jadchaar/sec-edgar-api)

- Thin wrapper around the four `data.sec.gov` JSON endpoints (submissions, companyconcept, companyfacts, frames).
- Auto-handles overflow pagination with `handle_pagination=True` (concatenates `recent` + each `files[]` overflow into one logical view). This is the cleanest pagination handling pattern in the ecosystem — adopt the shape.
- License: not explicit in fetched content (`[unverified]`, badges suggest MIT).

(source: https://sec-edgar-api.readthedocs.io/ ; https://github.com/jadchaar/sec-edgar-api)

### 6.4 python-edgar / py-edgar (joeyism)

- Smaller community; rate-limit handling is mostly issue-tracker-driven (see issue #24 in `joeyism/py-edgar`). Don't model on it. (source: https://github.com/joeyism/py-edgar/issues/24)

### 6.5 sec-edgar/sec-edgar (https://github.com/sec-edgar/sec-edgar)

- Pulls "all companies, periodic reports, filings and forms". Less actively maintained than edgartools. Useful as a reference for full-index iteration patterns. (source: https://github.com/sec-edgar/sec-edgar)

### 6.6 Arelle (https://github.com/Arelle/Arelle, https://arelle.org/)

- The canonical XBRL processor; SEC's reference implementation runs the EDGAR Renderer plugin atop Arelle.
- Apache-2 licensed.
- Python API exists but is heavyweight — most usage is via CLI / web service.
- Provides full base-spec, dimensions, formula, calculation, and EFM (Edgar Filing Manual) validation.
- **Pattern to copy:** subprocess Arelle for canonical validation passes (annual / pre-publish), don't import it into hot paths.
- **Modern wrapper:** `arelle-mcp` on PyPI offers a more ergonomic interface.

(sources: https://github.com/Arelle/Arelle ; https://github.com/Arelle/EDGAR ; https://github.com/Arelle/EdgarRenderer ; https://pypi.org/project/arelle-mcp/1.0.1/)

### 6.7 secfsdstools (https://pypi.org/project/secfsdstools/, https://github.com/HansjoergW/sec-fincancial-statement-data-set)

- Helper toolkit specifically for the DERA Financial Statement Data Sets — exactly the spine we want. Worth reading their CSV-loading and joining patterns even if we re-implement in Polars.

### 6.8 edgar-crawler (https://github.com/lefterisloukas/edgar-crawler)

- Open-source toolkit that downloads filings and extracts item-section text into structured JSON. Presented at WWW 2025. Less relevant for fundamentals/13F but a decent reference for textual extraction.

---

## 7. XBRL specifics

### 7.1 iXBRL mandate state

Inline XBRL (HTML with embedded XBRL) is now mandatory for substantively all financial statements:
- **2019-06-15:** large accelerated filers
- **2020-06-15:** accelerated filers
- **2021-06-15:** all other filers (incl. foreign private issuers using IFRS)

**Fee data** tagging mandate: all filers since **2025-07-31**. Starting **2026-03-16** (already in effect now), fee-data XBRL errors **suspend the filing** until corrected — so you'll see slightly fewer half-tagged filings than you would have a year ago. (source: https://www.sec.gov/data-research/structured-data/inline-xbrl ; https://www.toppanmerrill.com/blog/sec-fee-data-issues-will-now-trigger-filing-suspensions/)

EDGAR Release **26.1** (deployed 2026-03-16) added support for the 2026 taxonomies. (source: https://www.sec.gov/newsroom/whats-new/2603-2026-xbrl-taxonomies-update)

**Implication:** since 2021 we don't get separate `instance.xml` files for most filings — the iXBRL is embedded in the primary `.htm`. Use `arelle` or `python-xbrl` to extract, or — better — let DERA / companyfacts.json do it for us.

### 7.2 companyfacts.json — coverage and lag

`https://data.sec.gov/api/xbrl/companyfacts/CIK{padded}.json` returns **every us-gaap and dei concept ever tagged for that CIK, across all filings**. Single round-trip per issuer. (source: https://www.sec.gov/search-filings/edgar-application-programming-interfaces)

Coverage limits:
- Only XBRL-tagged facts. If a filer doesn't tag a concept (rare for major statements but happens for footnotes), it isn't there.
- Lag: hours-to-a-day after filing acceptance. Not real-time.
- Coverage is XBRL only — no narrative, no MD&A, no exhibits.

### 7.3 frames endpoint — usefulness for backfill

`https://data.sec.gov/api/xbrl/frames/{taxonomy}/{tag}/{unit}/{period}.json` returns **one fact per reporting entity** for that concept × period. Period formats:
- `CY2024` — annual (duration 365±30 days)
- `CY2024Q1` — quarterly (duration 91±30 days)
- `CY2024Q1I` — instantaneous (point-in-time)

Useful for sector snapshots (e.g. "give me Cash on Q4 2024 for everyone"); **less useful** for issuer-by-issuer backfill since you'd need (concept × period) round-trips. Use companyfacts as the per-issuer spine. (source: https://www.sec.gov/search-filings/edgar-application-programming-interfaces)

### 7.4 Calculation-linkbase validation strictness

Calculation linkbases assert that line items sum to subtotals (e.g. assets = current_assets + non-current_assets). Strict validation catches filer errors but **rejects ~5–10% of filings due to known historical inconsistencies** `[unverified estimate]`. Recommendation: log calc-linkbase failures, do not hard-reject. Arelle in EFM mode will surface them as warnings/errors; configurable.

---

## 8. 13F-specific gotchas (re-confirmation)

- **Unit cutover 2023-01-03**: still applies. Pre-2023 a single share lot was reported in lot-sized units; post-2023 in actual share units (or vice-versa per security). Already covered in `research/datasets/13f_holdings.md`. No further changes per cited sources in 2026. `[unverified — confirm against your existing notes]`
- **13F-CTR (confidential treatment requests)**: mandatory electronic filing on EDGAR since **2023-02-28**. Form types `13F-CTR` and `13F-CTR/A`. Detect by filtering `form` field in submissions JSON. Holdings inside a CTR are redacted from the public 13F-HR until the CTR window expires (typically 1 year extendable). On expiration, manager files a 13F-HR/A revealing the previously-confidential positions — track these explicitly. (source: https://www.sec.gov/investment/divisionsinvestmentguidance13fpt2htm ; https://www.sidley.com/en/insights/newsupdates/2022/07/sec-adopts-rules-requiring-electronic-filing-for-form-13f-confidential-treatment-requests)
- **Section 13(f) securities list**: PDF only at `https://www.sec.gov/files/investment/13flist{YYYY}q{N}.pdf` — there is **no machine-readable XML/CSV version officially**. We need to parse the PDF (each entry is CUSIP + issuer name + class). Q1 2026 list confirmed at `https://www.sec.gov/files/investment/13flist2026q1.pdf`. (source: https://www.sec.gov/rules-regulations/staff-guidance/official-list-section-13f-securities)
- **DERA Form 13F Data Sets cadence change**: as of March 2024, the data sets are run for the prior three months following the end of February, May, August, November (so a slight lag relative to the 45-day filing deadline). Plan ingestion accordingly. (source: https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets)
- **Information-table filename is not standard**: the SEC technical spec describes the *content* of the info table XML but does not mandate its filename within the accession. `infotable.xml` is the most common, but real-world examples include `4Q2011_13F_HR.XML`, `june30.xml`, etc. Always discover via `index.json`, never hardcode. (source: https://www.sec.gov/info/edgar/specifications/form13fxmltechspec.htm)

---

## 9. Failure modes + retry policy

| Status | Meaning | Recommended action |
|---|---|---|
| `200` | OK | proceed |
| `301/302` | redirect (rare; mostly between sec.gov/www.sec.gov) | follow ≤3 redirects |
| `304` | Not Modified (when sending If-None-Match) | use cached body |
| `403` | IP-block or missing/bad UA | **fail-fast on UA-bad; back off ~10 minutes on rate-block.** Don't hammer. |
| `404` | filing doesn't exist or path typo | do not retry; mark missing |
| `429` | too many requests | honor `Retry-After` if present; else exponential backoff from 5s |
| `5xx` | server-side | exponential backoff + jitter, ≤8 retries |

(source: https://blog.finxter.com/solving-response-403-http-forbidden-error-scraping-sec-edgar/ ; https://github.com/jadchaar/sec-edgar-downloader)

**Backoff curve (recommended):** `min(2^n + jitter, 60s)` for n = 0..7, capped at 8 retries. (`edgartools` uses `1, 2, 4, 8, 16` with jitter; `sec-edgar-downloader` allows up to 10. We pick 8 as the sweet spot.)

**Concurrent-connection limits per IP:** SEC doesn't publish a hard concurrency cap; the 10 req/s cap dominates. In practice, holding 2–4 concurrent HTTP/2 multiplexed connections to `www.sec.gov` (CloudFront edge) and 1–2 to `data.sec.gov` is comfortable. Above that you're statistically more likely to trip 403s. `[unverified — empirical]`

**`Retry-After` header reliability:** present on most 429s and some 503s; respect it absolutely when present.

**Detection of soft block (no error code):** symptom is sustained latency spikes / connection resets despite below-cap traffic. Mitigation: circuit-break and pause for 60s on 3 consecutive timeouts.

---

## 10. Storage layout for raw filings on disk

### 10.1 Recommended filesystem layout

```
{ats_eqt_data_root}/edgar/
  raw/
    archives/
      {cik:int}/{accession_no_dashes}/
        index.json              # cached SEC index
        primary_doc.xml         # for 13F
        infotable.xml           # for 13F (filename varies — see manifest)
        {primary}.htm           # for 10-K/10-Q (iXBRL)
        {primary}.htm.gz        # if compressed
        ...
    submissions/
      CIK{padded}.json
      CIK{padded}-submissions-001.json
      ...
    companyfacts/
      CIK{padded}.json
    dera/
      financial-statements/
        {YYYY}q{N}.zip          # keep zip; lazy-extract on use
      form-13f/
        {YYYY}q{N}/...
    full-index/
      {YYYY}/{QTRN}/master.idx.gz
    feed/
      {YYYY}/{YYYYMMDD}.nc.tar.gz
  manifests/
    {date}.parquet               # daily fetch manifest (URL, sha256, fetched_at, etag, status)
  cache/
    by-sha256/
      {sha256[:2]}/{sha256}      # content-addressed dedup
```

(rationale derived from `sec-edgar-downloader` storage shape and edgartools' `~/.edgar/` layout — sources: https://github.com/jadchaar/sec-edgar-downloader ; https://edgartools.readthedocs.io/en/latest/guides/local-storage/)

### 10.2 Compression policy

- **Keep DERA / Feed / Oldloads as their original .zip / .tar.gz** — they're already gzip'd; re-encoding wastes CPU.
- **Compress raw archive files (primary `.htm`, `.xml`) on-disk with gzip level 6.** Typical 10-K is 5–20 MB uncompressed, 1–4 MB compressed. Random-access not required for raw blobs (we re-parse start-to-end). Save ~70–80% disk.
- **Do not compress index.json / submissions.json** — small (< 500 KB usually) and we re-read often.
- **Parquet for derived/parsed data** (Polars-native, columnar, zstd compression).

### 10.3 Manifest format

No community standard exists; each library invents its own. Recommend:

```
manifests/{date}.parquet schema:
  url               string
  cik               int64        (nullable for non-archive URLs)
  accession         string       (nullable)
  filename          string       (nullable, for archive items)
  sha256            string
  bytes             int64
  etag              string
  last_modified     string
  fetched_at        timestamp[ns]
  status_code       int32
  retry_count       int32
  source            string       # "submissions" | "companyfacts" | "archive" | "dera" | "full-index" | "feed"
```

Append-only daily files; weekly compaction; partition by `source` for fast filter.

---

## 11. Recommendations for ats-eqt's `edgar/` module

### 11.1 What to implement first (P0, milestone 1)

1. **`EdgarClient` (httpx-based, async-first):**
   - Stable User-Agent built from project config (`f"ats-eqt {contact_email}"`).
   - Connection pool to `www.sec.gov` + `data.sec.gov` separately.
   - Single global token-bucket limiter (default 5 req/s, configurable to 9).
   - Retry decorator: 8 attempts, exponential backoff with jitter, 403/429/5xx aware, honor `Retry-After`.
   - Conditional-GET support: `If-None-Match` + `If-Modified-Since`.
2. **`SubmissionsLoader.fetch(cik) -> FilingsTable`**
   - Resolves CIK → padded.
   - GETs `submissions/CIK{padded}.json`.
   - Walks every `files[]` overflow JSON.
   - Returns Polars DataFrame with all filings, normalized columns.
3. **`CompanyFactsLoader.fetch(cik) -> ConceptFactsTable`**
   - GETs `companyfacts/CIK{padded}.json`.
   - Pivots into long Polars frame `[cik, taxonomy, concept, unit, period_start, period_end, value, accn, form, filed]`.
4. **`IndexJsonLoader.fetch(cik, accession) -> Manifest`**
   - GETs `Archives/edgar/data/{cik}/{accn-no-dashes}/index.json`.
   - Returns list of items.
5. **Disk cache layer** — content-addressed under `cache/by-sha256/`, with manifest Parquet append.

### 11.2 P1, milestone 2

6. **`DeraFinancialStatementsLoader`** — downloads `{YYYY}q{N}.zip`, lazy-loads each CSV with Polars `scan_csv`, joins `sub`+`num`+`tag`+`pre`. This is the fundamentals spine.
7. **`Form13FLoader`** — given an accession, fetch primary_doc.xml + info-table XML (auto-discovered via index.json); parse into Polars frame `[manager_cik, period, cusip, issuer, class, value, shares, sole/shared/none, putcall]`.
8. **`FullIndexLoader`** — `master.idx` per-quarter parser, returns iterator of accession refs.
9. **`Section13FSecuritiesListLoader`** — PDF parser for the quarterly list (use `pdfplumber`).

### 11.3 P2, milestone 3

10. **`FeedLoader`** — daily `.nc.tar.gz` ingest, untar streaming.
11. **Arelle subprocess wrapper** for canonical XBRL validation.
12. **`FramesLoader`** — sector-snapshot lookups.
13. **Polling daemon** — re-fetch `submissions/CIK*.json` for tracked CIKs every N minutes; emit "new filing" events into the rest of ats-eqt.

### 11.4 Implementation principles

- **Async-first, but sync wrappers everywhere.** Polars + downstream code is mostly sync.
- **One global limiter, no per-loader limiter.** The cap is per-IP, not per-loader.
- **No retries inside the limiter.** Limiter delays; retry is a separate decorator on top.
- **Cache invalidation via TTL + ETag, never via mutation detection.** SEC contract is strong enough.
- **Never hardcode filenames inside an accession folder.** Always discover via `index.json`.
- **Raise on bad UA, don't auto-fix.** Force callers to configure a real contact email — refuse to fetch otherwise.
- **Treat 403 as fatal-for-this-IP for ~10 minutes.** Trip a circuit breaker; do not silently retry.

---

## 12. Sources

1. https://www.sec.gov/about/developer-resources
2. https://www.sec.gov/search-filings/edgar-search-assistance/accessing-edgar-data
3. https://www.sec.gov/os/accessing-edgar-data
4. https://www.sec.gov/filergroup/announcements-old/new-rate-control-limits
5. https://www.novaworkssoftware.com/blog/archives/781-SEC-Applies-New-Rate-Control-Limits-to-EDGAR-Websites.html
6. https://dealcharts.org/blog/edgar-scraping-rate-limits-explained
7. https://www.sec.gov/search-filings/edgar-application-programming-interfaces
8. https://data.sec.gov/
9. https://sec-edgar-api.readthedocs.io/
10. https://github.com/jadchaar/sec-edgar-api
11. https://github.com/jadchaar/sec-edgar-downloader
12. https://github.com/dgunning/edgartools
13. https://edgartools.readthedocs.io/en/stable/configuration/
14. https://edgartools.readthedocs.io/en/latest/resources/performance/
15. https://edgartools.readthedocs.io/en/latest/guides/local-storage/
16. https://deepwiki.com/dgunning/edgartools/7.3-http-client-and-caching
17. https://deepwiki.com/dgunning/edgartools/8.1-http-client-and-rate-limiting
18. https://www.sec.gov/Archives/edgar/full-index/
19. https://www.sec.gov/Archives/edgar/daily-index/
20. https://www.sec.gov/Archives/edgar/Feed/
21. https://www.sec.gov/Archives/edgar/Oldloads/
22. https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets
23. https://www.sec.gov/data-research/sec-markets-data/financial-statement-notes-data-sets
24. https://www.sec.gov/files/dera/data/financial-statement-data-sets/2025q1.zip
25. https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets
26. https://www.sec.gov/files/form_13f.pdf
27. https://www.sec.gov/info/edgar/specifications/form13fxmltechspec.htm
28. https://www.sec.gov/edgar/filer-information/specifications/form13fxmltechspec-draft
29. https://www.sec.gov/files/investment/13flist2026q1.pdf
30. https://www.sec.gov/rules-regulations/staff-guidance/official-list-section-13f-securities
31. https://www.sec.gov/rules-regulations/staff-guidance/division-investment-management-frequently-asked-questions/frequently-asked-questions-about-form-13f
32. https://www.sec.gov/investment/divisionsinvestmentguidance13fpt2htm
33. https://www.sec.gov/files/rules/final/2022/34-95148.pdf
34. https://www.sec.gov/data-research/structured-data/inline-xbrl
35. https://www.sec.gov/files/edgar/filer-information/specifications/xbrl-guide.pdf
36. https://www.sec.gov/newsroom/whats-new/2603-2026-xbrl-taxonomies-update
37. https://www.toppanmerrill.com/blog/sec-fee-data-issues-will-now-trigger-filing-suspensions/
38. https://arelle.org/arelle/
39. https://github.com/Arelle/Arelle
40. https://github.com/Arelle/EDGAR
41. https://github.com/Arelle/EdgarRenderer
42. https://pypi.org/project/arelle-mcp/1.0.1/
43. https://www.sec.gov/info/edgar/pdsdissemspec910.pdf
44. https://www.sec.gov/files/edgar/pds_dissemination_spec.pdf
45. https://www.sec.gov/submit-filings/filer-support-resources/edgar-glossary
46. https://www.sec.gov/Archives/edgar/data/1067983/000119312525282901/0001193125-25-282901-index.html
47. https://www.sec.gov/Archives/edgar/data/1143565/000114356515000012/0001143565-15-000012-index-headers.html
48. https://www.sec.gov/Archives/edgar/data/1140334/000114033415000003/0001140334-15-000003-index-headers.html
49. https://blog.finxter.com/solving-response-403-http-forbidden-error-scraping-sec-edgar/
50. https://www.sidley.com/en/insights/newsupdates/2022/07/sec-adopts-rules-requiring-electronic-filing-for-form-13f-confidential-treatment-requests
51. https://github.com/HansjoergW/sec-fincancial-statement-data-set
52. https://pypi.org/project/secfsdstools/
53. https://github.com/lefterisloukas/edgar-crawler
54. https://github.com/sec-edgar/sec-edgar
55. https://tldrfiling.com/blog/sec-edgar-api-guide/
56. https://pypi.org/project/pyrate-limiter/
57. https://aiolimiter.readthedocs.io/

---

*End of research note. Next step: scaffold `src/ats_eqt/edgar/client.py` with the `EdgarClient` design from §11.1.*

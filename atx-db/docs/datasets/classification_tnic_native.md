# Native reproduced TNIC-style classification

Implements the original reproduced method under R24, distinct from the authors'
official TNIC data. No official import or source license admission is claimed.
Products in immutable generation `classification_tnic/`: `pairs.parquet`
keys `(year,cik,peer_cik)`; `firms.parquet` keys `(year,cik)`; `years.parquet`
keys `(year)`. Source years2018calibration and2019..2026output preserve full
linked original10K-filer denominators. No nearest-five fallback contributes.

Vocabulary follows pinned `tnic.vocabulary_words`, stop/geographic words;
exact strings replace hash collisions. Binary cosine; prior-year document
frequency removes words present in strictly>25%distinct accessions; at least
50distinct surviving words;20000histogram bins; strictly score>prior-year
SIC3-calibrated threshold. Co-registrants of one accession are not peers.
Latest original filing selection is known only at the independently authenticated
full original SEC census snapshot, never backdated to early acceptance. Fitting
uses the full original SEC annual cohort rather than issuers selected by future
membership. Complete original quarterly SEC master indexes must exactly replay
all native accession/form/date/owner records; their observed source clocks are
dependencies. Every generation is immutable; text inventory retains all vintages.

Historical SIC sources must be original SEC submissions JSON, authenticated
by independently pinned receipt URL/body hashes/bytes and explicit observed
UTC timestamp. Select the latest source observation strictly before next
calendar year. A2026snapshot cannot qualify historical calibration. Missing
SIC or unknown source clocks leave calibration missing, with reasons.
DF fitting first excludes raw vocabularies below the original50word minimum;
short-document inventory/provenance and publication denominators remain intact.
Each encoded histogram retains the prior raw DF corpus qualification, provenance
and latest clock. Subsequent calibration cannot forget a missing/late filter
dependency. Complete current/previous raw cohorts and prior encoded histogram
lineage participate in availability and source digest; year labels are not clocks.

| Columns | Type/unit | Definition/source/clock |
|---|---|---|
| year | INTEGER year | Calendar filing-year cohort, not official fiscal-year TNIC |
| cik, peer_cik | BIGINT | Native linked original issuer IDs |
| accession, peer_accession | string | Both original annual filings |
| score, threshold | DOUBLE fraction | Raw binary cosine and prior-year calibrated cut; not official excess score |
| available_at | TIMESTAMPTZ UTC | Max both filings, every current/prior vocabulary/calibration input, qualified historical SIC observation and original full-index census snapshot clocks |
| peer_basis | string | reproduced_tnic3_df25_sic3 |
| corpus_sha256 | string | Full current/prior corpus provenance digest including raw/filtered vocab and SIC vintages |
| n_peers, reason | BIGINT count/string | Strict peers; missing fit remains NULL with reason; zero only for proved complete qualified pair population |
| accession, filing_date, filing_available_at, vocabulary_words | string/DATE/TIMESTAMPTZ/BIGINT | Latest original census snapshot filing, own acceptance and exact surviving vocabulary |
| n_peers_own_clock, n_fallback_peers, total_similarity, best_score | BIGINT/BIGINT/DOUBLE/DOUBLE | Strict visible-at-own-clock count, separately labeled nearest5 fallback count, strict score sum, best scored neighbor; unknown fit remains NULL |
| calibrated_on, docs, calibration_reason | INTEGER/BIGINT/string | Prior year, full corpus and missing-fit explanation |

Peer pairs are streamed to disk, no full pair matrix. The reader completely
replays corpus fitting and immutable outputs from original source bytes.
Use `<22:00UTC preceding session` for daily visibility; at most550day TNIC
staleness. Original gate: strict peers for>=95%full linked10Kfilers per year2019+.
Actual measured coverage: **unmeasured/FAIL**. Offline strict success fixture:
8/8filers2019,8directed strict pairs, availability2020-01-01UTC; future SIC fixture
publishes zero qualified pairs. These are synthetic checks, not actual gates.
Nearest5 rows are emitted only with qualified complete source/corpus clocks,
explicit `nearest5_fallback` basis and the entire selection clock. They never
contribute to the95%strict-peer gate. Offline fallback-only fixture proves this.

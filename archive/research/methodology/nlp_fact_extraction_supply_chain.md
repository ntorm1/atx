# NLP Fact Extraction for Fundamentals and Supply Chain Data

**Date:** 2026-05-17
**Target:** ats-eqt
**Question:** How can ats-eqt extract fundamental facts, company relationships, and supply-chain signals from filings, news, press releases, and shipment records using classical NLP, entity recognition, and simple machine learning instead of expensive LLM calls?

## Executive takeaways

1. **Do not use NLP where structured data already exists.** Numeric fundamentals should come first from XBRL/Inline XBRL, SEC `companyfacts`, `companyconcept`, and raw filing XBRL. The SEC says Inline XBRL is both human-readable and machine-readable, and the EDGAR APIs expose company facts, concepts, frames, submissions, and nightly bulk ZIPs.

2. **Use NLP for the facts that are still trapped in prose.** The best first targets are major customers, supplier/customer/partner/competitor relationships, customer concentration, named products, segment/geography exposure, operational KPIs, facility/location facts, risk events, and supply-chain disruptions.

3. **Incumbents build with evidence, entity resolution, and analyst QA, not magic.** FactSet Revere-style relationship data is built from public company disclosures such as annual filings, investor presentations, and press releases. Bloomberg SPLC-style data is similarly described as coming from public filings, company reports, and transcripts, with proprietary algorithms/analysis layered on top. Shipment providers build from bills of lading, customs data, standardized parties, product text, ports, weights, and container fields.

4. **The moat is entity resolution and provenance.** A low-cost system can compete if every extracted fact has an evidence span, source URL/accession, filing date, confidence score, relation type, entity identifiers, and point-in-time validity.

5. **A classical pipeline is enough for a useful v1.** Regex, dictionaries, section parsers, spaCy matchers, CRF/linear NER, TF-IDF character n-gram entity matching, weak supervision, and simple relation classifiers can produce high-precision datasets. Exhaustive recall can improve later.

## How incumbents appear to build these datasets

### 1. Structured fundamentals: XBRL first

Fundamental data providers usually do not infer standard financial statement line items from prose if a structured source exists. SEC filings now include Inline XBRL for domestic 10-K/10-Q financial statement information, and the SEC describes Inline XBRL as a structured format that lets one filing be both readable by humans and machines.

The SEC EDGAR API surface is enough to build a strong open fundamentals backbone:

- `data.sec.gov/submissions/CIK##########.json`: filing history and metadata.
- `data.sec.gov/api/xbrl/companyfacts/CIK##########.json`: all company concepts in one JSON response.
- `data.sec.gov/api/xbrl/companyconcept/CIK##########/taxonomy/tag.json`: all disclosures for one company and concept.
- `data.sec.gov/api/xbrl/frames/...`: cross-company frame data for a period.
- Nightly bulk archives for company facts and submissions.

**ats-eqt implication:** Use XBRL for canonical financial statement values, and reserve NLP for untagged disclosures, narrative facts, relationship edges, and event metadata.

Sources: [SEC EDGAR APIs](https://www.sec.gov/search-filings/edgar-application-programming-interfaces), [SEC Inline XBRL](https://www.sec.gov/data-research/structured-data/inline-xbrl).

### 2. Relationship graphs from filings and company communications

FactSet Revere is the cleanest public model for a classical extraction target. The AWS Marketplace listing for FactSet Supply Chain Relationships says the feed exposes customers, suppliers, competitors, and strategic partners collected from annual filings, investor presentations, and press releases. It also distinguishes direct relationships disclosed by the reporting company from reverse relationships disclosed by another company.

Academic and library descriptions of FactSet/Bloomberg/Capital IQ supply-chain datasets are consistent with this model:

- Sources are public disclosures, regulatory filings, annual reports, investor presentations, press releases, company reports, transcripts, and news.
- Outputs are normalized relationship edges, usually with direction, type, source, and dates.
- Relationship types commonly include customer, supplier, distributor, partner, competitor, and sometimes subtypes.
- Larger vendors add analyst review and proprietary scoring/quantification.

Bloomberg says its supply-chain tools map relationships among more than 100,000 public and private companies with 500,000+ unique relationships and history back to 2006. A U.S. Department of Commerce library guide describes SPLC data as coming from public sources including SEC filings, company reports, and earnings-call transcripts, then using proprietary algorithms to estimate cost/revenue dependencies.

**ats-eqt implication:** A strong open competitor can start with source-backed edges rather than trying to estimate every dollar of dependency immediately. The v1 product should be "relationship + evidence + confidence"; quantified exposure can come later.

Sources: [FactSet Supply Chain Relationships on AWS](https://aws.amazon.com/marketplace/pp/prodview-h6mqbgeckx2gk), [Bloomberg Supply Chain](https://professional.bloomberg.com/solutions/corporations/supply-chain/), [Commerce Research Library SPLC guide](https://library.doc.gov/c.php?g=1257152&p=9211692).

### 3. Customer concentration disclosures

Major-customer disclosure is a high-value, high-precision extraction target. Public firms have accounting/disclosure requirements around major customers. The Federal Reserve notes that public firms disclose major customers accounting for at least 10 percent of total sales, and that this filing requirement feeds the customer segment data in Compustat. The Fed example also shows a practical non-LLM matching process: normalize reported customer names, run text matching, manually review ambiguous matches, and add geography from company/segment information.

Regulation S-K Item 101 also asks companies to describe revenue-generating activities and dependence on key products, services, product families, or customers.

**ats-eqt implication:** Start here. Customer concentration sentences are formulaic and often contain percentages, years, and repeated phrasing. This is the ideal "boring but valuable" extraction wedge.

Sources: [Federal Reserve FEDS Note on buyer-supplier data](https://www.federalreserve.gov/econres/notes/feds-notes/measuring-domestic-and-export-flows-in-buyer-supplier-data-20190626.html), [17 CFR 229.101](https://www.law.cornell.edu/cfr/text/17/229.101).

### 4. News analytics: entity, event, sentiment, relevance, novelty

News analytics vendors demonstrate the same architecture applied to faster text:

- LSEG News Analytics uses NLP to produce company sentiment, relevance, novelty, volume, headline classification, and event-style metadata on Reuters and third-party news, with archives back to 2003.
- RavenPack describes a large NLP infrastructure that turns unstructured text into structured analytics such as sentiment scores, with machine learning trained on a long curated archive.

The key product shape is not a prose summary. It is a stream of normalized records:

```text
{document_id, timestamp, entity_id, event_type, sentiment, relevance,
 novelty, evidence_headline_or_sentence, source, confidence}
```

**ats-eqt implication:** For a low-cost v1, build deterministic event detectors for common business events: guidance changes, customer wins/losses, supplier disruption, factory opening/closure, contract award, recall, lawsuit, strike, acquisition, product launch, bankruptcy, sanction/export-control action, and executive change.

Sources: [LSEG News Analytics](https://developers.lseg.com/en/product/news/news_analytics), [RavenPack Technology](https://www.ravenpack.com/technology).

### 5. Shipment and customs-data providers

Supply-chain data providers usually have two different products:

- **Disclosure relationship graph:** Extracted from filings, presentations, releases, transcripts, and company websites.
- **Shipment graph:** Built from bills of lading, customs declarations, trade data, carrier/manifests, and product descriptions.

Panjiva/S&P says its data includes bill-of-lading commodity descriptions, consignee, shipper, cargo weight, container information, product classifications, company information, corporate family, origin/destination countries, ports, and regions. Its API exposes global and country/direction-specific data sources such as `us-imports`, `us-exports`, Brazil, China, India, Mexico, Turkey, Vietnam, and others.

ImportYeti says U.S. sea-freight bill-of-lading data is public but expensive to obtain via FOIA, and says it requested 70 million BOLs. It also explains common missingness: companies import under alternate names, air/land freight is not public in the same way, and importers can request private treatment. Descartes Datamyne says U.S. maritime import records are added daily, 24 hours after receipt from CBP, and that the database supports search by shipper/consignee, commodity, ports, countries, equipment, HS code, and more.

CBP vessel-manifest rules explain the legal substrate: members of the public cannot directly examine vessel manifests, but may request information from CBP, and importers/consignees/shippers can request confidential treatment. The CFR also notes AMS manifest data availability and exclusion of confidential records.

**ats-eqt implication:** Treat shipment data as a separate ingestion family from filings NLP. The NLP work there is mostly entity normalization, address normalization, product-description classification, HS/HTS imputation, and edge aggregation. A bill-of-lading edge is evidence of a shipment relationship, not always proof of a strategic supplier relationship.

Sources: [Panjiva Supply Chain Intelligence](https://www.spglobal.com/market-intelligence/en/solutions/products/panjiva-supply-chain-intelligence), [Panjiva API data sources](https://panjiva.com/api-guide/data-selection/data-sources), [ImportYeti FAQ](https://www.importyeti.com/faqs), [Descartes Datamyne BOL database](https://www.datamyne.com/our-product/bill-of-lading-database/), [CBP vessel manifest confidentiality](https://www.cbp.gov/trade/automated/electronic-vessel-manifest-confidentiality), [19 CFR 103.31](https://www.law.cornell.edu/cfr/text/19/103.31).

## What to extract first

### Fundamental narrative facts

Prioritize facts that are:

- Repeated across many companies.
- Written in semi-standard language.
- Tied to a source document date.
- Useful as a tabular dataset or graph edge.
- Easy for a human to audit from an evidence span.

Recommended initial fact types:

| Fact type | Example output fields | Source sections |
|---|---|---|
| Major customer | supplier_cik, customer_name, customer_entity_id, revenue_percent, fiscal_year, named_flag | Notes to revenue, segment note, Item 1 |
| Customer concentration | company_id, top_customer_percent, top_n_customers_percent, customer_count | Notes to revenue, risk factors |
| Named supplier | company_id, supplier_entity_id, input_product, relationship_type, evidence | Item 1, risk factors, releases |
| Supply constraint | company_id, input, supplier/location, disruption_type, effective_date | Risk factors, MD&A, 8-K, news |
| Product/service | company_id, product_name, product_category, segment, revenue_percent | Item 1, segment note |
| Segment/geography exposure | company_id, segment/geography, revenue/assets/capex percent | XBRL first, text fallback |
| Facility/location | company_id, facility_type, city/state/country, ownership, capacity | Item 2, Item 1 |
| Contract/customer win | supplier_id, customer_id, contract/product, value, term, date | 8-K, press release |
| Guidance/KPI | company_id, metric_name, value/range, period, direction | 8-K, earnings release |

### Supply-chain relationship facts

Use a small relation ontology first:

```text
CUSTOMER_OF          A buys from B; edge B -> A
SUPPLIER_OF          A supplies B; edge A -> B
DISTRIBUTOR_OF       A distributes B products
MANUFACTURER_OF      A manufactures product/brand for B
STRATEGIC_PARTNER_OF broad partnership, lower precision
COMPETITOR_OF        product/market competitor
USES_INPUT           company uses commodity/component/input
SHIPS_TO             shipment evidence from shipper to consignee
OWNS_FACILITY        company to facility/location
PARENT_OF            corporate hierarchy
```

Each relation should carry:

```text
source_doc_id
source_url_or_accession
source_type
filed_at
observed_at
period_end
effective_start
effective_end
extractor_version
confidence
evidence_text
evidence_offsets
direct_or_reverse_disclosure
```

## Classical NLP architecture

### 1. Document ingestion

Ingest and store raw immutable documents before extraction:

- SEC submissions metadata from `data.sec.gov/submissions`.
- Primary filing HTML/SGML/XML from EDGAR archives.
- XBRL company facts and concepts.
- 8-K exhibits and earnings releases.
- Company investor relations press releases.
- News feeds where licensing allows.
- BOL/customs records where legally acquired.

Normalize every document into:

```text
raw_document
clean_text
html_dom_or_xml_dom
tables
sections
source_metadata
content_hash
```

### 2. Section and table parsing

Most precision comes before machine learning:

- Parse 10-K/10-Q item boundaries: Item 1, 1A, 2, 7, 8 notes, segment note.
- Preserve tables separately. Do not flatten tables into plain text and hope NLP recovers rows/columns.
- Use XBRL tags for tagged numeric facts.
- For untagged tables, extract row/column coordinates and keep cell adjacency.
- Remove boilerplate headers, page numbers, HTML navigation, exhibit lists, and repeated forward-looking safe-harbor text.

### 3. Entity detection

Use layered mention detection:

1. **Gazetteers:** SEC CIK names, ticker aliases, LEI names, OpenCorporates names, known subsidiaries, exchange names, product names, HS/HTS terms, commodity names, country/city names.
2. **Rule matchers:** spaCy `EntityRuler`/`Matcher` patterns for organizations, products, percentages, dates, money, facilities, quantities.
3. **Generic NER:** spaCy small/medium pipelines as a bootstrap for ORG/GPE/DATE/MONEY/PERCENT.
4. **Trainable NER:** CRF or spaCy NER trained on a few thousand annotated filing sentences for custom labels:
   - `PUBLIC_COMPANY`
   - `PRIVATE_COMPANY`
   - `CUSTOMER_GROUP`
   - `SUPPLIER_GROUP`
   - `PRODUCT`
   - `COMPONENT`
   - `FACILITY`
   - `PERCENT_REVENUE`
   - `FISCAL_PERIOD`

Simple features are enough for a first custom NER:

- Token lowercase, shape, prefix/suffix.
- Capitalization and punctuation.
- POS/dependency tags.
- Surrounding words.
- Gazetteer hits.
- Section name.
- Nearby trigger words such as customer, supplier, vendor, distributor, manufacturer, partner, competitor, revenue, sales, accounted for.

Sources for tooling patterns: [spaCy model capabilities](https://spacy.io/models), [scikit-learn text feature extraction](https://scikit-learn.org/stable/modules/feature_extraction.html).

### 4. Entity resolution

Entity resolution is the center of the product.

Use candidate generation plus scoring:

1. Normalize names: lowercase, strip punctuation, normalize legal suffixes, normalize whitespace, remove stop suffixes like inc/llc/ltd when useful.
2. Generate candidates using character n-gram TF-IDF and aliases.
3. Add blocking keys: country, state, exchange, ticker, CIK, LEI, website domain, address, parent, industry.
4. Score with:
   - char 3-gram TF-IDF cosine similarity
   - token Jaccard
   - Jaro-Winkler / Levenshtein
   - exact ticker/CIK/LEI/FIGI matches
   - domain/address/location match
   - parent/subsidiary context
5. Auto-accept only high-confidence matches; queue ambiguous matches.

This matches how serious vendors describe the problem. FactSet Concordance says it uses TF-IDF over name trigrams for candidate similarity, then applies attributes and reference data. The Fed's Compustat customer-segment note describes text matching plus manual review for nonstandard customer names.

Useful open identifiers:

- SEC CIK and company ticker files.
- GLEIF LEI API for legal entity search, ownership data, and fuzzy matching.
- OpenCorporates for registry records, filings, officers, corporate relationships, and jurisdictional identifiers.
- OpenFIGI for security/instrument mapping to FIGI from tickers, ISIN, CUSIP, SEDOL, and other IDs.

Sources: [FactSet Concordance methodology PDF](https://assets.ctfassets.net/lmz2w5z92b9u/6PwLI8eGYVMHVKIURBUGrJ/42d73d962f3226ed9f1e985e957891aa/Methodology.pdf), [Fed buyer-supplier matching note](https://www.federalreserve.gov/econres/notes/feds-notes/measuring-domestic-and-export-flows-in-buyer-supplier-data-20190626.html), [GLEIF API](https://www.gleif.org/en/lei-data/gleif-api), [OpenCorporates API guide](https://blog.opencorporates.com/2025/02/13/getting-started-with-the-opencorporates-api/), [OpenFIGI API](https://www.openfigi.com/api/documentation).

### 5. Relation candidate generation

Generate candidates with high-recall rules before classification:

- Same-sentence pairs: company mention + company/product/input mention.
- Nearby window: entities within one paragraph or table row.
- Table candidates: row label contains customer/supplier/revenue concentration and cells contain names/percentages.
- Section priors:
  - Notes to revenue/segments: major customer, revenue share.
  - Item 1: products, suppliers, customers, competitors.
  - Item 1A: dependence, disruption, concentration risk.
  - 8-K/exhibit: contract award, customer win, supply agreement.
  - Press release: partnership, contract, launch, manufacturing.
  - BOL: shipper/consignee shipment edge.

Example high-precision patterns:

```text
"X accounted for 22% of our net sales"
=> CUSTOMER_OF(company, X), revenue_percent=22

"No customer accounted for more than 10% of revenue"
=> CUSTOMER_CONCENTRATION(company, max_customer_percent_lt=10)

"We depend on a limited number of suppliers for semiconductor components"
=> USES_INPUT(company, semiconductor components), concentration_risk=true

"A will supply B with lithium-ion battery cells"
=> SUPPLIER_OF(A, B), product="lithium-ion battery cells"

"B selected A as its contract manufacturer"
=> MANUFACTURER_OF(A, B)

"Shipper=A, Consignee=B, commodity=fasteners"
=> SHIPS_TO(A, B), product_text="fasteners"
```

### 6. Relation classification without LLMs

After candidates are generated, use simple classifiers:

- Logistic regression / linear SVM over word n-grams and character n-grams.
- Random forest / gradient boosting over structured features.
- CRF for sequence labels when relation phrases are highly local.
- Distant supervision from known edges:
  - customer segment disclosures
  - hand-labeled 10-K sentences
  - BOL shipper/consignee pairs
  - press-release headline patterns
- Weak supervision with labeling functions.

Features:

```text
trigger words between entities
dependency path tokens
entity order
sentence voice/passive flag
section name
source form type
nearby percentages/money/dates
table row/column labels
direct/reverse disclosure
gazetteer entity types
prior known relationship count
```

Training approach:

1. Start with rule-only high precision.
2. Build a gold validation set of 1,000 to 2,000 sentences/rows.
3. Use weak labels from rules to train a classifier.
4. Review disagreement and high-uncertainty examples.
5. Keep thresholds conservative for published datasets.

Snorkel-style weak supervision is a good fit because rules, dictionaries, source metadata, and existing public datasets can all become noisy labeling functions rather than hand labels.

Source: [Snorkel weak supervision paper](https://link.springer.com/article/10.1007/s00778-019-00552-1).

### 7. Financial tone and risk flags

For sentiment/tone in filings, start with dictionary methods before ML:

- Loughran-McDonald negative, positive, uncertainty, litigious, strong modal, weak modal, constraining, and complexity categories.
- Section-level tone, not just document-level tone.
- Year-over-year deltas in Item 1A, MD&A, and liquidity sections.
- Combine with event/risk classifiers for supply-chain disruption language.

Source: [Loughran-McDonald Master Dictionary](https://sraf.nd.edu/loughranmcdonald-master-dictionary/).

## Dataset design for ats-eqt

### Core tables

```text
eqt_source_document
  doc_id
  source_type
  source_url
  accession_number
  cik
  filed_at
  period_end
  form_type
  content_hash
  retrieved_at

eqt_document_section
  section_id
  doc_id
  section_type
  title
  start_offset
  end_offset
  text_hash

eqt_entity
  entity_id
  entity_type
  canonical_name
  cik
  lei
  openfigi
  opencorporates_id
  country
  state
  website_domain
  parent_entity_id

eqt_entity_alias
  alias_id
  entity_id
  alias
  alias_norm
  source
  valid_from
  valid_to

eqt_entity_mention
  mention_id
  doc_id
  section_id
  text
  start_offset
  end_offset
  entity_id
  entity_link_score
  mention_type

eqt_relationship_fact
  fact_id
  subject_entity_id
  predicate
  object_entity_id
  object_text
  product_text
  percent_value
  money_value
  period
  effective_start
  effective_end
  source_doc_id
  evidence_text
  evidence_start
  evidence_end
  confidence
  direct_or_reverse
  extractor_version
  created_at

eqt_shipment_fact
  shipment_id
  shipper_entity_id
  consignee_entity_id
  shipper_raw
  consignee_raw
  product_description
  hs_code
  port_lading
  port_unlading
  origin_country
  destination_country
  weight_kg
  teu
  arrival_date
  source_record_id
  confidence
```

### Publishable datasets

1. **Major Customers**
   - One row per company/customer/period/evidence.
   - Highest confidence and easiest to validate.

2. **Company Relationships**
   - Customers, suppliers, partners, distributors, competitors.
   - Direct/reverse disclosure flag.
   - Source-backed, not just inferred.

3. **Supply Chain Shipment Edges**
   - Shipper to consignee edges aggregated by month/quarter/product.
   - Separate from disclosed strategic relationships.

4. **Product and Input Exposure**
   - Company to product/component/commodity exposure.
   - Built from Item 1, risk factors, BOL product descriptions, and HS/HTS mappings.

5. **Operational Risk Events**
   - Strikes, recalls, factory closures, sanctions/export controls, supplier failures, logistics disruptions.
   - Extracted from 8-Ks, news, and press releases.

## Extension design: natural-language fact extraction

This section turns the research into a build plan for an ats-eqt extension. The goal is a deterministic, inspectable pipeline that converts documents into evidence-backed rows.

### What was missing from the first pass

The first pass covered sources, vendor patterns, and broad architecture. For implementation, ats-eqt also needs:

- A precise **fact contract**: what every extracted fact must contain.
- A stable **document object model**: raw HTML/XML, clean text, tables, sections, sentence offsets.
- A **label ontology** for entity recognition.
- A **relation ontology** for fact recognition.
- Pseudocode for ingestion, segmentation, NER, entity linking, candidate generation, classification, confidence, and publishing.
- A table design that separates raw documents, text spans, extracted mentions, entity-resolution candidates, accepted entities, fact candidates, published facts, QA decisions, and extractor versions.
- A way to preserve "unknown but useful" values. Example: `object_text="one customer"` is still a publishable concentration fact even if the customer is unnamed.
- A weak-supervision loop so rules become training data.
- A human-review loop so corrections become aliases, labeling functions, and negative examples.
- A scoring model that lets ats-eqt publish only high-precision rows while keeping lower-confidence candidates internally.

### Fact contract

Every extractor should output the same envelope, even if the payload differs by fact type.

```text
FactEnvelope
  fact_type                enum
  subject_entity_id        nullable entity id
  subject_text             raw text fallback
  predicate                enum
  object_entity_id         nullable entity id
  object_text              raw text fallback
  payload                  typed key/value map
  source_doc_id            required
  source_section_id        nullable
  evidence_text            required
  evidence_start_offset    required
  evidence_end_offset      required
  source_report_date       filing date / publication timestamp
  source_period_end        nullable
  effective_start          nullable
  effective_end            nullable
  observed_at              retrieval timestamp
  extractor_name           required
  extractor_version        required
  confidence               numeric 0..1
  confidence_breakdown     JSON/object
  direct_or_reverse        enum direct, reverse, inferred, unknown
  publish_status           candidate, published, rejected, superseded
```

The envelope makes the output auditable. A consumer should be able to click from a table row to the exact source sentence/table row.

### Fact payload examples

```text
MAJOR_CUSTOMER
  revenue_percent
  account_receivable_percent
  purchase_percent
  fiscal_year
  named_flag
  rank
  threshold_phrase
  negated_flag

SUPPLIER_RELATIONSHIP
  product_text
  component_text
  exclusivity_flag
  sole_source_flag
  dependency_flag
  risk_flag

CONTRACT_AWARD
  contract_value
  contract_value_currency
  contract_term_text
  product_text
  award_date

FACILITY
  facility_type
  location_text
  city
  state
  country
  capacity_text
  owned_or_leased

SHIPMENT_EDGE
  shipper_raw
  consignee_raw
  product_description
  hs_code
  port_lading
  port_unlading
  origin_country
  destination_country
  weight_kg
  teu
  arrival_date
```

### End-to-end extraction DAG

```text
source registry
  -> fetch raw document
  -> hash and store raw artifact
  -> parse document structure
  -> extract sections
  -> extract tables
  -> clean text with offsets
  -> sentence segmentation
  -> mention detection
  -> entity linking
  -> candidate fact generation
  -> relation/fact classification
  -> numeric/date/unit normalization
  -> confidence scoring
  -> deduplication
  -> QA queue
  -> publishable fact tables
  -> materialized dataset views
```

The important engineering point is offset preservation. Cleaning should never destroy the ability to point back to the original text.

### Pipeline pseudocode

```python
def run_document_pipeline(doc_ref: DocumentRef) -> list[FactEnvelope]:
    raw = fetch_document(doc_ref)
    doc_id = store_raw_document(raw, doc_ref)

    parsed = parse_document(raw)
    sections = segment_sections(parsed)
    tables = extract_tables(parsed)
    text_blocks = normalize_text_blocks(parsed, sections, tables)

    sentences = []
    for block in text_blocks:
        sentences.extend(sentence_split(block))

    mentions = []
    for sent in sentences:
        mentions.extend(detect_mentions(sent))

    linked_mentions = link_mentions_to_entities(mentions)

    candidates = []
    candidates.extend(generate_sentence_relation_candidates(sentences, linked_mentions))
    candidates.extend(generate_table_fact_candidates(tables, linked_mentions))
    candidates.extend(generate_document_level_candidates(sections, tables, linked_mentions))

    normalized = []
    for cand in candidates:
        cand = normalize_numbers_dates_units(cand)
        cand = classify_fact_candidate(cand)
        cand = score_fact_candidate(cand)
        normalized.append(cand)

    deduped = dedupe_candidates(normalized)
    store_candidates(doc_id, deduped)

    publishable = [c for c in deduped if c.confidence >= publish_threshold(c.fact_type)]
    publish_facts(publishable)
    enqueue_review([c for c in deduped if needs_review(c)])

    return publishable
```

### Batch pseudocode

```python
def run_incremental_batch(as_of: datetime) -> None:
    docs = source_registry.list_new_or_changed_documents(as_of)
    for doc_ref in docs:
        try:
            run_document_pipeline(doc_ref)
            mark_success(doc_ref)
        except RecoverableParseError as err:
            mark_retry(doc_ref, err)
        except Exception as err:
            mark_failed(doc_ref, err)
```

### Document object model

Store document structure independently from extracted facts.

```text
DocumentArtifact
  doc_id
  source_type
  source_url
  accession_number
  cik
  form_type
  filed_at
  period_end
  raw_mime_type
  raw_bytes_hash
  raw_storage_uri
  parsed_storage_uri
  parser_name
  parser_version
  created_at

TextBlock
  block_id
  doc_id
  section_id
  block_type
  text
  clean_start_offset
  clean_end_offset
  raw_start_offset
  raw_end_offset
  table_id
  row_index
  col_index

Sentence
  sentence_id
  block_id
  doc_id
  text
  start_offset
  end_offset
  sentence_index
```

The `raw_start_offset` and `raw_end_offset` fields can be hard to maintain for broken SEC HTML. If exact raw offsets are not possible, store a block hash and DOM path as fallback.

### SEC document parsing details

For EDGAR filings, use a layered parser:

1. Parse SGML submission envelope.
2. Identify primary document.
3. Identify exhibits.
4. Parse primary HTML as DOM.
5. Remove hidden XBRL metadata blocks from visible text output, but store them separately.
6. Detect section headings with DOM, text, and regex rules.
7. Extract tables before text flattening.
8. Preserve links from text sentences to table cells/DOM nodes.

Section extraction should tolerate noisy headings.

```text
ITEM 1. BUSINESS
Item 1 - Business
ITEM 1A. RISK FACTORS
Risk Factors
PART I
Item 2. Properties
NOTES TO CONSOLIDATED FINANCIAL STATEMENTS
Revenue Recognition
Segment Information
Major Customers
Concentration of Credit Risk
```

### SEC section segmentation pseudocode

```python
ITEM_PATTERNS = [
    (r"\bitem\s+1\b[^a-z0-9]{0,20}business\b", "ITEM_1_BUSINESS"),
    (r"\bitem\s+1a\b[^a-z0-9]{0,20}risk\s+factors\b", "ITEM_1A_RISK"),
    (r"\bitem\s+2\b[^a-z0-9]{0,20}properties\b", "ITEM_2_PROPERTIES"),
    (r"\bitem\s+7\b[^a-z0-9]{0,20}management", "ITEM_7_MDA"),
    (r"\bitem\s+8\b[^a-z0-9]{0,20}financial", "ITEM_8_FINANCIALS"),
]

NOTE_PATTERNS = [
    (r"\brevenue\s+recognition\b", "NOTE_REVENUE"),
    (r"\bsegment\s+information\b", "NOTE_SEGMENTS"),
    (r"\bconcentration\s+of\s+(credit\s+)?risk\b", "NOTE_CONCENTRATION"),
    (r"\bmajor\s+customers?\b", "NOTE_MAJOR_CUSTOMERS"),
]

def segment_sections(parsed_doc: ParsedDocument) -> list[Section]:
    headings = find_heading_candidates(parsed_doc)
    labeled = []
    for h in headings:
        norm = normalize_heading(h.text)
        label = match_first(norm, ITEM_PATTERNS) or match_first(norm, NOTE_PATTERNS)
        if label:
            labeled.append((h, label))
    return build_non_overlapping_sections(labeled, parsed_doc.length)
```

### Table parsing details

Financial facts often live in tables:

```text
Customer A      38%      41%
Customer B      14%      16%
No other customer exceeded 10%
```

Tables should be represented as cells, rows, columns, and row/column labels.

```text
Table
  table_id
  doc_id
  section_id
  caption_text
  normalized_caption
  row_count
  col_count
  parse_quality

TableCell
  cell_id
  table_id
  row_index
  col_index
  row_span
  col_span
  text
  numeric_value
  unit
  inferred_row_header
  inferred_col_header
  raw_html_hash
```

### Table fact extraction pseudocode

```python
def generate_table_fact_candidates(tables, linked_mentions):
    for table in tables:
        context = table.caption_text + " " + table.section_title
        table_kind = classify_table_kind(context, table)

        if table_kind in {"MAJOR_CUSTOMERS", "CONCENTRATION"}:
            yield from extract_major_customer_table(table)

        if table_kind in {"SEGMENT", "GEOGRAPHY"}:
            yield from extract_segment_exposure_table(table)

        if table_kind in {"FACILITIES", "PROPERTIES"}:
            yield from extract_facility_table(table)

def extract_major_customer_table(table):
    for row in table.rows:
        row_label = normalize_text(row.header_text)
        pct_cells = [c for c in row.cells if c.unit == "percent"]
        if not pct_cells:
            continue
        if looks_like_customer_label(row_label):
            for pct in pct_cells:
                yield FactCandidate(
                    fact_type="MAJOR_CUSTOMER",
                    subject=table.reporting_company,
                    object_text=row.header_text,
                    payload={
                        "revenue_percent": pct.numeric_value,
                        "period": pct.column_header,
                    },
                    evidence=table.row_text(row),
                )
```

### Entity label ontology

Start with a small custom NER ontology. Avoid dozens of labels early.

```text
ORG_PUBLIC          public operating company
ORG_PRIVATE         private company, supplier, customer, distributor
ORG_GOV             government agency or public-sector customer
ORG_GROUP           unnamed group: "one customer", "two distributors"
PRODUCT             named product or branded product
PRODUCT_CLASS       generic product family or category
COMPONENT           input, part, commodity, raw material
FACILITY            plant, warehouse, mine, fab, data center, office
LOCATION            geography, site, port, region
PERCENT             percent expression
MONEY               monetary expression
QUANTITY            physical quantity or capacity
DATE                explicit date
PERIOD              fiscal year, quarter, or reporting period
CONTRACT            named agreement, contract, award, purchase order
```

Use existing NER labels as inputs, but map them into ats-eqt labels. Generic `ORG` from spaCy is not enough because the system needs public/private/government/group distinctions.

### Mention record

```text
Mention
  mention_id
  doc_id
  sentence_id
  section_id
  text
  normalized_text
  label
  start_offset
  end_offset
  detector_name
  detector_version
  detector_confidence
  linked_entity_id
  link_confidence
```

### Mention detection layers

Layer 1: deterministic entities.

```text
CIK/ticker universe
known issuer names
known subsidiary names
known exchange suffixes
known country/state/city names
known legal suffixes
known product dictionaries
known commodity dictionaries
HS/HTS code dictionaries
```

Layer 2: regex entities.

```text
percentages
money values
fiscal years
contract values
period ranges
customer ordinal phrases
facility capacity phrases
port/country phrases
```

Layer 3: statistical NER.

```text
CRF sequence model
spaCy transition-based NER
averaged perceptron sequence tagger
logistic token classifier plus Viterbi decoding
```

Layer 4: post-processing.

```text
merge adjacent ORG tokens
expand legal suffixes
trim determiners
split coordination cautiously
recover ticker in parentheses
normalize punctuation
```

### Regex mention examples

```python
PERCENT_PAT = r"(?P<value>\d+(?:\.\d+)?)\s?%"
FISCAL_YEAR_PAT = r"(fiscal\s+)?(year\s+)?(?P<year>20\d{2}|19\d{2})"
CUSTOMER_GROUP_PAT = r"\b(one|two|three|four|five|\d+)\s+(major\s+)?customers?\b"
NO_CUSTOMER_PAT = r"\bno\s+(single\s+)?customer\s+(accounted\s+for|represented|exceeded)\b"
SUPPLIER_TRIGGER_PAT = r"\b(supplier|vendor|manufacturer|fabricator|foundry|contract manufacturer)\b"
```

### CRF NER feature template

For classical NER, a CRF is a good baseline because filings have repetitive local syntax.

```python
def token_features(sent, i):
    tok = sent[i]
    prev = sent[i - 1] if i > 0 else None
    nxt = sent[i + 1] if i + 1 < len(sent) else None

    return {
        "bias": 1.0,
        "lower": tok.text.lower(),
        "shape": token_shape(tok.text),
        "prefix2": tok.text[:2].lower(),
        "prefix3": tok.text[:3].lower(),
        "suffix2": tok.text[-2:].lower(),
        "suffix3": tok.text[-3:].lower(),
        "is_title": tok.text.istitle(),
        "is_upper": tok.text.isupper(),
        "is_digit": tok.text.isdigit(),
        "has_digit": any(c.isdigit() for c in tok.text),
        "pos": tok.pos_,
        "dep": tok.dep_,
        "section": sent.section_type,
        "in_org_gazetteer": gazetteer.org.contains(tok.text),
        "in_product_gazetteer": gazetteer.product.contains(tok.text),
        "prev_lower": prev.text.lower() if prev else "<BOS>",
        "next_lower": nxt.text.lower() if nxt else "<EOS>",
        "prev_is_trigger": prev.text.lower() in TRIGGER_WORDS if prev else False,
        "next_is_trigger": nxt.text.lower() in TRIGGER_WORDS if nxt else False,
    }
```

### NER training data format

Use a simple JSONL format that preserves document context.

```json
{"doc_id":"0000320193-25-000010","section":"NOTE_REVENUE","text":"Customer A accounted for 15% of net sales in 2025.","spans":[{"start":0,"end":10,"label":"ORG_PRIVATE"},{"start":25,"end":28,"label":"PERCENT"},{"start":45,"end":49,"label":"PERIOD"}]}
```

### NER training pseudocode

```python
def train_crf_ner(train_examples, valid_examples):
    X_train = []
    y_train = []
    for ex in train_examples:
        sent = tokenize(ex.text)
        X_train.append([token_features(sent, i) for i in range(len(sent))])
        y_train.append(bio_tags_from_spans(sent, ex.spans))

    model = CRF(
        algorithm="lbfgs",
        c1=0.1,
        c2=0.1,
        max_iterations=100,
        all_possible_transitions=True,
    )
    model.fit(X_train, y_train)
    evaluate_sequence_model(model, valid_examples)
    return model
```

### Gazetteer matching pseudocode

```python
def detect_gazetteer_mentions(sentence):
    hits = alias_index.longest_prefix_matches(sentence.tokens)
    for hit in hits:
        if hit.score < 0.95:
            continue
        yield Mention(
            text=hit.text,
            label=hit.entity_type_hint,
            start_offset=hit.start,
            end_offset=hit.end,
            detector_name="alias_gazetteer",
            detector_confidence=hit.score,
        )
```

### Mention post-processing rules

```text
Rule: merge legal suffix
  "Taiwan Semiconductor Manufacturing" + "Company" + "Limited"
  -> "Taiwan Semiconductor Manufacturing Company Limited"

Rule: attach ticker parenthetical
  "Microsoft Corporation (MSFT)"
  -> mention text "Microsoft Corporation"; alias hint ticker=MSFT

Rule: do not treat generic groups as resolved companies
  "one customer"
  -> ORG_GROUP, linked_entity_id = null

Rule: preserve government customers
  "U.S. government"
  -> ORG_GOV, entity may be a government aggregate

Rule: split coordinated private customers only when pattern is clear
  "Apple, Samsung and Dell"
  -> three ORG mentions
```

## Entity resolution design

Entity resolution should be a first-class subsystem, not a helper function.

### Alias normalization

```python
LEGAL_SUFFIXES = {
    "inc", "incorporated", "corp", "corporation", "co", "company",
    "llc", "l.l.c", "ltd", "limited", "plc", "sa", "ag", "nv",
    "gmbh", "sarl", "bv", "kk", "pte", "lp", "llp",
}

def normalize_company_name(name: str) -> str:
    s = name.lower()
    s = unicode_to_ascii(s)
    s = replace_ampersand(s)
    s = strip_punctuation(s)
    s = normalize_whitespace(s)
    toks = [t for t in s.split() if t not in LEGAL_SUFFIXES]
    return " ".join(toks)
```

### Candidate generation

Use multiple candidate sources and merge them.

```text
exact alias match
normalized alias match
ticker/CIK/LEI/FIGI match
website domain match
address match
char 3-gram TF-IDF nearest neighbors
token-set fuzzy match
parent/subsidiary aliases
country/state blocked search
```

### Candidate generation pseudocode

```python
def generate_entity_candidates(mention: Mention, context: LinkContext) -> list[EntityCandidate]:
    candidates = []

    candidates += exact_alias_index.lookup(mention.normalized_text)

    if context.ticker_hint:
        candidates += ticker_index.lookup(context.ticker_hint)

    if context.lei_hint:
        candidates += lei_index.lookup(context.lei_hint)

    block = EntityBlock(
        country=context.country_hint,
        entity_type=context.entity_type_hint,
        first_char=mention.normalized_text[:1],
    )
    candidates += trigram_index.search(
        mention.normalized_text,
        block=block,
        top_k=25,
    )

    return dedupe_candidates(candidates)
```

### Link scoring features

```text
name_trigram_cosine
name_token_jaccard
rapidfuzz_ratio
legal_suffix_match
country_match
state_match
domain_match
ticker_match
cik_match
lei_match
openfigi_match
parent_context_match
section_prior
source_company_prior
historical_edge_prior
```

### Link scoring pseudocode

```python
def score_entity_candidate(mention, candidate, context):
    features = {
        "name_trigram_cosine": trigram_cosine(mention.norm, candidate.alias_norm),
        "token_jaccard": jaccard(tokens(mention.norm), tokens(candidate.alias_norm)),
        "fuzzy_ratio": fuzz_ratio(mention.norm, candidate.alias_norm),
        "country_match": mention.country_hint == candidate.country,
        "ticker_match": context.ticker_hint == candidate.ticker,
        "domain_match": context.domain_hint == candidate.domain,
        "parent_match": context.parent_hint == candidate.parent_entity_id,
        "public_company_prior": candidate.entity_type == "PUBLIC_COMPANY",
    }
    return entity_link_model.predict_proba(features)
```

### Auto-accept policy

```text
accept if:
  exact CIK/LEI/ticker match and no conflicting country
  OR score >= 0.97 and margin_to_second_best >= 0.08
  OR score >= 0.92 and candidate already has same edge in prior filing

review if:
  score between 0.70 and 0.97
  OR second-best margin is small
  OR mention maps to a private company with common name
  OR mention appears in a table with high-value fact

reject/unlinked if:
  score < 0.70
  OR mention is generic group
  OR mention is product/facility, not organization
```

### Entity resolution output tables

```sql
CREATE TABLE eqt_entity_candidate (
  candidate_id TEXT PRIMARY KEY,
  mention_id TEXT NOT NULL,
  entity_id TEXT NOT NULL,
  rank INTEGER NOT NULL,
  score DOUBLE NOT NULL,
  score_breakdown_json TEXT NOT NULL,
  candidate_source TEXT NOT NULL,
  created_at TIMESTAMP NOT NULL
);

CREATE TABLE eqt_entity_link_decision (
  decision_id TEXT PRIMARY KEY,
  mention_id TEXT NOT NULL,
  selected_entity_id TEXT,
  decision_status TEXT NOT NULL,
  decision_source TEXT NOT NULL,
  reviewer_id TEXT,
  reason_code TEXT,
  created_at TIMESTAMP NOT NULL
);
```

## Fact recognition design

Fact recognition has two stages:

1. Candidate generation with high recall.
2. Candidate classification/scoring with conservative publication thresholds.

### Candidate object

```text
FactCandidate
  candidate_id
  fact_type
  subject_mention_id
  subject_entity_id
  object_mention_id
  object_entity_id
  object_text
  sentence_id
  table_id
  source_section_id
  evidence_text
  trigger_text
  payload
  generator_name
  generator_confidence
```

### Candidate generation sources

```text
sentence patterns
dependency paths
nearby entity windows
table row/column patterns
section-level summary phrases
document metadata
XBRL context around tagged facts
shipment records
news headline patterns
press release boilerplate structures
```

### Relation candidate pseudocode

```python
def generate_sentence_relation_candidates(sentences, mentions):
    by_sentence = group_mentions_by_sentence(mentions)

    for sent in sentences:
        sent_mentions = by_sentence.get(sent.id, [])
        orgs = [m for m in sent_mentions if m.label in ORG_LABELS]
        products = [m for m in sent_mentions if m.label in PRODUCT_LABELS]
        percents = [m for m in sent_mentions if m.label == "PERCENT"]

        if has_customer_trigger(sent.text):
            yield from customer_candidates(sent, orgs, percents)

        if has_supplier_trigger(sent.text):
            yield from supplier_candidates(sent, orgs, products)

        if has_competitor_trigger(sent.text):
            yield from competitor_candidates(sent, orgs)

        if has_contract_trigger(sent.text):
            yield from contract_candidates(sent, orgs, products)
```

### Trigger lexicons

```text
CUSTOMER_TRIGGERS
  customer
  customers
  client
  clients
  accounted for
  represented
  generated
  derived from
  sales to
  revenue from
  net sales to
  concentration

SUPPLIER_TRIGGERS
  supplier
  suppliers
  vendor
  vendors
  source from
  sourced from
  supplied by
  purchase from
  component from
  sole source
  contract manufacturer
  foundry
  distributor

PARTNER_TRIGGERS
  partnership
  collaboration
  alliance
  joint development
  strategic agreement
  reseller agreement
  distribution agreement

COMPETITOR_TRIGGERS
  compete
  competitor
  competition
  rival
  alternative to
  market participants

FACILITY_TRIGGERS
  facility
  plant
  factory
  warehouse
  distribution center
  fab
  foundry
  mine
  refinery
  data center
```

### Rule pattern format

Store rules as data so they can be versioned and audited.

```yaml
id: major_customer_named_percent_v1
fact_type: MAJOR_CUSTOMER
source_sections:
  - NOTE_REVENUE
  - NOTE_CONCENTRATION
  - ITEM_1_BUSINESS
pattern:
  regex: "(?P<object>[^.;]{2,80}?)\\s+(accounted for|represented|comprised)\\s+(?P<pct>\\d+(\\.\\d+)?)\\s?%\\s+of\\s+(our\\s+)?(?P<metric>net sales|revenue|sales)"
negation:
  forbid_regex: "\\bno\\s+customer\\b"
payload:
  percent_field: revenue_percent
  metric_group: revenue
confidence:
  base: 0.92
```

### Customer concentration extractor pseudocode

```python
def extract_customer_concentration(sentence, reporting_company):
    text = normalize_space(sentence.text)

    if re.search(r"\bno\s+(single\s+)?customer\s+.*(10|ten)\s?%", text, re.I):
        return FactCandidate(
            fact_type="CUSTOMER_CONCENTRATION",
            subject_entity_id=reporting_company.entity_id,
            predicate="HAS_NO_MAJOR_CUSTOMER",
            object_text=None,
            payload={"max_customer_percent_lt": 10, "named_flag": False},
            evidence_text=sentence.text,
            generator_confidence=0.95,
        )

    for m in re.finditer(MAJOR_CUSTOMER_REGEX, text, re.I):
        object_text = clean_customer_name(m.group("object"))
        pct = parse_percent(m.group("pct"))
        metric = normalize_metric(m.group("metric"))

        return FactCandidate(
            fact_type="MAJOR_CUSTOMER",
            subject_entity_id=reporting_company.entity_id,
            predicate="HAS_MAJOR_CUSTOMER",
            object_text=object_text,
            payload={f"{metric}_percent": pct, "named_flag": is_named(object_text)},
            evidence_text=sentence.text,
            generator_confidence=0.90,
        )
```

### Supplier relationship extractor pseudocode

```python
def extract_supplier_relationship(sentence, reporting_company, linked_mentions):
    text = sentence.text.lower()
    if not has_supplier_trigger(text):
        return []

    candidates = []
    orgs = org_mentions(sentence, linked_mentions)
    products = product_mentions(sentence, linked_mentions)

    for org in orgs:
        direction = infer_supplier_direction(sentence.text, reporting_company, org)
        if direction == "ORG_SUPPLIES_COMPANY":
            subject = org.entity_id
            obj = reporting_company.entity_id
            predicate = "SUPPLIER_OF"
        elif direction == "COMPANY_SUPPLIES_ORG":
            subject = reporting_company.entity_id
            obj = org.entity_id
            predicate = "SUPPLIER_OF"
        else:
            continue

        candidates.append(FactCandidate(
            fact_type="SUPPLIER_RELATIONSHIP",
            subject_entity_id=subject,
            predicate=predicate,
            object_entity_id=obj,
            payload={
                "product_text": nearest_product_text(org, products),
                "sole_source_flag": "sole source" in text,
                "dependency_flag": has_dependency_language(text),
            },
            evidence_text=sentence.text,
            generator_confidence=0.75,
        ))

    return candidates
```

### Direction inference rules

```text
Pattern: "we purchase X from Supplier"
  Supplier -> reporting company

Pattern: "Supplier provides X to us"
  Supplier -> reporting company

Pattern: "we supply X to Customer"
  reporting company -> Customer

Pattern: "Customer purchases X from us"
  reporting company -> Customer

Pattern: "we rely on Supplier for X"
  Supplier -> reporting company

Pattern: "we are a supplier to Customer"
  reporting company -> Customer

Pattern: "our customer Customer"
  reporting company -> Customer

Pattern: "our supplier Supplier"
  Supplier -> reporting company
```

### Relation classifier

Rules should generate candidates. A classifier should decide whether the candidate is valid and which predicate it represents.

Recommended v1 model:

```text
model: logistic regression or linear SVM
features:
  word n-grams in sentence
  character n-grams in sentence
  trigger words
  dependency path tokens
  entity order
  distance between entities
  section type
  source type
  table/sentence flag
  nearby percentage/money/date flags
  entity labels
  entity-link confidence
outputs:
  CUSTOMER_OF
  SUPPLIER_OF
  PARTNER_OF
  COMPETITOR_OF
  NO_RELATION
```

### Relation feature pseudocode

```python
def relation_features(candidate):
    sent = candidate.sentence
    subj = candidate.subject_mention
    obj = candidate.object_mention

    between = text_between(sent.text, subj.span, obj.span)
    left = window_left(sent.text, subj.span, 5)
    right = window_right(sent.text, obj.span, 5)

    return {
        "section": candidate.section_type,
        "source_type": candidate.source_type,
        "fact_type_hint": candidate.fact_type,
        "entity_order": entity_order(subj, obj),
        "token_distance": token_distance(subj, obj),
        "between_tokens": bag(between),
        "left_tokens": bag(left),
        "right_tokens": bag(right),
        "has_customer_trigger": has_customer_trigger(sent.text),
        "has_supplier_trigger": has_supplier_trigger(sent.text),
        "has_partner_trigger": has_partner_trigger(sent.text),
        "has_competitor_trigger": has_competitor_trigger(sent.text),
        "has_percent": bool(candidate.nearby_percents),
        "has_money": bool(candidate.nearby_money),
        "subject_link_conf": subj.link_confidence,
        "object_link_conf": obj.link_confidence,
        "dependency_path": dependency_path_tokens(sent, subj, obj),
    }
```

### Relation classifier training pseudocode

```python
def train_relation_classifier(candidates, gold_labels):
    X = [relation_features(c) for c in candidates]
    y = [gold_labels[c.candidate_id] for c in candidates]

    vectorizer = DictVectorizer()
    Xv = vectorizer.fit_transform(X)

    clf = LogisticRegression(
        max_iter=1000,
        class_weight="balanced",
        C=1.0,
    )
    clf.fit(Xv, y)

    calibrator = CalibratedClassifierCV(clf, method="isotonic", cv=3)
    calibrator.fit(Xv, y)

    return RelationModel(vectorizer, calibrator)
```

### Weak supervision

Weak supervision turns rules into noisy training labels. This is ideal because ats-eqt will have many heuristics and few labels at the beginning.

Example label set:

```python
ABSTAIN = -1
NO_RELATION = 0
CUSTOMER_OF = 1
SUPPLIER_OF = 2
PARTNER_OF = 3
COMPETITOR_OF = 4
```

Labeling functions:

```python
def lf_customer_accounted_for(c):
    if "accounted for" in c.sentence.lower() and has_percent(c):
        return CUSTOMER_OF
    return ABSTAIN

def lf_supplier_purchase_from(c):
    text = c.sentence.lower()
    if "purchase" in text and "from" in text and has_supplier_trigger(text):
        return SUPPLIER_OF
    return ABSTAIN

def lf_compete_with(c):
    text = c.sentence.lower()
    if "compete with" in text or "competitors include" in text:
        return COMPETITOR_OF
    return ABSTAIN

def lf_negated_no_customer(c):
    if re.search(r"\bno\s+(single\s+)?customer\b", c.sentence.lower()):
        return NO_RELATION
    return ABSTAIN

def lf_too_far_apart(c):
    if token_distance(c.subject_mention, c.object_mention) > 60:
        return NO_RELATION
    return ABSTAIN
```

Weak labeling pseudocode:

```python
def weak_label_candidates(candidates, labeling_functions):
    matrix = []
    for c in candidates:
        matrix.append([lf(c) for lf in labeling_functions])

    label_model = LabelModel(cardinality=5)
    label_model.fit(matrix, n_epochs=500, log_freq=100)
    probs = label_model.predict_proba(matrix)

    return probs
```

Use weak labels to:

- bootstrap relation classifiers
- identify rule conflicts
- rank examples for human labeling
- measure which rules are noisy
- create high-precision seed data

### Active review loop

```python
def select_review_items(candidates):
    return sorted(
        candidates,
        key=lambda c: (
            c.high_business_value,
            c.confidence_near_threshold,
            c.entity_link_ambiguous,
            c.rule_conflict_count,
            c.new_pattern_score,
        ),
        reverse=True,
    )[:REVIEW_BATCH_SIZE]
```

Human review decisions should feed three stores:

```text
approved/rejected fact labels
entity alias/link corrections
new negative examples
```

Then update:

```text
alias tables
blocking rules
labeling functions
validation sets
model training data
```

## Confidence scoring

Confidence should be explainable, not just a classifier probability.

### Confidence components

```text
rule_score                  how precise the generator rule is
classifier_score            calibrated relation probability
subject_link_score          entity-link confidence
object_link_score           entity-link confidence
section_score               source section prior
source_score                source type reliability
numeric_parse_score         confidence in percent/money/date extraction
evidence_quality_score      clean evidence span, not boilerplate
cross_source_support_score  supported by previous filings/news/BOL
staleness_penalty           older relationship without recent support
negation_penalty            sentence contains negation/hedging
ambiguity_penalty           multiple entities/values possible
```

### Confidence pseudocode

```python
def score_fact_candidate(c):
    subject_link = (
        c.subject_link_confidence
        if c.subject_link_confidence is not None
        else 1.0
    )
    object_link = (
        c.object_link_confidence
        if c.object_link_confidence is not None
        else (0.8 if c.object_text else 1.0)
    )

    parts = {
        "rule": c.generator_confidence,
        "classifier": c.classifier_probability,
        "subject_link": subject_link,
        "object_link": object_link,
        "section": section_prior(c.section_type, c.fact_type),
        "source": source_prior(c.source_type),
        "numeric": numeric_parse_confidence(c.payload),
        "evidence": evidence_quality(c.evidence_text),
        "support": cross_source_support(c),
        "negation_penalty": negation_penalty(c.evidence_text),
        "ambiguity_penalty": ambiguity_penalty(c),
    }

    base = geometric_mean([
        parts["rule"],
        parts["classifier"],
        parts["subject_link"],
        parts["object_link"],
        parts["section"],
        parts["source"],
        parts["numeric"],
        parts["evidence"],
    ])

    score = base
    score += 0.05 * parts["support"]
    score -= parts["negation_penalty"]
    score -= parts["ambiguity_penalty"]

    return clamp(score, 0.0, 1.0), parts
```

### Publication thresholds

```text
MAJOR_CUSTOMER                  0.90
CUSTOMER_CONCENTRATION          0.90
CUSTOMER_OF                     0.88
SUPPLIER_OF                     0.85
COMPETITOR_OF                   0.82
PARTNER_OF                      0.90
CONTRACT_AWARD                  0.88
FACILITY                        0.80
PRODUCT_EXPOSURE                0.78
SHIPMENT_EDGE                   0.75
SUPPLY_CHAIN_RISK_EVENT         0.85
```

Publish thresholds can be tuned per dataset. The internal candidate store should retain lower-confidence rows.

## Fact type playbooks

### Major customer playbook

High-value patterns:

```text
"Customer A accounted for 18% of net sales"
"one customer accounted for 22% of revenues"
"No customer accounted for more than 10% of revenue"
"Customers A and B represented 31% and 14% of accounts receivable"
"sales to the U.S. government represented 46% of total revenue"
```

Fields:

```text
company_id
customer_entity_id
customer_raw
customer_group_flag
metric_type
percent_value
period
fiscal_year
named_flag
no_major_customer_flag
source_doc_id
evidence_text
confidence
```

Special cases:

```text
"two customers accounted for 45% and 12%"
  -> two rows with customer_raw = "unnamed customer 1/2"

"one customer accounted for 90% of accounts receivable"
  -> metric_type = accounts_receivable, not revenue

"no customer accounted for more than 10%"
  -> no_major_customer_flag = true

"government customers"
  -> customer_entity_id can be government aggregate
```

### Supplier playbook

High-value patterns:

```text
"we rely on Taiwan Semiconductor Manufacturing Company to manufacture..."
"our sole supplier of wafers is..."
"we purchase substantially all of our lithium carbonate from..."
"a disruption at our supplier's facility could..."
"we have a supply agreement with..."
```

Fields:

```text
company_id
supplier_entity_id
supplier_raw
input_product_text
sole_source_flag
dependency_flag
agreement_flag
risk_flag
relationship_direction
source_doc_id
evidence_text
confidence
```

Special cases:

```text
"limited number of suppliers"
  -> supplier_entity_id = null, object_text = "limited number of suppliers"

"foundries in Taiwan"
  -> object_text = geography/product group, lower confidence

"supplier may fail"
  -> risk event plus relationship candidate
```

### Competitor playbook

High-value patterns:

```text
"Our competitors include A, B and C"
"We compete with A in the market for..."
"Competition comes from large cloud providers such as..."
```

Fields:

```text
company_id
competitor_entity_id
competitor_raw
market_text
product_text
source_doc_id
evidence_text
confidence
```

Competitors are often listed in dense sentences. Use high precision and do not over-split vague categories.

### Contract award playbook

High-value patterns:

```text
"Company A awarded Company B a $500 million contract"
"Company A selected Company B to supply..."
"Company A entered into a multi-year supply agreement with Company B"
```

Fields:

```text
awardee_entity_id
customer_entity_id
contract_value
currency
term_text
product_text
award_date
source_doc_id
evidence_text
confidence
```

Contract award extraction is easier from 8-Ks and press releases than 10-Ks.

### Facility playbook

High-value patterns:

```text
"We operate a 300,000 square foot manufacturing facility in Austin, Texas"
"The company opened a new battery plant in Georgia"
"Our principal executive offices are located in..."
```

Fields:

```text
company_id
facility_type
location_text
city
state
country
capacity_text
owned_or_leased
opened_closed_status
source_doc_id
evidence_text
confidence
```

Facility extraction needs address/location parsing and geocoding. Do not require geocoding for publication if the raw location text is clear.

### Product/input exposure playbook

High-value patterns:

```text
"Our products include..."
"We depend on lithium, cobalt and nickel"
"Semiconductors are a key component of..."
"Revenue from Product X represented..."
```

Fields:

```text
company_id
product_or_input_text
normalized_product_id
exposure_type
percent_value
risk_flag
source_doc_id
evidence_text
confidence
```

Normalize products gradually. Start with raw text plus a product taxonomy mapping table.

## Shipment-data extension

Shipment rows are structured but messy. They need NLP mostly for names and product descriptions.

### Shipment normalization steps

```text
normalize shipper name
normalize consignee name
parse addresses
link shipper to entity
link consignee to entity
normalize product description
extract quantities/units
map port/country codes
infer HS/HTS when missing
identify freight forwarder/trading company intermediaries
aggregate repeated shipments
```

### Product description classifier

Use classical text classification:

```text
input: "LED LIGHTING FIXTURES PARTS"
features:
  word n-grams
  character n-grams
  normalized units
  country/port priors
  shipper/consignee industry priors
model:
  logistic regression / linear SVM
output:
  hs6 candidate
  product_class
  confidence
```

### HS imputation pseudocode

```python
def infer_hs_code(shipment):
    text = normalize_product_description(shipment.product_description)
    features = product_vectorizer.transform([text])
    probs = hs_classifier.predict_proba(features)
    hs6, prob = top_label(probs)

    if prob < 0.70:
        return None, prob

    if not industry_prior_allows(hs6, shipment.consignee_entity_id):
        prob *= 0.85

    return hs6, prob
```

### Freight intermediary detection

Not every shipper/consignee pair is the economic supplier/customer.

Intermediary signals:

```text
name contains logistics, forwarding, freight, nvocc, trading, customs broker
entity industry is freight/logistics
appears with many unrelated counterparties
product descriptions vary widely
address is port/warehouse
consignee is "to order"
```

Pseudocode:

```python
def intermediary_score(entity, shipment_history):
    score = 0
    score += 0.30 if has_logistics_name(entity.name) else 0
    score += 0.25 if entity.industry in LOGISTICS_INDUSTRIES else 0
    score += 0.20 if shipment_history.counterparty_count > 500 else 0
    score += 0.15 if shipment_history.product_entropy > 0.8 else 0
    score += 0.10 if is_port_or_warehouse_address(entity.address) else 0
    return min(score, 1.0)
```

Shipment facts should include intermediary flags so consumers can filter.

## Output tables and materialized views

### Raw extraction tables

```sql
CREATE TABLE eqt_nlp_extractor_run (
  run_id TEXT PRIMARY KEY,
  extractor_name TEXT NOT NULL,
  extractor_version TEXT NOT NULL,
  started_at TIMESTAMP NOT NULL,
  completed_at TIMESTAMP,
  code_hash TEXT,
  config_hash TEXT,
  status TEXT NOT NULL
);

CREATE TABLE eqt_nlp_fact_candidate (
  candidate_id TEXT PRIMARY KEY,
  run_id TEXT NOT NULL,
  doc_id TEXT NOT NULL,
  fact_type TEXT NOT NULL,
  subject_entity_id TEXT,
  subject_text TEXT,
  predicate TEXT NOT NULL,
  object_entity_id TEXT,
  object_text TEXT,
  payload_json TEXT NOT NULL,
  evidence_text TEXT NOT NULL,
  evidence_start INTEGER,
  evidence_end INTEGER,
  confidence DOUBLE NOT NULL,
  confidence_breakdown_json TEXT NOT NULL,
  publish_status TEXT NOT NULL,
  created_at TIMESTAMP NOT NULL
);
```

### Published relationship fact table

```sql
CREATE TABLE eqt_relationship_fact (
  fact_id TEXT PRIMARY KEY,
  subject_entity_id TEXT NOT NULL,
  predicate TEXT NOT NULL,
  object_entity_id TEXT,
  object_text TEXT,
  product_text TEXT,
  percent_value DOUBLE,
  money_value DOUBLE,
  currency TEXT,
  period_start DATE,
  period_end DATE,
  effective_start DATE,
  effective_end DATE,
  source_doc_id TEXT NOT NULL,
  source_section_id TEXT,
  evidence_text TEXT NOT NULL,
  confidence DOUBLE NOT NULL,
  direct_or_reverse TEXT NOT NULL,
  extractor_version TEXT NOT NULL,
  knowledge_from TIMESTAMP NOT NULL,
  knowledge_to TIMESTAMP
);
```

### Published customer concentration table

```sql
CREATE TABLE eqt_customer_concentration_fact (
  fact_id TEXT PRIMARY KEY,
  company_entity_id TEXT NOT NULL,
  customer_entity_id TEXT,
  customer_raw TEXT,
  customer_group_flag BOOLEAN NOT NULL,
  metric_type TEXT NOT NULL,
  percent_value DOUBLE,
  max_customer_percent_lt DOUBLE,
  fiscal_year INTEGER,
  period_end DATE,
  no_major_customer_flag BOOLEAN NOT NULL,
  source_doc_id TEXT NOT NULL,
  evidence_text TEXT NOT NULL,
  confidence DOUBLE NOT NULL,
  knowledge_from TIMESTAMP NOT NULL,
  knowledge_to TIMESTAMP
);
```

### Published event table

```sql
CREATE TABLE eqt_event_fact (
  event_id TEXT PRIMARY KEY,
  event_type TEXT NOT NULL,
  primary_entity_id TEXT NOT NULL,
  related_entity_id TEXT,
  product_text TEXT,
  location_text TEXT,
  event_date DATE,
  event_period_text TEXT,
  sentiment_score DOUBLE,
  severity_score DOUBLE,
  novelty_score DOUBLE,
  source_doc_id TEXT NOT NULL,
  evidence_text TEXT NOT NULL,
  confidence DOUBLE NOT NULL,
  extractor_version TEXT NOT NULL,
  knowledge_from TIMESTAMP NOT NULL
);
```

### Published shipment edge table

```sql
CREATE TABLE eqt_shipment_edge_fact (
  fact_id TEXT PRIMARY KEY,
  shipper_entity_id TEXT,
  consignee_entity_id TEXT,
  shipper_raw TEXT NOT NULL,
  consignee_raw TEXT NOT NULL,
  product_description TEXT,
  hs_code TEXT,
  hs_confidence DOUBLE,
  origin_country TEXT,
  destination_country TEXT,
  port_lading TEXT,
  port_unlading TEXT,
  shipment_count INTEGER NOT NULL,
  weight_kg DOUBLE,
  teu DOUBLE,
  period_start DATE NOT NULL,
  period_end DATE NOT NULL,
  shipper_intermediary_score DOUBLE,
  consignee_intermediary_score DOUBLE,
  confidence DOUBLE NOT NULL,
  source_batch_id TEXT NOT NULL,
  knowledge_from TIMESTAMP NOT NULL
);
```

### Materialized views

```sql
CREATE VIEW eqt_dataset_major_customers AS
SELECT
  f.company_entity_id,
  c.canonical_name AS company_name,
  f.customer_entity_id,
  coalesce(cust.canonical_name, f.customer_raw) AS customer_name,
  f.metric_type,
  f.percent_value,
  f.max_customer_percent_lt,
  f.fiscal_year,
  f.period_end,
  f.no_major_customer_flag,
  f.confidence,
  f.source_doc_id,
  f.evidence_text,
  f.knowledge_from
FROM eqt_customer_concentration_fact f
JOIN eqt_entity c ON c.entity_id = f.company_entity_id
LEFT JOIN eqt_entity cust ON cust.entity_id = f.customer_entity_id
WHERE f.knowledge_to IS NULL
  AND f.confidence >= 0.90;
```

```sql
CREATE VIEW eqt_dataset_supply_chain_edges AS
SELECT
  subject_entity_id AS supplier_entity_id,
  object_entity_id AS customer_entity_id,
  product_text,
  predicate,
  confidence,
  direct_or_reverse,
  source_doc_id,
  evidence_text,
  knowledge_from
FROM eqt_relationship_fact
WHERE predicate IN ('SUPPLIER_OF', 'CUSTOMER_OF')
  AND knowledge_to IS NULL
  AND confidence >= 0.85;
```

## Deduplication and bitemporal behavior

The same fact will appear in multiple filings. Do not blindly append duplicates.

### Fact identity key

```text
fact_natural_key =
  fact_type
  subject_entity_id
  predicate
  object_entity_id or normalized_object_text
  normalized_product_text
  metric_type
  period_end
```

### Deduplication pseudocode

```python
def dedupe_candidates(candidates):
    groups = group_by_natural_key(candidates)
    result = []
    for key, rows in groups.items():
        rows = sorted(
            rows,
            key=lambda r: (r.confidence, source_priority(r.source_type)),
            reverse=True,
        )
        best = rows[0]
        best.supporting_candidate_ids = [r.candidate_id for r in rows[1:]]
        best.confidence = min(1.0, best.confidence + 0.02 * len(rows[1:]))
        result.append(best)
    return result
```

### Supersession rules

```text
same company, customer, metric, fiscal year, later 10-K/A:
  supersede prior fact if source amendment date is later

same relationship, later filing says relationship ended:
  set effective_end and knowledge_to on active edge

same relationship repeated in later filing:
  update last_seen_at, keep original effective_start if no contradiction

press release creates edge, later 10-K confirms:
  raise confidence and mark source_support_count
```

## Evaluation sets

Build small labeled sets by fact type.

```text
major_customer_sentences.jsonl        2,000 examples
supplier_sentences.jsonl              2,000 examples
competitor_sentences.jsonl            1,000 examples
contract_award_news.jsonl             1,000 examples
facility_sentences.jsonl              1,000 examples
shipment_product_descriptions.jsonl   5,000 examples
entity_link_mentions.jsonl            5,000 examples
```

Each example should include:

```text
doc_id
section
sentence/table row
mentions
entity links
accepted facts
rejected candidate facts
reviewer
review_date
```

### Evaluation metrics

```text
NER:
  exact span precision/recall/F1
  relaxed span precision/recall/F1
  label confusion matrix

Entity linking:
  top-1 accuracy
  top-5 recall
  auto-accept precision
  review rate

Relation extraction:
  candidate recall
  published precision
  relation F1 by predicate
  numeric payload accuracy

Dataset:
  row audit pass rate
  duplicate rate
  stale edge rate
  source coverage
  time-to-publication
```

### QA sampling

```python
def qa_sample(facts):
    return stratified_sample(
        facts,
        strata=[
            "fact_type",
            "source_type",
            "confidence_bucket",
            "extractor_version",
            "entity_link_status",
        ],
        n_per_stratum=25,
    )
```

### QA decision codes

```text
ACCEPT
REJECT_WRONG_ENTITY
REJECT_WRONG_DIRECTION
REJECT_WRONG_RELATION
REJECT_WRONG_NUMBER
REJECT_NEGATED
REJECT_BOILERPLATE
REJECT_AMBIGUOUS
NEEDS_ENTITY_ALIAS
NEEDS_NEW_RULE
NEEDS_TABLE_FIX
```

## Operational considerations

### Incremental runs

```text
SEC filings:
  poll submissions and daily index
  fetch new accessions
  re-run on amended filings
  preserve old extraction version output

Press releases:
  poll RSS/sitemaps/news feed
  canonicalize URL
  hash body
  avoid duplicate syndications

Shipment data:
  ingest daily/weekly drops
  normalize batch
  aggregate by period
  re-run entity resolution when alias table improves
```

### Reprocessing policy

Reprocess when:

```text
extractor version changes
entity alias table changes materially
section parser improves
table parser improves
source document is amended
QA rejects exceed threshold
```

Keep previous facts with `knowledge_to` rather than overwriting. This preserves what ats-eqt would have known at the time.

### Monitoring

```text
documents fetched per source
parse failures by form/source
section detection coverage
table parse quality distribution
mentions per document
entity auto-link rate
candidate facts per document
published facts per document
QA rejection rate
confidence distribution drift
top new unknown entity names
top unmatched customer/supplier names
```

### Failure modes to monitor

```text
filing parser silently drops tables
sentence splitter breaks on numbered item headings
generic NER labels product names as ORG
entity linker maps subsidiary to parent incorrectly
regex captures "no customer" as named customer
percentage is accounts receivable, not revenue
press release repeats partner boilerplate as new event
shipment edge is freight forwarder, not supplier
HS classifier overfits common words
```

## Validation and QA

Measure at three levels:

| Level | Metric | Target for v1 |
|---|---|---|
| Mention detection | span F1 by entity type | 85%+ on company names, lower acceptable on products |
| Entity linking | top-1 accuracy, ambiguous rate | 95%+ auto-accepted, queue the rest |
| Relation extraction | precision/recall by predicate | 90%+ precision for published high-confidence rows |
| Dataset quality | sampled row audit pass rate | 95%+ for customer concentration |
| Timeliness | document-to-fact latency | same day for SEC, daily for news/releases |

Keep a human review queue for:

- Low confidence entity matches.
- Private companies with common names.
- Foreign names with transliteration variants.
- Relationship sentences with multiple entities.
- Strategic partner language.
- Shipment rows involving freight forwarders, NVOCCs, or trading companies.

## Recommended implementation roadmap

### Phase 1: high-precision filing extraction

- Build SEC document ingestion from submissions + archive documents.
- Parse 10-K/10-Q sections and tables.
- Load SEC company/ticker/CIK universe.
- Implement company alias normalization and TF-IDF char n-gram matching.
- Extract major-customer and customer-concentration facts.
- Store evidence spans and confidence.

Deliverable: `major_customers_us_equities` dataset.

### Phase 2: relationship graph from public disclosures

- Add Item 1/1A relation patterns for customers, suppliers, competitors, partners.
- Add press release and 8-K/exhibit ingestion.
- Add weak supervision and linear relation classifiers.
- Add direct/reverse disclosure handling.
- Add QA sampling reports.

Deliverable: `company_relationships_public_disclosures` dataset.

### Phase 3: shipment graph

- Acquire legally usable BOL/customs data or start with a licensed/sample/public subset.
- Normalize shipper/consignee names and addresses.
- Classify product descriptions and map HS/HTS when available or inferable.
- Aggregate `SHIPS_TO` edges by period/product/port/country.
- Link shipment edges to disclosed company graph.

Deliverable: `shipment_edges_us_maritime` dataset.

### Phase 4: news/release event stream

- Add event detectors for supply-chain disruptions, contract awards, recalls, sanctions, facility changes, guidance, and product launches.
- Use Loughran-McDonald and event-specific dictionaries for tone/risk.
- Add novelty/repeat detection with headline/body similarity.

Deliverable: `company_event_facts` dataset.

## Design principles

- **Prefer high precision over high recall at launch.** False supplier/customer edges are expensive for users.
- **Every fact must be source-backed.** No evidence span, no published fact.
- **Separate disclosed relationships from inferred shipment relationships.** They answer different questions.
- **Separate raw names from resolved entities.** Raw strings are valuable and help debug resolution.
- **Make the system bitemporal.** Store filing dates, retrieval dates, period dates, extraction dates, and validity dates.
- **Version extractors.** Facts should be reproducible under the extractor version that created them.
- **Keep a human correction loop.** Corrections should become aliases, blocking rules, and labeling functions.

## Practical low-cost stack

```text
Document parsing: lxml, BeautifulSoup/html5lib, sec filing SGML parser, pandas tables
XBRL: SEC companyfacts/companyconcept APIs, raw XBRL parser as fallback
NLP: spaCy tokenizer/matcher/EntityRuler, custom NER, sklearn-crfsuite
ML: scikit-learn logistic regression/SVM, lightgbm/xgboost optional
Weak supervision: Snorkel or skweak
Entity matching: rapidfuzz, char n-gram TF-IDF, libpostal/usaddress for addresses
Storage: ats-core tables plus raw document/evidence blob store
QA: sampled audit UI, confusion reports, precision dashboards
```

## Risks and limits

- **SEC filings under-disclose private supplier networks.** Major customers are more consistently disclosed than suppliers.
- **Vendor shipment data is noisy.** Freight forwarders, trading companies, shell importers, alternate names, confidentiality requests, and missing air/land coverage can distort true supply chains.
- **Names are unstable.** Mergers, subsidiaries, rebrands, local-language names, and ticker changes require alias history.
- **Relation language is asymmetric.** "A supplies B" and "B relies on A" imply the same edge but have different syntax.
- **Percentages need context.** A percentage may refer to revenue, accounts receivable, purchases, segment revenue, or total assets.
- **Press releases are promotional.** Treat them as evidence, but score lower unless confirmed by filings or shipment behavior.
- **Quantified dependency is hard.** Start by publishing relationship and evidence; add exposure estimates only when backed by percentages, shipment volumes, or modeled confidence intervals.

## Bottom line

The ats-eqt opportunity is not to imitate a chat model. It is to build a transparent extraction factory:

```text
public documents + section parsers + dictionaries + NER + entity linking
+ relation rules + weak supervision + QA + point-in-time evidence
= auditable fundamental and supply-chain datasets
```

This can produce useful datasets before any LLM is needed. LLMs can remain optional for offline QA, label suggestion, or hard-case review, while the production extractor stays cheap, deterministic, explainable, and source-linked.

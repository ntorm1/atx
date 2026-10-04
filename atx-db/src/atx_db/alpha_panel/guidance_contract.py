"""Frozen Task10 guidance economics, clocks and independent evidence interface."""
from __future__ import annotations
import hashlib,json
CONFIG={
 "version":"guidance-native-v2-frozen-1","stage":"events_guidance_v2","schema":"atx.events.guidance/v2",
 "parser":"legacy guidance-rules-v1 candidates plus strict independent native qualification",
 "target":"exact source FY/FQ/period type/end and historical fiscal_calendar_id; never inferred date",
 "candidate":{"primary_feature":"guid_surprise","measure":"EPS","economic_sign":1,"selected_before_actual_values_or_return_results":True},
 "basis":"explicit GAAP/ADJUSTED and independently identified accounting definition/scope accounting_basis_id; currency USD; EPS diluted/basic independently identified; revenue consolidated",
 "actual":"current native Task9 GAAP diluted EPSFI/consolidated quarterly revenue with independently pinned original source_value equality, exact historical fiscal identity and accounting/share bridge; unsupported adjusted/basic/annual/FPI bridges NULL",
 "clocks":"original qualified SEC acceptance; evidence public clock <= original issue; daily strict preceding calendar session22 UTC",
 "values":"source low/high only for range or one-sided bound; source point only for point quote; separately derived midpoint for complete range/point",
 "prior":"latest strictly earlier comparable original guidance for exact target/basis/currency/unit/share/calendar identity; equal-clock ambiguity missing; never text-prior fallback",
 "guid_revision":"current qualified midpoint minus prior qualified midpoint, native units; floor/ceiling no midpoint",
 "guid_surprise":"EPS actual minus prior midpoint divided by independently qualified comparable per-share price; revenue surprise NULL with unit_mismatch_total_revenue (R22)",
 "precision":{"threshold":0.90,"required":300,"selection":"lowest SHA256(cik/accession/task10-precision-300-v1), ALL original eligible releases2020-2025 independent of parser success; missing retained"},
 "coverage":{"years":list(range(2020,2026)),"year_axis":"original SEC filing_date; never backdates available_at","population":"independently pinned member issuer-year inventory; guiding issuer numerator with qualified nonnull guidance"},
 "limits":{"document_bytes":8*1024*1024,"candidates_per_document":256,"documents_per_release":64,"rows_per_issuer":10000,"batch_rows":512},
 "price_denominator":"last completed admitted exchange-session close strictly before original actual issue; genuine availability and comparable currency/share basis; no reaction price",
 "evaluation_end_exclusive":"2023-01-01","complete_forward_labels":"none; no return evaluation supplied",
 "source_quality_certified":False,"readiness":"NOT_PRODUCTION_READY"}
def digest(value):return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(",",":"),default=str).encode()).hexdigest()
CONFIG_SHA256=digest(CONFIG)

# P9 A0 local recovery candidates

The vendor source bytes are retained: the Downloads `TickerHistory3.parquet`
(3,617,973,507 bytes) streams to SHA256
`0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae`,
exactly the original r4 input pin. It remains at its existing path; no copy,
hard link, restored original path or replacement input receipt was created.
The original source's receipt clock remains the published descriptor's clock,
not this new observation time or the Downloads file's timestamp.

The complete 367 document SHA names were checked at the native plain/gzip
content-addressed locations in four existing stores: `sec_events`,
`sec-earnings-release`, `sec_issuer_history/round2`, and
`sec_universe_hand_check/round2`. None had a candidate there. This is a bounded
negative lookup, not an exhaustive search of all possible caches or backups.
No HTML/gzip body or ledger was read, and no network request occurred.

`p9-a0-recovery-candidates-v1.py` wrote
`p9-a0-recovery-candidates-v1.json`. Its receipt
`logs/p9-a0-recovery-candidates-v1.receipt.json` completed exit0/8 seconds at
30,564,352 peak job bytes, under unchanged0.15GiB and40GiB disk guards; no cap
hit or override. Minimum observed commit/physical headroom was3.645/2.7236GiB.
The complete original build hash, before/open/final vendor identity, stream
length, and final path states were checked. The single streamed digest is a
recovery lead; this audit does not prove global quiescence or source quality.

The source bodies required by the original document-review pin remain a concrete
A0 rebuild dependency. Do not replace the pinned review with later assertions,
regenerate its body hashes from extracted text, infer bodies from filing metadata,
or silently substitute a later vendor source. The earlier retained-provenance
metadata review passed independently; this separate recovery audit does not
change any D0, arm, coverage, predictive or consumer gate.

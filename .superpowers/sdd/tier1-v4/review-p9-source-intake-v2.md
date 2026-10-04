# P9 bounded source capture and clock diagnostic review

Spec PASS; quality PASS for the captured inventory and non-admission diagnostic.
No findings requiring correction within this scope. This verdict does not admit
NYSE calendars, CPI, market/index products, source clocks or any acceptance gate.

The intake code fixes four non-SEC URLs, disables redirects/retries/environment
proxies, reserves every attempt in a flushed/fsynced ledger before sending, keeps
both success/error bodies, refuses prior output/landing and bounds decoded response
size2MiB. Actual recorded starts are separated by at least1.05s. The four reserve/
outcome pairs exactly equal the final inventory: two Nasdaq200 captures and two
BLS403 access-denied bodies, all qualified=false. Failure captures are not CPI
observations; retrieval/header dates do not become historical publication proof.
Original metadata guard completed0/7s/37,982,208B, no cap. No capture was rerun.

The two literal Nasdaq publication labels are present exactly once in the pinned
raw inputs: Dec28,2020 3pm EST ->20:00UTC, and Dec27,2021 9:30am EST ->14:30UTC.
The parser requires a single Published label, explicit EST/EDT, New York timezone
agreement and a valid UTC round trip. Complete selected holiday-table cells and
all matching early-close paragraphs are compared with whitespace normalization;
no numerical fit or equality declaration replaces original input parsing.
Source body size/SHA checks surround that comparison in the inspected code.

The preserved v1 failed guard is failed1/1s/30,396,416B, no cap. Its traceback
refuses original/distributed provenance or table mismatch before output; it is
not misreported as success. V2 explicitly diagnoses rather than waives that
refusal: 2020 has the same canonical Business Wire link, 2021 has no ICE Business
Wire link and records same_explicit_source_version=false. Both factual tables
compare equal, with10/11 rows, and early-close paragraph counts2/2 and3/3.
V2 retains the original failed code/logs, keeps ICE publication clocksNULL and
calendar_admitted=false for both years, network_attempts0 and gate_promotion=false.
Its completed0/1s/31,137,792B guard confers only diagnostic completion. Full dated
schedule/amendment corpus, weekday authority, original source rights, complete
native parser/replay and reviewed consumer qualification remain outstanding.

Covering read commands: full scripts intakev2/preflightv1/v2 (exit0,1.56s);
metadata JSON/three exact receipts plus preserved v1 traceback (exit0,1.56s);
named literal-label/link/access-denied reads of the six already pinned small HTML
inputs and eight-line intake ledger comparison (exit0,1.78s). No preflight/capture
rerun, test, source-hash scan, network request, native product job or index action.

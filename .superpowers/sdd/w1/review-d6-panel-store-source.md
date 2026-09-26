# Independent D6 panel-store source review

Reviewed core `98ad63d5` + correction `97a00229`, and adapter `6e44a6da` via frozen Git objects. Read-only: no source edits, compiler, tests, actual payloads or market data. Core source verdict: APPROVE for compilation qualification. Adapter verdict: HOLD for the source-axis/sealed-source corrections already assigned by root; no additional blocker found in this pass.

Core findings checked:

- Geometry limits bound arithmetic before layout/chunk allocation. Metadata/chunk/artifact ceilings and writer admission are explicit. Mapping uses the opened handle's captured extent, checks exact expected bytes and maps that admitted length.
- Copies of PanelStore share a mutex-protected byte/handle budget. Chunk lifetime independently owns the read-only mapping and budget. Failure releases reservations, and the destructor releases OS resources before making a reservation available. Borrowed mask spans are documented as chunk-lifetime views.
- Present and tradable bits remain independent. A missing source row cannot erase membership; tradability requires a positive decision clock strictly earlier than the session. Numeric absence is canonical NaN and never fabricated zero.
- `returns` storage is exact-f64. The separate original-f64 adjusted close is positive finite or missing; forward_returns enforces endpoint < maturity_end and computes the original double ratio, releasing the first chunk before mapping another. It does not silently apply delisting imputation or claim source-vintage proof.
- The immutable manifest binds axes, field basis/precision, source indices, recipe, parents and chunk hashes; publication is last and refuses replacement. Reader validates numeric/mask/clock invariants before exposing a chunk.

Adapter findings checked:

- Complete membership-artifact union provides a stable numeric column axis, retaining absent and later-entering names. Dated membership uses the last effective rebalance with rank < session. Presence preserves rolling warm-up independently.
- The bounded window adapter uses original-f64 close, widens other stored fields and returns separate tradable/basis descriptors. Returned data own their memory. Derived identity binds the parent store, selection and read rule. Generic legacy pipeline reads/writes refuse implicit loss of the separate mask.
- Chunk assembly carries preceding warm-up for ADV/one-day returns and explicitly discloses archive-snapshot availability and unverified vintages. Floating-point rank-IC/large-union RSS, exact original year-window overlap and full D6 acceptance remain unqualified.

Open adapter blockers (already owned by G0 following root review): a reopened segment can change its timestamp axis after history_session_keys, making an unchecked lower_bound result an out-of-range cell write; source selection must enforce known sealed filenames before mapped CRC/payload access; repeated per-chunk full-source attach/CRC walks need a once-admitted source descriptor path. I independently observed the first issue and did not duplicate the owner's correction work. Approval of the combined adapter awaits the exact correction freeze and focused runtime evidence. Synthetic core/adapter test source was inspected, not executed.

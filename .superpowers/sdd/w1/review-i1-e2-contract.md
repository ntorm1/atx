# Independent I1/E2 catalog contract review

Date: 2026-09-26. G0 lane, pool-3. Proposal reviewed directly with the pool-5
owner; implementation was not frozen at this checkpoint. This is a design
review, not source, runtime, import-completeness or full I1 acceptance.

The proposed bounded append-only metadata catalog is coherent. It separates
alias-independent numerical cells from document/family lineage and individual
measurement attempts. A durable reservation precedes family VM evaluation.
Completed and failed terminal events never erase the reservation; a valid
reservation without a terminal remains incomplete. Failed/incomplete work still
contributes its unique numerical configurations. No PnL, IC-derived PnL, sketch
or cluster correlation is fabricated.

The owner proposes an exclusive OS lock, framed SHA-chained records, explicit
file/event/cell/attempt/working budgets, and a required externally supplied
expected head. The zero head is permitted only for an empty new catalog.
Same-token exact reservation requests are idempotent; conflicting requests or
terminal outcomes are refused. The stage refuses to execute an already reserved
attempt again. A truncated final frame preserves bytes and fails closed; it is
different from a complete pending reservation. No automatic truncation/recovery
or historical data import is authorized by this review.

## Concrete implementation review points sent to the owner

1. Construct the cell recipe from an explicit numerical/data/window whitelist:
   canonical DSL, sign, one horizon, forward variant, restriction and effective
   calculation/input recipe. Exclude aliases/themes, whole prereg hashes,
   checkpoint/output paths and provenance-only source versions from the cell
   key. Store those separately in attempt/proof metadata. A selected numerical
   implementation rule remains part of the recipe even when provenance is not.
2. Retained lineage must prove each exact prior reserved cell together with its
   prior family, prereg and trial reference. Finding an unrelated prior family
   hash is insufficient. Current `prereg.cpp` at root `5acae159` includes
   id/name/theme in the family configuration SHA and compares a retained prior
   hash to the current one. Preserve prior proof fields separately if retained
   aliases are supported; otherwise explicitly disclose the retained-rename
   restriction. Unique numerical cells still deduplicate aliases.
3. Avoid a manifest/terminal-head hash cycle. The immutable run manifest can
   bind the reservation head, and the completion event can bind that manifest's
   hash. Publish the refreshed external head anchor separately; do not rewrite
   an already hash-bound manifest to insert a later terminal head.
4. Cover every post-reservation failure seam, including legacy preregistration,
   VM work, output writes and final publication. Append a failed terminal when
   writable; otherwise retain the incomplete reservation as proof. Keep the
   two-store failure boundary explicit. A completed artifact with an incomplete
   catalog event is not permission to execute the same token again.
5. Counts must distinguish prior/imported unique cells, new epoch unique cells,
   their verified union, attempt count, and completed/failed/incomplete attempts.
   A retry contributes no new unique cell, but a genuinely new attempt remains
   observable. Missing historical evidence must not be described as an empty
   historical population. Cluster/observation coverage is separately unavailable
   until compatible observations or justified mappings are actually supplied.

## Original acceptance still open

The original DAG W1-I1 requires all prior cp14-cp22/L9/L10 sidecars, cumulative
cluster-N, all-stage registry handles/manifests and a single shared guard.
The proposed initial equity-ic catalog slice does not establish those claims.
The prior evidence inventory remains `.superpowers/sdd/w1/i1-preparation-codex.md`.
Its declared 570+2065+120 starting sum is not a deduplicated or cluster N.

No files outside this review note were edited. No registry was opened, no
warehouse/payload was read, and no build or numerical run was launched.

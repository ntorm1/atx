# Issuer query integration - 2026-09-22

Applied reviewed final draft ca74a08c3e917363e1c083c0e433edc0cd017f883c55c2af1a57ce260947ca74
after archive10 terminal recovery/checkpoint. Root corrected only unused
imports/import spacing in the live integration (Ruff --fix).

Guarded focused tests: 5 passed in2.87s, peak0.602GiB.
Existing API service/commercial regression:14 passed in155.53s, peak0.952GiB;
one existing Starlette/httpx deprecation warning. Scoped Ruff reports all five
fixable import issues repaired. No full-suite gate has run.

Critical derived-owner collision review and fixture repairs are closed in
issuer-content-query-critical-rereview.md. NULL invalidation and exact CIK
isolation are tested. Live warehouse downstream materialization remains pending.
The persisted API catalog in an already-upgraded warehouse still needs an
additive reseed migration; isolated task issuer_catalog_upgrade owns0322,
after source0320/core0321 registration. Do not claim live production API
catalog readiness until that migration and measured queries pass.

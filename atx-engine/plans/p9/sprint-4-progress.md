# Sprint 4 progress — Capacity + Turnover as First-Class NSGA Objectives

Ledger convention: append one line per clean review-gated unit.

- S4-0: enum/constant append (kMaxObjectives 7->9; kObjCapacity=7, kObjTurnover=8,
  APPEND-ONLY frozen-prefix) + inert-default gate fields on SearchConfig/FitnessCfg +
  report projections on FitnessReport + compute carriers on FitnessCore. Additive only,
  no compute yet. Golden `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` unmoved. NOTE:
  growing kMaxObjectives widens the on-disk `--resume` checkpoint record (search_progress.hpp
  sizes off kMaxObjectives) — a pre-S4 checkpoint is not expected to resume byte-compatibly,
  the same accepted, precedented consequence as the novelty (3->4) and dsr (6->7) width bumps.

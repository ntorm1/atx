# Reported quarterly EPS source integration - 2026-09-22

The source stage is integrated after the archive10 recovery/checkpoint. It
loads bounded, cached SEC Item2.02 EX99 evidence using the typed filing-detail
index; rejects ambiguous earnings bases; preserves reported fiscal periods,
raw acceptance strings and conservative daily clocks; and atomically records
accepted source facts with their receipts. Malformed indexes and failed fetches
remain retryable. Migration0320, its registry entry, dataset/job, activation
stage, CLI and readiness inventory land together. No raw CompanyFacts owner or
fingerprint changes are part of this task.

Integration sequence from the original draft:

- BAC6C8DE5808B5FCF2897E2AB3DADD2F962F1D808762309F07A787F900C0B170: complete source draft.
- 5B6332C1AF3EE7E5BA6115F64010B5DECBE89DF8035D707BEFC3430BC4663A23: typed SEC index and fixture/lint corrections.
- FD9B24F274C7EC56F09E54BAD9D802B3ED8EE718E3C2F6D800FEABD11E0DA93C: malformed-index retry distinction.
- A8501F28A73B0DA13971ACDE83291CB120B3CDCF7CA3F29F2B366B8582FFCB73: valid SEC complete-submission row with blank Seq/Type.

Root also sorted the new bulk-test hashlib import. All Critical findings are
closed in reported-eps-source-critical-rereview.md. Earlier rejected artifacts
are retained as history; do not reapply them to the integrated files.

Validation used one2GiB guarded runtime at a time. Initial combined source,
bulk and existing press-release tests had28passes and3mistaken source fixtures:
two expected48hours despite the46hour policy, and one lacked a comparative
header span. Those were corrected without weakening source qualification.
Final source tests:22passed in4.86s, peak0.679GiB. Three activation registration
and stage-signature checks passed in1.17s, peak0.586GiB. The unchanged bulk and
existing press-release tests had no failures in the combined run.

The complete frozen official CVX index selects a12312025ex9918-k.htm, including
its blank complete-submission summary row. The official exhibit parser selects
Q42025 dilutedEPS1.39, prior1.84 and Dec31 period end. These are decoded UTF8
reference checks, not production receipts or warehouse writes. Peaks were
0.569GiB for final discovery and0.571GiB for extraction.

Scoped new/other changed-file Ruff checks pass. Comparison to92cf42c4 shows no
introduced findings in legacy jobs.py or press_release.py; respectively4 and11
preexisting diagnostics remain. The full non-slow suite is still reserved for
the sprint gate. Live source loading, core0321 integration, persisted catalog
0322, downstream materialization, measured coverage and release remain pending.

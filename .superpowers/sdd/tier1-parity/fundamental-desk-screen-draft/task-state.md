# Task state

- Scope: static fundamental desk SQL acceptance only; isolated draft ownership.
- Prepared: `atx-db/sql/research/fundamental-desk-screen-acceptance.sql` and
  `atx-db/docs/FUNDAMENTAL_DESK_SCREEN_ACCEPTANCE.md` in this draft tree.
- Integration: `integration-final.patch`, normalized `a/atx-db` and `b/atx-db`
  new-file paths; `git apply --check` passed.
- Runtime: not executed. No measured membership, metric, price, or pass counts.
- Review: independent static review received; its five findings are addressed
  in `fix-report.md`. No second review is required absent a new critical issue.
- Next: root applies patch, runs the SQL after materialization, then uses the
  observed diagnostic to make only a bounded generic source/implementation fix.

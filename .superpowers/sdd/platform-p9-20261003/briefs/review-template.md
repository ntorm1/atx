## Adversarial review (one per lane head, read-only; paste with the lane id and SHA)

**You review lane `<id>` at exact SHA `<sha>` (base `<frozen-sha>`).** Read the lane's brief (above), its report, plan
§0.6 and §2.2, `.agents/cpp/agent.md` §10 and `.agents/harness/TEMPLATES.md` "Review". Read the diff with
`git -C C:/atx-wt/pool-2 diff <frozen-sha> <sha>` and files with `git show <sha>:<path>`; edit nothing, build nothing,
run no real data, open no 2020-2023 output and nothing dated 2024-01-01 or later. Re-run at least one of the lane's
pytest commands inside the lane's pool with `PYTHONDONTWRITEBYTECODE=1` and `-p no:cacheprovider` (no file written) and
paste its exit code and output tail as your evidence.
Look for: flag-absent identity broken (a default changed, an output key added unconditionally, an iteration order
changed); look-ahead (a window ending at d instead of d-1 / d-2; a label or return read before it matures); seal (any
path or row at or after 2024-01-01 decoded); a Python copy of the C++ rule kept or added; files outside the brief's
scope; `/W4 /WX` hazards (unused variables, sign conversion, shadowing, missing includes, 100-column limit); lifetime,
bounds and error paths; determinism (unordered iteration, thread-order writes); tests that are tautologies (YCOMB's
two-speed closed form at fixed L, `review-ycomb.md:24`) or that pin the implementation instead of the contract;
registry rows that disagree with the code; contract drift from K-P9-n.
Write `.superpowers/sdd/platform-p9-20261003/task-<id>-review.md` in the TEMPLATES "Review" shape (verdict APPROVE or
BLOCK; findings `path:line | severity | problem | required fix`) and return it to the PM in at most 12 lines. APPROVE
with a blocker or major open is invalid.

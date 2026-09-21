# VR1 packaging fix

The original hand-authored `integration.patch` remains preserved as evidence.
It failed `git apply --check` at the first source hunk because its sparse
manual context was not robust against the baseline's line endings.

`integration-v2.patch` was regenerated from the exact live baseline and frozen
draft copies with Git's normal five-line unified context. It uses CRLF for the
`fundamentals.py` hunk and LF for the test hunk because those are the baseline
files' respective line endings. Its SHA-256 is:

`CD9CB33AFE77F9A718A39C28BB93A85079C458245DAD264D7CC08C1DDBA0A3C2`

The subsequently regenerated `integration-final.patch` includes the approved
test-only review repairs and retains the same standard-context/EOL packaging.
Root should use that final patch:

```powershell
git apply --check .superpowers/sdd/tier1-parity/verified-resume-recycling-draft/integration-final.patch
git -c core.autocrlf=false apply --check .superpowers/sdd/tier1-parity/verified-resume-recycling-draft/integration-final.patch
```

Both checks passed during static packaging verification. No patch was applied,
and neither live source nor either frozen draft source copy was changed.

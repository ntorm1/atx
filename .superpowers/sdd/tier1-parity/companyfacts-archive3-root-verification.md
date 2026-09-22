# CF5 root verification

Root accepted both Important findings on the implementer's fix report and focused
execution, as requested by the program. The independent review found no Critical
issues; no second independent review was commissioned.

- Initial nine-file run: 125/126 passed. All 42 new CF5 cases passed. The one
  existing activation test inherited the explicitly configured dummy User-Agent;
  root made its missing-environment precondition explicit with monkeypatch.
  That single case then passed.
- After Important fixes, the two CF5 files plus that environment-isolation case
  passed all 53 cases. Receipt `companyfacts-archive3-fix1-focused-memory.json`
  records exit 0 and peak job memory 0.7037620544433594 GiB under a 2.5 GiB cap.
- Ruff passed all six touched source/test paths. Strict mypy passed the new
  `_companyfacts_resume.py` module, using the repository configuration.
- Root used the project Python and only one guarded workload at a time. No live
  warehouse writer ran during verification. All SEC contact configuration used
  the dummy research address.

The accepted fixes cover all-owner point multiplicity, exact source receipts,
repeated newest-UUID resumes with durable source-incomplete outcomes, and invalid
or absent marker rejection. Root also read the resulting bounded aggregate proof.
This is fixture validation, not a claim that the full archive will complete at
1 GB. Production proof and ingestion remain to be measured under that budget.

Commit scope: fundamentals.py, _companyfacts_resume.py, activation.py, the two
CF5 test files, the existing activation environment-isolation test, and these
CF5 reports/evidence. AF1 and its migration registry changes are excluded.

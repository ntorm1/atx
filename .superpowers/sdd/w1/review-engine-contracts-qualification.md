# Independent receipt audit: W1 engine contracts

Reviewed report `16c962930ef590981635432b0acda0770786597d` against the
seven `w1-*-qualified` XML/receipt pairs in root pool2 `build-equity`, current
target binaries, D1 Python logs and the fixture repair Git diff. No tests rerun.

Verdict: **approve the bounded qualification evidence as reported**.

- Reconstructed the union by XML `(classname,name)`, replacing only the corrected
  library fixture result: **283 unique C++ checks, zero final failures, zero skips**.
  Original counts are cost35, library37, data19, eval72, IC64 and impl56; the
  one-test repair is not counted as an extra distinct check.
- All seven XML SHA256 values match the report. All receipt binary hashes match
  the report. The six current binaries match their applicable successful receipts.
  The initial library executable has been replaced by its corrected binary, so
  its old hash is supported by the preserved runtime receipt rather than a second
  still-existing executable. The initial XML and exit1 remain preserved honestly.
- Initial receipts pin `d9031039601a9ed12ff4291f0e5b9708cb2246ad`; the repaired
  fixture pins `0f3ace263d000c8bb052815902de693dfc4d34b1`. Their Git diff changes
  exactly two lines in one library test: bind the explicit index recipe before
  staging records. No production guard or other test behavior changed.
- Native XML timings and receipt exits agree with the report, including the
  initial 36/37 library result and its subsequent 1/1 repair. The integrated
  build receipt records native0, Jobs1 and 646.203s; these are shared-host evidence,
  not a controlled performance comparison.
- The two preserved D1 Python logs end `Ran 15 tests / OK` and `Ran 7 tests / OK`,
  matching the additional 22 reported checks. PowerShell's stderr wrapper text in
  those logs is not a Python test failure; the unittest terminal results are green.

Approval is limited to the implemented contracts and reported target filters.
New B1 spread/FIM/borrow, date-CPCV, D5 historical acquisition, production-scale
gates, broad IC recall/pruning and the deferred 81-case benchmark are not qualified
by these receipts. No alpha admission or tradeability conclusion follows.

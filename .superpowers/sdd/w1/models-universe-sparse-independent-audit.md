# Independent bounded qualification audit

Reviewed root report `dbb16af3` on 2026-09-26 from pool-3. **Approved for
the bounded result claimed:** 262 distinct C++ checks pass after replacement
of the single repaired cutoff case. No unresolved failures or skips remain.
This is a read-only artifact/source audit; no tests, builds or data runs were
repeated.

Artifacts are under `C:/atx-wt/pool-2/build-equity/`, with prefix
`w1-models-<name>-qualified` and suffixes `.xml`, `.log`, `-receipt.json`.
XML testcase identities were combined by `(classname, name)`, replacing only
the matching cutoff result. Receipt native exits, source/filter, wall times,
XML hashes and the five current executable SHA256 values were inspected.

| Name | XML passes / cases | XML seconds | Native exit | Binary binding |
|---|---:|---:|---:|---|
| cost | 46/46 | 0.846 | 0 | Current executable matches |
| data | 60/60 | 0.083 | 0 | Current executable matches |
| cpcv | 13/13 | 0.041 | 0 | Current executable matches |
| risk | 74/74 | 14.802 | 0 | Current executable matches |
| impl | 68/69 | 9.264 | 1 | Historical receipt only |
| cutoff | 1/1 | 0.001 | 0 | Current application executable matches |

All six XML hashes and all five current binary hashes equal the report's
Artifact bindings table. The initial application's hash
`c63021c92772f1f6be1d7a9251f32d6f807c244b91842d4e70d74053de08ad15`
cannot be rehashed from its replaced executable; it is supported by its
historical receipt. The current application hash is
`134e3e28e254d75c57e8a57cede61108394f5bf78d18a6a6a5e4cccae9519636`.

Receipt SHA256 bindings, in the same order:

```
cost   4b25d28df1d508f9d0c44708843027354a3e8d1bc74bce370d037b4a1c50ed56
data   4884cc13f81dbee3cc5d979a1adf3e55d16a18168f2b58d138d9dee14d14149b
cpcv   bf1bfd5a7c921aec1b364468d05cf4bec52719aa0053b97b9496ef113f4bd393
risk   b88be2f5bcaa3df44af7e366b77cce5aedaa30567b6d02128c7af8aef629f38a
impl   c672531b13da0f1258ed23313be961378845f21210fa8b480645721d25b1cbc0
cutoff e9d19d1ef76583ad3a7d46ff0af3a4e89e563f5c6ea12332753b197fec0cc39c
```

The sole initial failure is
`ConfigEquityUniverse.RankEnd_After2019_Rejected`. Inspection of `402af294`
confirms a fixture-only dummy instrument-type path lets validation reach the
intended date guard before opening a path. `c42eb524` is also fixture-only;
neither changes production relative to `6debcc10`. The first five runtime
receipts identify `c42eb524`; the repaired one identifies `402af294`.
This audit verifies source differences and receipts, not an independently
decoded embedded Git SHA from each executable.

The build receipts support the reported native outcomes and wall times:
leaf 0/9.969s, initial 1/35.123s, resumed 1/209.852s, fixture 0/48.009s,
cutoff 0/17.575s. `w1-d5-integrated-qa-tests.log` reports seven synthetic
Python tests, 0.268s, `OK`; no independent Python exit receipt was found.
The report's process/memory observations were not reconstructed from a
continuous process trace and remain samples, not peak or speedup evidence.

No broader suite, numerical alpha, package-reference parity, historical
coverage, real artifact construction, or scale/RSS acceptance follows from
these checks. Prior qualification counts remain separate. The report
preserves these limitations and the overwritten-binary caveat explicitly.

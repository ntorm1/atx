# SEC filing index follow-up

Patch: `sec-index-retry-followup.patch`  
SHA-256: `A8501F28A73B0DA13971ACDE83291CB120B3CDCF7CA3F29F2B366B8582FFCB73`

The official CVX filing index contains a five-cell `Complete submission text file` row with blank Seq and Type cells. The previous validation rejected the entire index. This follow-up permits those blank values while retaining the five-cell `td` layout and one document anchor. EX-99 still requires an explicit fourth-column Type match before selection. The compact index fixture now includes that official row shape, so the existing selection, retry, and terminal no-EX-99 tests exercise it.

`git apply --check` passed against the current live files. This isolated follow-up did not run Python, tests, a database, or the network.

Current live source SHA-256 before patch: `E63DC68534580A4D61FA0070518B45B98C888E0C9662BF3EFFE077C022F8FF0D`  
Current live test SHA-256 before patch: `01BF1D7870469F9D5B9C72EFF6A288A828FC866FA1A90B0DB98694CDA52AA6EC`

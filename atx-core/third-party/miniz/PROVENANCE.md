# miniz vendoring

- Library: miniz (single-file PKZIP / DEFLATE reader/writer)
- Version: 3.0.2
- Source: https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
- Files vendored: `miniz.h`, `miniz.c` (the release amalgamation) + `LICENSE`.
  The `examples/`, `ChangeLog.md`, and `readme.md` from the release were discarded.
- Release zip SHA256: `ADA38DB0B703A56D3DD6D57BF84A9C5D664921D870D8FEA4DB153979FB5332C5`
  (verified via `Get-FileHash -Algorithm SHA256`).
- License: MIT (see `LICENSE` and the header of `miniz.h`).
- Why: read PKZIP archives (Databento batch `.zip`) without a heavy dependency.
  Compiled as its own static lib (`atx_miniz`) with warnings disabled, linked
  PRIVATE into atx-core so `miniz.h` stays confined to the `.cpp` TUs (and the
  test target), mirroring the `atx_sqlite3` vendoring pattern.

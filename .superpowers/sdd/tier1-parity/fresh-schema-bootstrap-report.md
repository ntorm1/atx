# Fresh schema bootstrap investigation

The reported migration 0280 dependency failure was not reproducible with the
locked project runtime. The earlier failure used system Python with DuckDB
1.5.1 and was not run under the required memory guard; it is invalid evidence.

The guarded isolated `git archive HEAD` baseline used
`C:\atx\atx-db\.venv\Scripts\python.exe` with DuckDB 1.5.5. It imported
`atx_db` only from
`C:\atx\.superpowers\sdd\tier1-parity\fresh-schema-head155-export-2\atx-db\src`,
configured DuckDB with a 1 GB memory limit and one thread before initialization,
and completed bootstrap through migration 313. `verify_schema` returned no
violations.

The 3 GiB job guard completed successfully with a native peak of 0.822 GiB.
The receipt is `fresh-schema-head155-memory.json`; runtime and export evidence
is in `fresh-schema-head155.log`.

No migration body, checksum allowlist, or test change is needed. The proposed
0280 repair and its checksum compatibility entry were withdrawn before commit.

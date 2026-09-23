# Fatal dataset failure ledger recovery: independent static review

Verdict: clean. No Critical or Important findings in the reviewed change.

`Dataset.run` captures the original load traceback, tries the ordinary failure-ledger update, and makes one bounded recovery attempt only if that update fails. The recovery path delegates to `DuckDBStore.recover_failed_connection`, which requires recorded memory and thread limits, closes the failed connection without a checkpoint, and supplies those limits in the new connection's opening config. The code then re-raises the original load exception. If recovery or the second update fails, it adds an operator note to that same exception.

The change does not call `load` or `initialize` again, change `rows_loaded`, or touch committed partial rows. The new test checks original exception identity, a single load and schema setup, close-before-reopen ordering, bounded reopen config, a failed ledger row with the original traceback and null `rows_loaded`, and persistence of an already committed row. Its invalidated connection is a controlled simulation of the post-COMMIT failure state; it does not reproduce DuckDB's actual OOM behavior.

Validation was static only, as requested. `git diff --check` passed for the reviewed paths. Runtime tests and warehouse inspection remain with root.

# Actual fundamental native freeze and extract recovery

Reviewed code189fe919 and independently reviewed binding8c63e1ba...6894b3 were
used for native build generationr17_20261004_001 with --max-new-batches0.
First guarded attempt ended18.0s/121,196,544bytepeak, exit1, no memorycap hit:
complete extraction inventory refused before snapshot or financial computation.
Logs t16-fund-freeze-v1 preserve the full failure.

Guarded layout audit2s/59,981,824bytes established v10extract256files including
85statistics sidecars, while v11extract171files omitted those85. Both plans have
SHA490dcb2f4632367dfc128b5ea8929a4e415ac7ed662e86f6b83910d18c46a447.
All retained sidecars match native statistical schema; their total is9,261,380bytes.

Recovery12.1s/46,833,664bytepeak/exit0 independently compared every171sharedfile
SHA/size before and after, verified each auditedsidecar, and exclusively created
only85missingdestinations with no overwrite. Actualsource/destinationpins match;
v11extractnow256files. This is retained-source evidence recovery, not after-the-
fact admission of oldfinancialbatches. StatsstillrequirefullsourceZIPreplay.
See task16-extract-layout-audit-v1.json and logs/t16-extract-stats-recovery-v1.json.

Second sourcefreezeattempt uses identicalreviewedcode/binding/generation and
--max-new-batches0 under unchanged.8GiBguard; rootexecsession42951 atlaunch,
logs/t16-fund-freeze-v2. This corrected-input retry is not a repeated identical
memory failure. NoFinancialunit/gate/sourcequality/productionPASS follows from
successfulsidecarcopy. Future status is recorded in HANDOFF/progress/guardreceipt.

## Actual second-attempt result

Session42951 ended `stopped_low_commit` after1502.7s, child returncode137.
Peak647,417,856 bytes (0.603GiB) was below the unchanged0.8GiB cap; no cap event.
Host free commit fell to0.3095GiB. No snapshot or financial unit was created,
and full native source replay did not complete. Do not repeat the whole replay
unchanged. R19/task-16-extract-resume-ruling.md routes bounded durable source
validation to Task16's final cumulative fix round5/5 with independent admission.

Dead writer lock23584 was preserved under `_interrupted_locks` with unchanged
SHA01f186fa9609dddec4a43fe278d3632af1232d1a97c7f777cd29280211988e7c.
The ended private replay/spill files were safely removed in cleanupv2; original
archive, full256 extraction inventory, all85 restored statistics and144 current
preparation units remain. Cleanup verification does not replace archive replay.

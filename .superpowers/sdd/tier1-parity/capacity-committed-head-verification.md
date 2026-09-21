# Capacity committed-HEAD verification

Root exported exact1a3cb68b5f9a2be25a6b9c9a5006fbe5eda74b18 with git archive
and imported only from that isolated source. Import succeeded; all module-boundary
and schema-contract tests passed, with one existing default slow skip. Exit0.
Guard2.5GiB, native peak0.951126GiB. Logs capacity-committed-head.log/.err and
receipt capacity-committed-head-memory.json. This covered committed PR1/FP1/
AP1/DP1/MP1 through0318 and excluded shared-tree WIP. No full-suite gate claim.
FC1/0319 is subsequent work and requires its own focused/contract verification.

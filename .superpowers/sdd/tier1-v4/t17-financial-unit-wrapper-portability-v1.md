# Retained audit success and PowerShell numeric portability

Root wrapper session61803 terminated1 after the successful first native audit.
The exact guard for batch3 completed0 in62.1seconds with557,080,576 peak bytes,
cap_hit=false and no stop reason. Its Python child exited normally after the
R23 runtime context's final check. The result is5,640bytes, SHA256
7fc2cb6c4ad286d084c8a0b5623fdf393f395125e271fce79f33a56a32c06546.

The wrapper's terminal message was "Original guard resource contract required"
at line27. Windows PowerShell ConvertFrom-Json produced System.Decimal for
the legitimate numeric0.8 memory limit. The reviewed wrapper accepted only
System.Double. All actual guard resource values matched their expected values.
This is an overly strict wrapper type check; it is not a native data failure,
memory-cap stop, increased limit or an unsuccessful guard counted as successful.

The original failing wrapper is preserved in commit b3f7c503. The narrow fix
accepts Double OR Decimal while still requiring exact numeric0.8 and refusing
strings. All command/receipt/version/status/exit/cap/resource/result/code
identity checks remain unchanged. Independent scoped portability review PASS
is in review-financial-unit-validation-v1.md. Root exercised the exact corrected
acceptance block against the retained actual batch3 receipt and result: accepted;
changing the memory limit to the string0.8: refused. This check took2.0seconds,
wrote no files and did not rerun native batch3.

Root continues batches4..6 individually through the already reviewed unchanged
Python audit and exact same external guard CLI, with unique original v1 paths.
The PowerShell multi-batch wrapper is not rerun and refuses existing attempts.
Each process must exit successfully; readinessv8 separately validates every
exact successful guard command and immutable result/start/code binding before
reporting seven checked units. A mere surviving result is still insufficient.

The improved per-unit checkpoints therefore retain useful completed work even
when an orchestration check fails. No financial publication, original economic
gate, gold release, return evaluation or consumer acceptance follows from them.

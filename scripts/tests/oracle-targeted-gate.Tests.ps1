$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptPath = Join-Path (Split-Path -Parent $here) 'oracle-targeted-gate.ps1'
. $scriptPath

Describe 'oracle targeted gate production adapter' {
  It 'executes the production adapter path and binds ordinary output to typed JSON fields' {
    $script:captured = $null
    $result = Invoke-OracleTargetedGate 'mode_a_targeted_tests' {
      param($spec)
      $script:captured = $spec
      [pscustomobject]@{ ExitCode = 0; Lines = @('Test project C:/atx/build', '100% tests passed, 0 tests failed out of 2') }
    }
    $script:captured.Program | Should Be 'powershell'
    ($script:captured.Arguments -join ' ') | Should Match '-Ctest -R \^mode_a_targeted_tests\$'
    $result.schema_version | Should Be 1
    $result.status | Should Be 'PASS'
    $result.command_id | Should Be 'mode_a_targeted_tests'
    $result.observations | Should Be 2
    $result.raw_output_sha256 | Should Match '^[0-9a-f]{64}$'
    ($result | ConvertTo-Json -Compress) | Should Match '"command_id":"mode_a_targeted_tests"'
  }

  It 'fails closed on a nonzero tool exit or empty evidence' {
    { Invoke-OracleTargetedGate 'mode_a_smoke' { [pscustomobject]@{ ExitCode = 1; Lines = @('failed') } } } | Should Throw
    { Invoke-OracleTargetedGate 'mode_b_smoke_tune' { [pscustomobject]@{ ExitCode = 0; Lines = @() } } } | Should Throw
  }
}

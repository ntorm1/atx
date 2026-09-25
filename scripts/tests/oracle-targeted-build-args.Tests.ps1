$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path (Split-Path -Parent $here) 'oracle-targeted-gate.ps1')

# Exercise only preparation argv. The retired oracle sources, executables,
# cohorts and store are neither needed nor opened by this compatibility check.
$cases = @(
  @{ Gate = 'mode_a_targeted_tests'; Preset = 'dev'; Targets = @('atx-vol-tests', 'atx-vol-oracle-bench') }
  @{ Gate = 'convention_tests'; Preset = 'dev'; Targets = @('atx-vol-oracle-convention-tests') }
  @{ Gate = 'mode_a_smoke_tune'; Preset = 'rel-avx2'; Targets = @('atx-vol-oracle-bench') }
  @{ Gate = 'convention_speed_measure'; Preset = 'rel-avx2'; Targets = @('atx-vol-oracle-bench') }
  @{ Gate = 'convention_speed'; Preset = 'rel-avx2'; Targets = @('atx-vol-oracle-bench') }
  @{ Gate = 'mode_b_targeted_tests'; Preset = 'dev'; Targets = @('atx-vol-tests') }
  @{ Gate = 'mode_b_smoke_tune'; Preset = 'rel-avx2'; Targets = @('atx-vol-oracle-bench') }
)

Describe 'Oracle preparation uses bounded wrapper jobs' {
  It '<Gate> preserves its preset and targets with two build workers' -TestCases $cases {
    param($Gate, $Preset, $Targets)
    $spec = Get-OracleTargetedGateSpec $Gate ([pscustomobject]@{ Sha = 'fixture'; Tree = 'fixture' })
    $expected = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
      (Join-Path $script:OracleRepoRoot 'scripts\atx-build.ps1'),
      '-Preset', $Preset, '-Jobs', '2', 'build') + $Targets
    @(Compare-Object @($spec.PrepareArguments) $expected -SyncWindow 0).Count | Should Be 0

    # The actual wrapper parses these exact caller arguments, but DryRun stops
    # before any configure, build, oracle workflow or process under test.
    $shell = Join-Path $PSHOME 'powershell.exe'
    $json = & $shell @($spec.PrepareArguments) -DryRun
    $LASTEXITCODE | Should Be 0
    $command = $json | ConvertFrom-Json
    $command.build_jobs | Should Be 2
    $binaryDir = if ($Preset -eq 'dev') { 'build' } else { 'build-rel-avx2' }
    $expectedBuild = @('--build', (Join-Path $script:OracleRepoRoot $binaryDir),
      '--parallel', '2', '--target') + $Targets
    @(Compare-Object @($command.arguments) $expectedBuild -SyncWindow 0).Count | Should Be 0
  }
}

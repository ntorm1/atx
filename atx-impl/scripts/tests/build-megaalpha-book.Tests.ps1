#Requires -Modules Pester
<#
.SYNOPSIS
  Pester argv tests for atx-impl/scripts/build-megaalpha-book.ps1 (p8 S5-4).

  Strategy: dot-source the script to load the New-*Argv functions without executing
  the pipeline body (the guard `if ($MyInvocation.InvocationName -ne '.')` prevents it,
  matching scripts/build-tradeable-alphas.ps1's own convention). All assertions are
  pure-function checks - no binary, no panel, no filesystem writes.

  Target Pester version: 3.4.0 (Windows built-in).
  Syntax used: Describe / It / Should - compatible with Pester 3.x.
  AVOIDS v5-only features: -ForEach, BeforeDiscovery.

  NOTE on Pester 3 array assertions:
    `Should Contain` in Pester 3 is a FILE-content assertion, NOT array membership.
    Array membership is tested with `($arr -contains 'x') | Should Be $true`.
#>

$scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'build-megaalpha-book.ps1'

# Dot-source to import functions; the guard must prevent the pipeline body from running.
. $scriptPath

# Fixed test inputs - isolated from any on-disk state.
$testPanel   = 'C:\atx-test\panel.bin'
$testWorkDir = 'C:\atx-test\work'
$testSeed    = 'C:\atx-test\alpha101.txt'

Describe 'New-DiscoverArgv - core flags present' {

    $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir

    It 'has discover as first element (subcommand)' {
        $argv[0] | Should Be 'discover'
    }

    It 'contains --gated flag' {
        ($argv -contains '--gated') | Should Be $true
    }

    It 'contains --seed-file flag and its path' {
        ($argv -contains '--seed-file') | Should Be $true
        ($argv -contains $testSeed) | Should Be $true
    }

    It 'contains --library-dir flag' {
        ($argv -contains '--library-dir') | Should Be $true
    }

    It 'contains --alpha-out flag (NOT bare --out)' {
        ($argv -contains '--alpha-out') | Should Be $true
        ($argv -contains '--out') | Should Be $false
    }
}

Describe 'New-DiscoverArgv - inert-value p8 flags on smoke (loose) profile' {

    $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir -LooseGates

    It 'contains --min-dsr at 0.0 (the RunConfig inert default)' {
        $i = [array]::IndexOf($argv, '--min-dsr')
        $i | Should BeGreaterThan -1
        $argv[$i + 1] | Should Be '0'
    }

    It 'does NOT contain the opt-in boolean switches (absence == inert)' {
        ($argv -contains '--impact-in-selection')  | Should Be $false
        ($argv -contains '--require-split-stable') | Should Be $false
        ($argv -contains '--blocking-pbo')          | Should Be $false
    }
}

Describe 'New-DiscoverArgv - prod profile opts in to the p8 deflation/impact seams' {

    $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
        -ImpactInSelection -RequireSplitStable -BlockingPbo -MinDsr 0.5 -MaxPbo 0.5

    It 'contains --impact-in-selection' {
        ($argv -contains '--impact-in-selection') | Should Be $true
    }

    It 'contains --require-split-stable' {
        ($argv -contains '--require-split-stable') | Should Be $true
    }

    It 'contains --blocking-pbo' {
        ($argv -contains '--blocking-pbo') | Should Be $true
    }

    It 'contains --min-dsr 0.5 and --max-pbo 0.5' {
        $di = [array]::IndexOf($argv, '--min-dsr')
        $pi = [array]::IndexOf($argv, '--max-pbo')
        $argv[$di + 1] | Should Be '0.5'
        $argv[$pi + 1] | Should Be '0.5'
    }
}

Describe 'New-DiscoverArgv - --workers conditional' {

    It '--workers is ABSENT when Workers=0' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir -Workers 0
        ($argv -contains '--workers') | Should Be $false
    }

    It '--workers IS PRESENT when Workers=5' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir -Workers 5
        ($argv -contains '--workers') | Should Be $true
        ($argv -contains '5') | Should Be $true
    }
}

Describe 'New-DiscoverArgv - population/generations parameterized (profile tiers)' {

    It 'emits the smoke search budget when passed (population 40, generations 4)' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
            -Population 40 -Generations 4
        $pi = [array]::IndexOf($argv, '--population')
        $gi = [array]::IndexOf($argv, '--generations')
        $argv[$pi + 1] | Should Be '40'
        $argv[$gi + 1] | Should Be '4'
    }

    It 'emits the prod search budget when passed (population 300, generations 15)' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
            -Population 300 -Generations 15
        $pi = [array]::IndexOf($argv, '--population')
        $gi = [array]::IndexOf($argv, '--generations')
        $argv[$pi + 1] | Should Be '300'
        $argv[$gi + 1] | Should Be '15'
    }
}

Describe 'New-CombineArgv - --method deviation (no --combine-method flag exists)' {

    It 'omits --method when Method is empty (smoke profile: engine default)' {
        $argv = New-CombineArgv -PanelBin $testPanel -WorkDir $testWorkDir -Method ''
        ($argv -contains '--method') | Should Be $false
    }

    It 'emits --method stack when Method=stack (prod profile), NOT --combine-method' {
        $argv = New-CombineArgv -PanelBin $testPanel -WorkDir $testWorkDir -Method 'stack'
        ($argv -contains '--method') | Should Be $true
        ($argv -contains '--combine-method') | Should Be $false
        $mi = [array]::IndexOf($argv, '--method')
        $argv[$mi + 1] | Should Be 'stack'
    }

    It 'has combine as first element (subcommand)' {
        $argv = New-CombineArgv -PanelBin $testPanel -WorkDir $testWorkDir
        $argv[0] | Should Be 'combine'
    }

    It 'contains --library-dir and --combo-out' {
        $argv = New-CombineArgv -PanelBin $testPanel -WorkDir $testWorkDir
        ($argv -contains '--library-dir') | Should Be $true
        ($argv -contains '--combo-out') | Should Be $true
    }
}

Describe 'New-MetabookArgv (S5-4 new stage)' {

    $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp'

    It 'has metabook as first element (subcommand)' {
        $argv[0] | Should Be 'metabook'
    }

    It 'contains --sleeve-method hrp' {
        $si = [array]::IndexOf($argv, '--sleeve-method')
        $si | Should BeGreaterThan -1
        $argv[$si + 1] | Should Be 'hrp'
    }

    It 'contains --combo, --library-dir, and --books-out' {
        ($argv -contains '--combo') | Should Be $true
        ($argv -contains '--library-dir') | Should Be $true
        ($argv -contains '--books-out') | Should Be $true
    }
}

Describe 'New-OptimizeArgv - --risk-model deviation (threaded via run_optimize, no riskmodel stage)' {

    It 'defaults to --risk-model diagonal (the RunConfig inert default) on smoke' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10
        $ri = [array]::IndexOf($argv, '--risk-model')
        $ri | Should BeGreaterThan -1
        $argv[$ri + 1] | Should Be 'diagonal'
        ($argv -contains '--dead-alpha-factors') | Should Be $false
        ($argv -contains '--group-neutralize') | Should Be $false
    }

    It 'emits --risk-model factor --dead-alpha-factors --group-neutralize on prod' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10 `
            -RiskModel 'factor' -DeadAlphaFactors -GroupNeutralize
        $ri = [array]::IndexOf($argv, '--risk-model')
        $argv[$ri + 1] | Should Be 'factor'
        ($argv -contains '--dead-alpha-factors') | Should Be $true
        ($argv -contains '--group-neutralize') | Should Be $true
    }

    It 'has optimize as first element and contains --position-mode/--cost-bps' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10
        $argv[0] | Should Be 'optimize'
        ($argv -contains '--position-mode') | Should Be $true
        ($argv -contains '--cost-bps') | Should Be $true
        ($argv -contains '10') | Should Be $true
    }
}

Describe 'New-ReportArgv - --capacity-curve opt-in' {

    It 'omits --capacity-curve by default (smoke)' {
        $argv = New-ReportArgv -PanelBin $testPanel -WorkDir $testWorkDir
        ($argv -contains '--capacity-curve') | Should Be $false
    }

    It 'includes --capacity-curve when requested (prod)' {
        $argv = New-ReportArgv -PanelBin $testPanel -WorkDir $testWorkDir -CapacityCurve
        ($argv -contains '--capacity-curve') | Should Be $true
    }

    It 'has report as first element and contains --books/--combo/--report-out' {
        $argv = New-ReportArgv -PanelBin $testPanel -WorkDir $testWorkDir
        $argv[0] | Should Be 'report'
        ($argv -contains '--books') | Should Be $true
        ($argv -contains '--combo') | Should Be $true
        ($argv -contains '--report-out') | Should Be $true
    }
}

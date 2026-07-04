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

    It 'does NOT contain --selection-aum (ImpactInSelection absent -> the companion is never emitted)' {
        ($argv -contains '--selection-aum') | Should Be $false
    }
}

Describe 'New-DiscoverArgv - prod profile opts in to the p8 deflation/impact seams' {

    $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
        -ImpactInSelection -SelectionAum 5.0e7 -RequireSplitStable -BlockingPbo -MinDsr 0.5 -MaxPbo 0.5

    It 'contains --impact-in-selection' {
        ($argv -contains '--impact-in-selection') | Should Be $true
    }

    It 'contains --selection-aum with a positive value (p8 final-wave: the companion --impact-in-selection needs to actually bite -- CostSelectionConfig contract)' {
        $i = [array]::IndexOf($argv, '--selection-aum')
        $i | Should BeGreaterThan -1
        [double]$argv[$i + 1] | Should BeGreaterThan 0
    }

    It 'omits --selection-aum when ImpactInSelection is set but SelectionAum is 0 (the documented inert no-op)' {
        $argvZero = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
            -ImpactInSelection -SelectionAum 0.0
        ($argvZero -contains '--selection-aum') | Should Be $false
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

Describe 'Resolve-ActiveStages - explicit stage requests are never silently emptied (S7-0)' {

    It 'an explicit -Stage optimize -Profile prod resolves to @(''optimize''), NOT empty' {
        $stages = Resolve-ActiveStages -Stage @('optimize') -Profile 'prod'
        ($stages -contains 'optimize') | Should Be $true
        $stages.Count | Should Be 1
    }

    It 'the "all" shorthand still auto-excludes optimize for -Profile prod (metabook substitutes)' {
        $stages = Resolve-ActiveStages -Stage @('all') -Profile 'prod'
        ($stages -contains 'metabook') | Should Be $true
        ($stages -contains 'optimize') | Should Be $false
    }

    It 'the "all" shorthand still auto-excludes metabook for -Profile smoke (optimize substitutes)' {
        $stages = Resolve-ActiveStages -Stage @('all') -Profile 'smoke'
        ($stages -contains 'optimize') | Should Be $true
        ($stages -contains 'metabook') | Should Be $false
    }

    It 'the "pipeline" shorthand behaves identically to "all" minus discover' {
        $stages = Resolve-ActiveStages -Stage @('pipeline') -Profile 'prod'
        ($stages -contains 'discover') | Should Be $false
        ($stages -contains 'metabook') | Should Be $true
        ($stages -contains 'optimize') | Should Be $false
    }
}

Describe 'Script defaults - S7-1 $AtxExe fix (stale p8 worktree -> p9 worktree)' {

    It 'the $AtxExe default points at the p9 worktree binary this script now lives in' {
        $AtxExe | Should Be 'C:\atx-wt\p9\build\bin\atx-impl.exe'
    }
}

Describe 'New-CombineArgv - S7-1 --risk-model factor reaches the combine CLI entry (S2)' {

    It 'prod: contains --risk-model factor' {
        $argv = New-CombineArgv -PanelBin $testPanel -WorkDir $testWorkDir -Method 'stack' -RiskModel 'factor'
        $ri = [array]::IndexOf($argv, '--risk-model')
        $ri | Should BeGreaterThan -1
        $argv[$ri + 1] | Should Be 'factor'
    }

    It 'default (smoke): omits --risk-model entirely (absence == inert; combine CLI entry stays Diagonal)' {
        $argv = New-CombineArgv -PanelBin $testPanel -WorkDir $testWorkDir
        ($argv -contains '--risk-model') | Should Be $false
    }
}

Describe 'New-MetabookArgv - S7-1 risk-model + book-level gates now reach the prod book stage' {

    It 'prod: contains --risk-model factor --dead-alpha-factors --dead-alpha-lib-dir --group-neutralize' {
        $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp' `
            -RiskModel 'factor' -DeadAlphaFactors -DeadAlphaLibDir 'C:\atx-test\work\_library' -GroupNeutralize
        ($argv -contains '--risk-model')         | Should Be $true
        ($argv -contains '--dead-alpha-factors')  | Should Be $true
        ($argv -contains '--dead-alpha-lib-dir')  | Should Be $true
        ($argv -contains '--group-neutralize')    | Should Be $true
    }

    It 'default (smoke): omits risk-model/dead-alpha/group-neutralize entirely (absence == inert)' {
        $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'invvol'
        ($argv -contains '--risk-model')        | Should Be $false
        ($argv -contains '--dead-alpha-factors') | Should Be $false
        ($argv -contains '--dead-alpha-lib-dir') | Should Be $false
        ($argv -contains '--group-neutralize')   | Should Be $false
    }

    It 'coupling: --dead-alpha-factors implies --dead-alpha-lib-dir is present with a non-empty value (fail-open default)' {
        $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp' `
            -RiskModel 'factor' -DeadAlphaFactors
        if ($argv -contains '--dead-alpha-factors') {
            $i = [array]::IndexOf($argv, '--dead-alpha-lib-dir')
            $i | Should BeGreaterThan -1
            $argv[$i + 1] | Should Not Be ''
        }
    }

    It 'contains --book-turnover-gate 0.20 and --participation-cap when requested' {
        $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp' `
            -BookTurnoverGate 0.20 -ParticipationCap 0.10
        $gi = [array]::IndexOf($argv, '--book-turnover-gate')
        $pi = [array]::IndexOf($argv, '--participation-cap')
        $argv[$gi + 1] | Should Be '0.2'
        $argv[$pi + 1] | Should Be '0.1'
    }

    It 'omits --book-turnover-gate/--participation-cap when both are 0 (the inert default)' {
        $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp'
        ($argv -contains '--book-turnover-gate') | Should Be $false
        ($argv -contains '--participation-cap')  | Should Be $false
    }
}

Describe 'New-DiscoverArgv - S7-1 capacity/turnover objectives + deflation + robustness battery (S4/S5/p8)' {

    It 'prod: contains --deflate-selection, --capacity-objective, --turnover-objective' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
            -DeflateSelection -CapacityObjective -TurnoverObjective
        ($argv -contains '--deflate-selection')  | Should Be $true
        ($argv -contains '--capacity-objective') | Should Be $true
        ($argv -contains '--turnover-objective') | Should Be $true
    }

    It 'default (smoke): omits deflate-selection/capacity-objective/turnover-objective entirely' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir -LooseGates
        ($argv -contains '--deflate-selection')  | Should Be $false
        ($argv -contains '--capacity-objective') | Should Be $false
        ($argv -contains '--turnover-objective') | Should Be $false
    }

    It 'prod: contains --robustness-battery and its 3 sub-checks when all requested' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
            -RobustnessBattery -RobustnessSubUniverse -RobustnessAltNeutralization -RobustnessParamPerturb
        ($argv -contains '--robustness-battery')            | Should Be $true
        ($argv -contains '--robustness-sub-universe')       | Should Be $true
        ($argv -contains '--robustness-alt-neutralization') | Should Be $true
        ($argv -contains '--robustness-param-perturb')      | Should Be $true
    }

    It 'default (smoke): omits --robustness-battery and all 3 sub-checks' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir -LooseGates
        ($argv -contains '--robustness-battery')            | Should Be $false
        ($argv -contains '--robustness-sub-universe')       | Should Be $false
        ($argv -contains '--robustness-alt-neutralization') | Should Be $false
        ($argv -contains '--robustness-param-perturb')      | Should Be $false
    }
}

Describe 'New-OptimizeArgv - S7-1 dead-alpha-lib-dir coupling + gp-trading flags (S1/S3)' {

    It 'coupling: --dead-alpha-factors implies --dead-alpha-lib-dir is present with a non-empty value (fail-open default)' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10 `
            -RiskModel 'factor' -DeadAlphaFactors -GroupNeutralize
        if ($argv -contains '--dead-alpha-factors') {
            $i = [array]::IndexOf($argv, '--dead-alpha-lib-dir')
            $i | Should BeGreaterThan -1
            $argv[$i + 1] | Should Not Be ''
        }
    }

    It 'contains --gp-trading --gp-risk-aversion --gp-trade-cost-scale when explicitly requested' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10 `
            -GpTrading -GpRiskAversion 1.0 -GpTradeCostScale 1.0
        ($argv -contains '--gp-trading')          | Should Be $true
        $ri = [array]::IndexOf($argv, '--gp-risk-aversion')
        $ci = [array]::IndexOf($argv, '--gp-trade-cost-scale')
        $ri | Should BeGreaterThan -1
        $ci | Should BeGreaterThan -1
    }
}

Describe 'New-ReportArgv - S7-1 --borrow-bps (S5 financing into the honest book-level cost numbers)' {

    It 'prod: contains --borrow-bps with a positive value' {
        $argv = New-ReportArgv -PanelBin $testPanel -WorkDir $testWorkDir -CapacityCurve -BorrowBps 25
        $i = [array]::IndexOf($argv, '--borrow-bps')
        $i | Should BeGreaterThan -1
        [double]$argv[$i + 1] | Should BeGreaterThan 0
    }

    It 'default (smoke): omits --borrow-bps' {
        $argv = New-ReportArgv -PanelBin $testPanel -WorkDir $testWorkDir
        ($argv -contains '--borrow-bps') | Should Be $false
    }
}

Describe 'New-OptimizeArgv - S7-2 gp-trading coupling' {

    It 'contains --gp-trading --gp-risk-aversion --gp-trade-cost-scale together' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10 `
            -GpTrading -GpRiskAversion 1.0 -GpTradeCostScale 1.0
        ($argv -contains '--gp-trading') | Should Be $true
        $ri = [array]::IndexOf($argv, '--gp-risk-aversion')
        $ci = [array]::IndexOf($argv, '--gp-trade-cost-scale')
        $ri | Should BeGreaterThan -1
        $ci | Should BeGreaterThan -1
    }

    It 'omits all three by default (smoke stays untouched)' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10
        ($argv -contains '--gp-trading') | Should Be $false
        ($argv -contains '--gp-risk-aversion') | Should Be $false
        ($argv -contains '--gp-trade-cost-scale') | Should Be $false
    }

    It 'coupling: --gp-risk-aversion/--gp-trade-cost-scale are NEVER emitted without --gp-trading (no orphan numerics)' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10 `
            -GpRiskAversion 1.0 -GpTradeCostScale 1.0
        ($argv -contains '--gp-trading') | Should Be $false
        ($argv -contains '--gp-risk-aversion') | Should Be $false
        ($argv -contains '--gp-trade-cost-scale') | Should Be $false
    }

    It 'coupling: --gp-trading alone still emits its own risk-aversion/cost-scale (illustrative defaults, never a bare flag)' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10 -GpTrading
        ($argv -contains '--gp-trading') | Should Be $true
        ($argv -contains '--gp-risk-aversion') | Should Be $true
        ($argv -contains '--gp-trade-cost-scale') | Should Be $true
    }
}

Describe 'New-DiscoverArgv - S7-2 robustness sub-checks require the master --robustness-battery' {

    It 'sub-checks are ABSENT even if requested when RobustnessBattery is not set (fail-safe, not silent)' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
            -RobustnessSubUniverse -RobustnessAltNeutralization -RobustnessParamPerturb
        ($argv -contains '--robustness-sub-universe') | Should Be $false
        ($argv -contains '--robustness-alt-neutralization') | Should Be $false
        ($argv -contains '--robustness-param-perturb') | Should Be $false
        ($argv -contains '--robustness-battery') | Should Be $false
    }

    It 'sub-checks ARE present when RobustnessBattery is also set (the prod combination)' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
            -RobustnessBattery -RobustnessSubUniverse -RobustnessAltNeutralization -RobustnessParamPerturb
        ($argv -contains '--robustness-battery') | Should Be $true
        ($argv -contains '--robustness-sub-universe') | Should Be $true
        ($argv -contains '--robustness-alt-neutralization') | Should Be $true
        ($argv -contains '--robustness-param-perturb') | Should Be $true
    }
}

Describe 'New-DiscoverArgv / New-MetabookArgv - S7-2 S6 guard: ml-seeds/nco deferred, never emitted' {

    It 'discover: --ml-seeds / --ml-seed-model-dir are structurally absent -- S6 deferred, no wiring exists' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir `
            -DeflateSelection -CapacityObjective -TurnoverObjective `
            -RobustnessBattery -RobustnessSubUniverse -RobustnessAltNeutralization -RobustnessParamPerturb
        ($argv -contains '--ml-seeds') | Should Be $false
        ($argv -contains '--ml-seed-model-dir') | Should Be $false
    }

    It 'metabook: --sleeve-method stays hrp (prod)/invvol (smoke) -- the raw string pass-through is unchanged by S7' {
        $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'hrp'
        $si = [array]::IndexOf($argv, '--sleeve-method')
        $argv[$si + 1] | Should Be 'hrp'
    }

    It 'the top-level script exposes no -MlSeeds / -MlSeedModelDir / -SleeveMethod switch (S6 wiring was never added, not silently omitted)' {
        $scriptParams = (Get-Command $scriptPath).Parameters.Keys
        ($scriptParams -contains 'MlSeeds')        | Should Be $false
        ($scriptParams -contains 'MlSeedModelDir') | Should Be $false
        ($scriptParams -contains 'SleeveMethod')   | Should Be $false
    }
}

Describe 'Smoke profile - S7-3 stays minimal: none of the new p9 flags leak in' {

    It 'New-DiscoverArgv default (smoke-shaped call) omits every new p9 flag' {
        $argv = New-DiscoverArgv -PanelBin $testPanel -SeedFile $testSeed -WorkDir $testWorkDir -LooseGates
        foreach ($f in @('--deflate-selection','--capacity-objective','--turnover-objective',
                          '--robustness-battery','--robustness-sub-universe',
                          '--robustness-alt-neutralization','--robustness-param-perturb',
                          '--ml-seeds','--ml-seed-model-dir')) {
            ($argv -contains $f) | Should Be $false
        }
    }

    It 'New-CombineArgv default (smoke-shaped call) omits --risk-model' {
        $argv = New-CombineArgv -PanelBin $testPanel -WorkDir $testWorkDir
        ($argv -contains '--risk-model') | Should Be $false
    }

    It 'New-MetabookArgv default (smoke-shaped call) omits every new p9 flag' {
        $argv = New-MetabookArgv -PanelBin $testPanel -WorkDir $testWorkDir -SleeveMethod 'invvol'
        foreach ($f in @('--risk-model','--dead-alpha-factors','--dead-alpha-lib-dir',
                          '--group-neutralize','--book-turnover-gate','--participation-cap')) {
            ($argv -contains $f) | Should Be $false
        }
    }

    It 'New-OptimizeArgv default (smoke-shaped call) omits gp-trading + new S1 flags' {
        $argv = New-OptimizeArgv -PanelBin $testPanel -WorkDir $testWorkDir -CostBps 10
        foreach ($f in @('--gp-trading','--gp-risk-aversion','--gp-trade-cost-scale','--dead-alpha-lib-dir')) {
            ($argv -contains $f) | Should Be $false
        }
    }

    It 'New-ReportArgv default (smoke-shaped call) omits --borrow-bps' {
        $argv = New-ReportArgv -PanelBin $testPanel -WorkDir $testWorkDir
        ($argv -contains '--borrow-bps') | Should Be $false
    }
}

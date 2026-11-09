param(
    [Parameter(Mandatory=$true)][string]$Editor,
    [Parameter(Mandatory=$true)][string]$Project,
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..\..\.."),
    [string]$OutRoot = "C:\tmp\lightusd_ue_regressions",
    [switch]$MetaHuman,
    [switch]$MetaHumanOnly
)

$ErrorActionPreference = "Stop"
$runId = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$results = @()
New-Item -ItemType Directory -Force $OutRoot | Out-Null
$env:LIGHTUSD_REPO_ROOT = $RepoRoot
$env:LIGHTUSD_USDSKEL_FIXTURE = Join-Path $RepoRoot "tests\usda\ue-skinned-blendshape.usda"

function Invoke-LightUSDRegression {
    param(
        [string]$Name,
        [string]$Script,
        [string]$GroomUsd = ""
    )
    $out = Join-Path $OutRoot $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    $env:LIGHTUSD_UE_TEST_OUT = $out
    $env:LIGHTUSD_UE_TEST_PACKAGE = "/Game/LightUSD/Automated_${runId}_${Name}"
    $env:LIGHTUSD_GROOM_PACKAGE = "/Game/LightUSD/Grooms_${runId}_${Name}"
    if ($GroomUsd) {
        $env:LIGHTUSD_GROOM_USD = $GroomUsd
    } else {
        Remove-Item Env:LIGHTUSD_GROOM_USD -ErrorAction SilentlyContinue
    }
    $started = Get-Date
    $scriptPath = Join-Path $PSScriptRoot $Script
    & $Editor $Project "-ExecutePythonScript=$scriptPath" `
        -unattended -nullrhi -NoSplash -NoSound -NoP4 -NoUBA -log
    $exitCode = $LASTEXITCODE
    $reportPath = Join-Path $out "report.json"
    if ($exitCode -ne 0) {
        throw "$Name editor process failed with exit code $exitCode"
    }
    if (!(Test-Path $reportPath) -or (Get-Item $reportPath).LastWriteTime -lt $started) {
        throw "$Name did not create a fresh report"
    }
    $script:results += [ordered]@{
        name = $Name
        report = $reportPath
        result = (Get-Content $reportPath -Raw | ConvertFrom-Json)
    }
}

if (!$MetaHumanOnly) {
    Invoke-LightUSDRegression "physics" "ue_physics_roundtrip.py"
    Invoke-LightUSDRegression "material_graph" "ue_material_graph_roundtrip.py"
    Invoke-LightUSDRegression "rigged" "ue_rigged_roundtrip.py"
    Invoke-LightUSDRegression "usdskel_facial" "ue_usdskel_fixture.py"
    Invoke-LightUSDRegression "groom" "ue_groom_roundtrip.py"
    Invoke-LightUSDRegression "groom_animated" "ue_groom_roundtrip.py" `
        (Join-Path $RepoRoot "tests\usda\blender-animated-groom.usda")
    Invoke-LightUSDRegression "groom_nurbs" "ue_groom_roundtrip.py" `
        (Join-Path $RepoRoot "tests\usda\blender-nurbs-groom.usda")
    Invoke-LightUSDRegression "groom_guides" "ue_groom_roundtrip.py" `
        (Join-Path $RepoRoot "tests\usda\blender-guide-groom.usda")
    Invoke-LightUSDRegression "groom_cards" "ue_groom_cards_roundtrip.py"
}

if ($MetaHuman -or $MetaHumanOnly) {
    Invoke-LightUSDRegression "metahuman_template" "ue_metahuman_template_roundtrip.py"
    Invoke-LightUSDRegression "metahuman_material_udim" "ue_metahuman_material_roundtrip.py"
    Invoke-LightUSDRegression "metahuman_scene" "ue_metahuman_template_scene_roundtrip.py"
}

$summary = [ordered]@{
    format = "lightusd-ue-regression-v1"
    succeeded = $true
    ue_minimum = "5.8"
    tests = $results
}
$summaryPath = Join-Path $OutRoot "summary.json"
$summary | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 $summaryPath
Write-Output ("LIGHTUSD_UE_SUITE_REPORT=" + ($summary | ConvertTo-Json -Depth 12 -Compress))

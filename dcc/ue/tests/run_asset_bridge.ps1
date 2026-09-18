param(
    [Parameter(Mandatory=$true)][string]$BridgeUrl,
    [Parameter(Mandatory=$true)][string]$BridgeToken,
    [Parameter(Mandatory=$true)][string]$AssetId
)

$env:LIGHTUSD_ASSET_BRIDGE_URL = $BridgeUrl
$env:LIGHTUSD_ASSET_BRIDGE_TOKEN = $BridgeToken
$env:LIGHTUSD_ASSET_ID = $AssetId
$env:LIGHTUSD_ASSET_BUNDLE_ID = $AssetId
$env:LIGHTUSD_BRIDGE_INPUT = "D:\work\lightusd\UBTFullTest\BlenderBridge\skinned.usda"
$env:LIGHTUSD_BRIDGE_OUTPUT = "D:\work\lightusd\UBTFullTest\BlenderBridge\ue_roundtrip.usda"
$env:LIGHTUSD_BRIDGE_REPORT = "D:\work\lightusd\UBTFullTest\BlenderBridge\report.json"

New-Item -ItemType Directory -Force "D:\work\lightusd\UBTFullTest\BlenderBridge" | Out-Null
$started = Get-Date
& "C:\PROGRA~1\EPICGA~1\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
    "D:\work\lightusd\UBTFullTest\UBTFullTest.uproject" `
    "-ExecutePythonScript=D:\work\lightusd\UBTFullTest\UEAssetBridgeRoundtrip.py" `
    -unattended -nullrhi -NoSplash -NoSound -NoP4 -NoUBA -log
$editorExit = $LASTEXITCODE
if ($editorExit -ne 0) {
    exit $editorExit
}
$reportPath = $env:LIGHTUSD_BRIDGE_REPORT
if (!(Test-Path $reportPath) -or (Get-Item $reportPath).LastWriteTime -lt $started) {
    Write-Error "UE bridge did not create a fresh report: $reportPath"
    exit 2
}
$report = Get-Content $reportPath -Raw | ConvertFrom-Json
if (!$report.output_asset.id -or $report.validation_prim_count -lt 1) {
    Write-Error "UE bridge report failed validation"
    exit 3
}
Write-Output ("LIGHTUSD_UE_REPORT=" + ($report | ConvertTo-Json -Depth 8 -Compress))
exit 0

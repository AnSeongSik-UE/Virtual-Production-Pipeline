param(
    [ValidateSet("Development", "Shipping")]
    [string]$Configuration = "Shipping",
    [switch]$SkipUnreal,
    [switch]$RunSmokeTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$UnrealProjectRoot = Join-Path $ProjectRoot "VPPipeline"
$UnrealProject = Join-Path $UnrealProjectRoot "VPPipeline.uproject"
$TrackerRoot = Join-Path $ProjectRoot "vp-tracker"
$EngineRoot = "C:\Program Files\Epic Games\UE_5.8"
$RunUAT = Join-Path $EngineRoot "Engine\Build\BatchFiles\RunUAT.bat"
$UnrealArchive = Join-Path $ProjectRoot "Build\Windows-$Configuration"
$UnrealBuildManifest = Join-Path $UnrealArchive ".vpp-unreal-build.json"
$ReleaseRoot = Join-Path $ProjectRoot "Build\OneClick-Windows-$Configuration"
$SymbolsRoot = Join-Path $ProjectRoot "Build\Symbols-Windows-$Configuration"
$PyInstaller = Join-Path $TrackerRoot ".venv\Scripts\pyinstaller.exe"
$ReleaseValidator = Join-Path $PSScriptRoot "Test-OneClickRelease.ps1"

function Get-UnrealInputFingerprint {
    $InputFiles = @((Get-Item -LiteralPath $UnrealProject))
    foreach ($RelativeRoot in @("Config", "Content", "Plugins", "Source")) {
        $Root = Join-Path $UnrealProjectRoot $RelativeRoot
        if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
            continue
        }
        $InputFiles += Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
            $_.FullName -notmatch '[\\/](Binaries|Intermediate|Saved|DerivedDataCache)[\\/]'
        }
    }

    $Records = $InputFiles | Sort-Object FullName | ForEach-Object {
        $RelativePath = $_.FullName.Substring($UnrealProjectRoot.Length).TrimStart('\', '/')
        "{0}`t{1}`t{2}" -f $RelativePath, $_.Length, $_.LastWriteTimeUtc.Ticks
    }
    $Payload = [Text.Encoding]::UTF8.GetBytes(($Records -join "`n"))
    $Hasher = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($Hasher.ComputeHash($Payload))).Replace("-", "")
    }
    finally {
        $Hasher.Dispose()
    }
}

function Assert-ReusableUnrealArchive {
    param([string]$ExpectedFingerprint)

    if (-not (Test-Path -LiteralPath $UnrealBuildManifest -PathType Leaf)) {
        throw "-SkipUnreal requires a build manifest created by this script: $UnrealBuildManifest"
    }
    $Manifest = Get-Content -LiteralPath $UnrealBuildManifest -Raw | ConvertFrom-Json
    if ($Manifest.configuration -ne $Configuration) {
        throw "Unreal archive configuration mismatch: expected $Configuration, found $($Manifest.configuration)"
    }
    if ($Manifest.inputFingerprint -ne $ExpectedFingerprint) {
        throw "Unreal project inputs changed after the archive was built. Re-run without -SkipUnreal."
    }
}

function Move-ReleaseSymbols {
    if (Test-Path -LiteralPath $SymbolsRoot) {
        Remove-Item -LiteralPath $SymbolsRoot -Recurse -Force
    }

    $PdbFiles = @(Get-ChildItem -LiteralPath $ReleaseRoot -Recurse -File -Filter "*.pdb")
    if ($PdbFiles.Count -eq 0) {
        return
    }

    New-Item -ItemType Directory -Path $SymbolsRoot | Out-Null
    foreach ($Pdb in $PdbFiles) {
        $RelativePath = $Pdb.FullName.Substring($ReleaseRoot.Length).TrimStart('\', '/')
        $Destination = Join-Path $SymbolsRoot $RelativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
        Move-Item -LiteralPath $Pdb.FullName -Destination $Destination -Force
    }
    Write-Host "Debug symbols separated: $SymbolsRoot ($($PdbFiles.Count) files)"
}

if (-not (Test-Path -LiteralPath $RunUAT -PathType Leaf)) {
    throw "Unreal Automation Tool not found: $RunUAT"
}
if (-not (Test-Path -LiteralPath $PyInstaller -PathType Leaf)) {
    throw "PyInstaller is not installed in vp-tracker/.venv."
}
if (-not (Test-Path -LiteralPath $ReleaseValidator -PathType Leaf)) {
    throw "Release validator not found: $ReleaseValidator"
}

$InputFingerprint = Get-UnrealInputFingerprint
if ($SkipUnreal) {
    Assert-ReusableUnrealArchive -ExpectedFingerprint $InputFingerprint
}
else {
    if (Test-Path -LiteralPath $UnrealArchive) {
        Remove-Item -LiteralPath $UnrealArchive -Recurse -Force
    }
    & $RunUAT BuildCookRun `
        "-project=$UnrealProject" `
        -nop4 -utf8output -platform=Win64 `
        "-clientconfig=$Configuration" `
        -build -cook "-map=/Game/Maps/Lvl_Empty" `
        -stage -package -pak -prereqs -archive `
        "-archivedirectory=$UnrealArchive"
    if ($LASTEXITCODE -ne 0) {
        throw "Unreal packaging failed with exit code $LASTEXITCODE"
    }

    @{
        schemaVersion = 1
        configuration = $Configuration
        inputFingerprint = $InputFingerprint
        createdUtc = [DateTime]::UtcNow.ToString("o")
    } | ConvertTo-Json | Set-Content -LiteralPath $UnrealBuildManifest -Encoding UTF8
}

$UnrealExe = Join-Path $UnrealArchive "VPPipeline.exe"
if (-not (Test-Path -LiteralPath $UnrealExe -PathType Leaf)) {
    throw "Packaged Unreal application not found: $UnrealExe"
}

Push-Location $TrackerRoot
try {
    & $PyInstaller --noconfirm --clean "VirtualProductionPipeline.spec"
    if ($LASTEXITCODE -ne 0) {
        throw "Python launcher packaging failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

$PythonBundle = Join-Path $TrackerRoot "dist\VirtualProductionPipeline"
if (-not (Test-Path -LiteralPath $PythonBundle -PathType Container)) {
    throw "Python bundle not found: $PythonBundle"
}

if (Test-Path -LiteralPath $ReleaseRoot) {
    Remove-Item -LiteralPath $ReleaseRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $ReleaseRoot | Out-Null
Copy-Item -Path (Join-Path $PythonBundle "*") -Destination $ReleaseRoot -Recurse -Force

$RuntimeRoot = Join-Path $ReleaseRoot "Runtime"
New-Item -ItemType Directory -Path $RuntimeRoot | Out-Null
Copy-Item -Path (Join-Path $UnrealArchive "*") -Destination $RuntimeRoot -Recurse -Force
$CopiedBuildManifest = Join-Path $RuntimeRoot ".vpp-unreal-build.json"
if (Test-Path -LiteralPath $CopiedBuildManifest -PathType Leaf) {
    Remove-Item -LiteralPath $CopiedBuildManifest -Force
}

foreach ($Document in @("README.md", "THIRD_PARTY_NOTICES.md", "LICENSE")) {
    $Source = Join-Path $ProjectRoot $Document
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        Copy-Item -LiteralPath $Source -Destination $ReleaseRoot
    }
}
Copy-Item -LiteralPath (Join-Path $ProjectRoot "tools\release\START_HERE.txt") -Destination $ReleaseRoot

$ThirdPartyLicenseRoot = Join-Path $ReleaseRoot "ThirdPartyLicenses"
New-Item -ItemType Directory -Path $ThirdPartyLicenseRoot | Out-Null
$SitePackages = Join-Path $TrackerRoot ".venv\Lib\site-packages"
Get-ChildItem -LiteralPath $SitePackages -Directory -Filter "*.dist-info" | ForEach-Object {
    $Destination = Join-Path $ThirdPartyLicenseRoot $_.Name
    New-Item -ItemType Directory -Path $Destination | Out-Null
    Copy-Item -LiteralPath (Join-Path $_.FullName "METADATA") -Destination $Destination
    $Licenses = Join-Path $_.FullName "licenses"
    if (Test-Path -LiteralPath $Licenses -PathType Container) {
        Copy-Item -Path (Join-Path $Licenses "*") -Destination $Destination -Recurse -Force
    }
}
Copy-Item -LiteralPath (Join-Path $SitePackages "cv2\LICENSE.txt") -Destination $ThirdPartyLicenseRoot
Copy-Item -LiteralPath (Join-Path $SitePackages "cv2\LICENSE-3RD-PARTY.txt") -Destination $ThirdPartyLicenseRoot
Copy-Item -LiteralPath (Join-Path $ProjectRoot "VPPipeline\Plugins\Spout2_DX12\LICENSE") -Destination (Join-Path $ThirdPartyLicenseRoot "Spout2_DX12-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $ProjectRoot "VPPipeline\Plugins\VRM4U\ThirdParty\assimp\LICENSE") -Destination (Join-Path $ThirdPartyLicenseRoot "VRM4U-assimp-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $ProjectRoot "VPPipeline\Plugins\VRM4U\ThirdParty\rapidjson\license.txt") -Destination (Join-Path $ThirdPartyLicenseRoot "VRM4U-rapidjson-LICENSE.txt")

if ($Configuration -eq "Shipping") {
    Move-ReleaseSymbols
    $ArmRedistributable = Join-Path $RuntimeRoot "Engine\Extras\Redist\en-us\vc_redist.arm64.exe"
    if (Test-Path -LiteralPath $ArmRedistributable -PathType Leaf) {
        Remove-Item -LiteralPath $ArmRedistributable -Force
    }
}

$Launcher = Join-Path $ReleaseRoot "VirtualProductionPipeline.exe"
if (-not (Test-Path -LiteralPath $Launcher -PathType Leaf)) {
    throw "One-click launcher not found: $Launcher"
}

$ValidationParameters = @{
    ReleaseRoot = $ReleaseRoot
    Configuration = $Configuration
}
if ($RunSmokeTest) {
    $ValidationParameters.RunSmokeTest = $true
}
& $ReleaseValidator @ValidationParameters

Write-Host "One-click distribution created and validated: $ReleaseRoot"

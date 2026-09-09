[CmdletBinding()]
param(
    [string]$Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ReleaseTag = 'v1.2026.07.22'
$ArchiveName = 'VRM4U_5_8_20260722.zip'
$ArchiveUri = "https://github.com/ruyo/VRM4U/releases/download/$ReleaseTag/$ArchiveName"
$ArchiveSha256 = '0989A53FD76A4EEA48B8C88A823BDD24B69091974C03E117F67D6A58315BC25B'
$PatchPath = Join-Path $PSScriptRoot 'patches\VRM4U-v1.2026.07.22-UE58-runtime.patch'
$RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $RepositoryRoot 'VPPipeline\Plugins\VRM4U'
}
elseif (![IO.Path]::IsPathRooted($Destination)) {
    $Destination = Join-Path (Get-Location) $Destination
}
$Destination = [IO.Path]::GetFullPath($Destination)

if (!(Test-Path -LiteralPath $PatchPath -PathType Leaf)) {
    throw "VRM4U patch file is missing: $PatchPath"
}
if (!(Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'Git is required to verify and apply the VRM4U compatibility patch.'
}

function Invoke-PatchCheck {
    param([switch]$Reverse)

    $Arguments = @('apply', '--check', '--unsafe-paths', "--directory=$Destination")
    if ($Reverse) {
        $Arguments += '--reverse'
    }
    $Arguments += $PatchPath

    & git @Arguments 2>$null
    return $LASTEXITCODE -eq 0
}

function Get-PluginPatchState {
    if (!(Test-Path -LiteralPath $Destination)) {
        return 'Missing'
    }
    if (!(Test-Path -LiteralPath $Destination -PathType Container) -or
        !(Test-Path -LiteralPath (Join-Path $Destination 'VRM4U.uplugin') -PathType Leaf)) {
        return 'Mismatch'
    }

    $CanApply = Invoke-PatchCheck
    $CanReverse = Invoke-PatchCheck -Reverse
    if ($CanReverse -and !$CanApply) {
        return 'Patched'
    }
    if ($CanApply -and !$CanReverse) {
        return 'Pristine'
    }
    return 'Mismatch'
}

function Assert-Ue58Plugin {
    $DescriptorPath = Join-Path $Destination 'VRM4U.uplugin'
    $Descriptor = Get-Content -LiteralPath $DescriptorPath -Raw | ConvertFrom-Json
    if ($Descriptor.FriendlyName -ne 'VRM4U' -or $Descriptor.EngineVersion -ne '5.8.0') {
        throw "Expected the official UE 5.8 VRM4U plugin at: $Destination"
    }
}

$InitialState = Get-PluginPatchState
if ($InitialState -eq 'Patched') {
    Assert-Ue58Plugin
    Write-Host "VRM4U $ReleaseTag for UE 5.8 is already installed and patched."
    exit 0
}
if ($InitialState -eq 'Mismatch') {
    throw "The existing VRM4U source does not match the supported $ReleaseTag UE 5.8 release or patch state. No files were changed."
}

$TemporaryRoot = $null
try {
    if ($InitialState -eq 'Missing') {
        $TemporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("VPPipeline-VRM4U-" + [Guid]::NewGuid().ToString('N'))
        $ArchivePath = Join-Path $TemporaryRoot $ArchiveName
        $ExtractRoot = Join-Path $TemporaryRoot 'extracted'
        New-Item -ItemType Directory -Path $TemporaryRoot | Out-Null

        Write-Host "Downloading official VRM4U $ReleaseTag for UE 5.8..."
        Invoke-WebRequest -Uri $ArchiveUri -OutFile $ArchivePath
        $ActualHash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash
        if ($ActualHash -ne $ArchiveSha256) {
            throw "VRM4U archive SHA-256 mismatch. Expected $ArchiveSha256 but received $ActualHash."
        }

        Expand-Archive -LiteralPath $ArchivePath -DestinationPath $ExtractRoot
        $ExtractedPlugin = Join-Path $ExtractRoot 'Plugins\VRM4U'
        if (!(Test-Path -LiteralPath (Join-Path $ExtractedPlugin 'VRM4U.uplugin') -PathType Leaf)) {
            throw 'The official archive did not contain Plugins\VRM4U\VRM4U.uplugin.'
        }

        $PluginParent = Split-Path -Parent $Destination
        New-Item -ItemType Directory -Path $PluginParent -Force | Out-Null
        Move-Item -LiteralPath $ExtractedPlugin -Destination $Destination
    }

    Assert-Ue58Plugin
    if ((Get-PluginPatchState) -ne 'Pristine') {
        throw 'VRM4U was installed, but the compatibility patch cannot be applied cleanly.'
    }

    & git apply --unsafe-paths "--directory=$Destination" $PatchPath
    if ($LASTEXITCODE -ne 0) {
        throw "git apply failed with exit code $LASTEXITCODE."
    }
    if ((Get-PluginPatchState) -ne 'Patched') {
        throw 'VRM4U patch verification failed after application.'
    }

    Write-Host "VRM4U $ReleaseTag for UE 5.8 was installed and patched successfully."
}
finally {
    if ($TemporaryRoot) {
        $ResolvedTemp = [IO.Path]::GetFullPath($TemporaryRoot)
        $SystemTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if ($ResolvedTemp.StartsWith($SystemTemp, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $ResolvedTemp).StartsWith('VPPipeline-VRM4U-', [StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $ResolvedTemp -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

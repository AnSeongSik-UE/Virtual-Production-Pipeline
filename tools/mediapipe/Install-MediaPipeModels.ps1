[CmdletBinding()]
param(
    [switch]$IncludeHeavy
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ModelsDirectory = Join-Path $ProjectRoot 'vp-tracker\models'

$Models = @(
    @{
        Name = 'face_landmarker.task'
        Uri = 'https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/latest/face_landmarker.task'
        Sha256 = '64184E229B263107BC2B804C6625DB1341FF2BB731874B0BCC2FE6544E0BC9FF'
    },
    @{
        Name = 'pose_landmarker_full.task'
        Uri = 'https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/latest/pose_landmarker_full.task'
        Sha256 = '4EAA5EB7A98365221087693FCC286334CF0858E2EB6E15B506AA4A7ECDCEC4AD'
    }
)

if ($IncludeHeavy) {
    $Models += @{
        Name = 'pose_landmarker_heavy.task'
        Uri = 'https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_heavy/float16/latest/pose_landmarker_heavy.task'
        Sha256 = '64437AF838A65D18E5BA7A0D39B465540069BC8AAE8308DE3E318AAD31FCBC7B'
    }
}

New-Item -ItemType Directory -Path $ModelsDirectory -Force | Out-Null

foreach ($Model in $Models) {
    $Destination = Join-Path $ModelsDirectory $Model.Name
    if (Test-Path -LiteralPath $Destination) {
        $ExistingHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
        if ($ExistingHash -ne $Model.Sha256) {
            throw "Existing model hash mismatch; refusing to overwrite: $Destination"
        }
        Write-Host "[OK] $($Model.Name) already installed and verified."
        continue
    }

    $TemporaryPath = "$Destination.download"
    try {
        Write-Host "[GET] $($Model.Name)"
        Invoke-WebRequest -Uri $Model.Uri -OutFile $TemporaryPath
        $DownloadedHash = (Get-FileHash -LiteralPath $TemporaryPath -Algorithm SHA256).Hash
        if ($DownloadedHash -ne $Model.Sha256) {
            throw "Downloaded model hash mismatch: $($Model.Name)"
        }
        Move-Item -LiteralPath $TemporaryPath -Destination $Destination
        Write-Host "[OK] $($Model.Name) installed and verified."
    }
    finally {
        if (Test-Path -LiteralPath $TemporaryPath) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
    }
}

Write-Host '[OK] MediaPipe runtime models are ready.'

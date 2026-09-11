param(
    [Parameter(Mandatory = $true)]
    [string]$ReleaseRoot,
    [ValidateSet("Development", "Shipping")]
    [string]$Configuration = "Shipping",
    [switch]$RunSmokeTest,
    [ValidateRange(30, 600)]
    [int]$SmokeTimeoutSeconds = 180,
    [string]$ReportPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ReleaseRoot = [IO.Path]::GetFullPath($ReleaseRoot)
if (-not $ReportPath) {
    $ReportPath = Join-Path $ProjectRoot "Build\ReleaseValidation-$Configuration.json"
}
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
$Results = [Collections.Generic.List[object]]::new()

function Add-Result {
    param(
        [ValidateSet("PASS", "WARN", "FAIL", "INFO")]
        [string]$Status,
        [string]$Check,
        [string]$Detail
    )

    $Results.Add([PSCustomObject]@{
        status = $Status
        check = $Check
        detail = $Detail
    })
    $Color = switch ($Status) {
        "PASS" { "Green" }
        "WARN" { "Yellow" }
        "FAIL" { "Red" }
        default { "Gray" }
    }
    Write-Host "[$Status] $Check - $Detail" -ForegroundColor $Color
}

function Get-RelativeReleasePath {
    param([string]$Path)
    return $Path.Substring($ReleaseRoot.Length).TrimStart('\', '/')
}

function Test-PortAvailable {
    param([int]$Port)
    $Probe = $null
    try {
        $Probe = [Net.Sockets.UdpClient]::new($Port)
        return $true
    }
    catch [Net.Sockets.SocketException] {
        return $false
    }
    finally {
        if ($null -ne $Probe) {
            $Probe.Dispose()
        }
    }
}

function Get-SpoutSenderInfo {
    $SenderNameMap = $null
    $SenderNameView = $null
    $SenderInfoMap = $null
    $SenderInfoView = $null
    try {
        $SenderNameMap = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("SpoutSenderNames")
        $SenderNameView = $SenderNameMap.CreateViewAccessor(
            0,
            0,
            [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read
        )
        $NamesBuffer = [byte[]]::new([int]$SenderNameView.Capacity)
        [void]$SenderNameView.ReadArray(0, $NamesBuffer, 0, $NamesBuffer.Length)

        $Found = $false
        for ($Offset = 0; $Offset + 256 -le $NamesBuffer.Length; $Offset += 256) {
            $Length = 0
            while ($Length -lt 256 -and $NamesBuffer[$Offset + $Length] -ne 0) {
                $Length++
            }
            if ($Length -eq 0) {
                break
            }
            $Name = [Text.Encoding]::ASCII.GetString($NamesBuffer, $Offset, $Length)
            if ($Name -eq "Virtual Production Pipeline") {
                $Found = $true
                break
            }
        }
        if (-not $Found) {
            return $null
        }

        $SenderInfoMap = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("Virtual Production Pipeline")
        $SenderInfoView = $SenderInfoMap.CreateViewAccessor(
            0,
            16,
            [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read
        )
        return [PSCustomObject]@{
            ShareHandle = $SenderInfoView.ReadUInt32(0)
            Width = $SenderInfoView.ReadUInt32(4)
            Height = $SenderInfoView.ReadUInt32(8)
            Format = $SenderInfoView.ReadUInt32(12)
        }
    }
    catch [IO.FileNotFoundException] {
        return $null
    }
    catch [UnauthorizedAccessException] {
        return $null
    }
    catch {
        return $null
    }
    finally {
        if ($null -ne $SenderInfoView) { $SenderInfoView.Dispose() }
        if ($null -ne $SenderInfoMap) { $SenderInfoMap.Dispose() }
        if ($null -ne $SenderNameView) { $SenderNameView.Dispose() }
        if ($null -ne $SenderNameMap) { $SenderNameMap.Dispose() }
    }
}

function Get-ReleaseProcesses {
    $Prefix = $ReleaseRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    try {
        return @(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object {
            $_.ExecutablePath -and $_.ExecutablePath.StartsWith(
                $Prefix,
                [StringComparison]::OrdinalIgnoreCase
            )
        })
    }
    catch {
        Add-Result "WARN" "Process inventory" "Unable to query Win32_Process: $($_.Exception.Message)"
        return @()
    }
}

function Stop-SmokeProcesses {
    foreach ($ProcessInfo in @(Get-ReleaseProcesses)) {
        try {
            Stop-Process -Id $ProcessInfo.ProcessId -Force -ErrorAction Stop
        }
        catch {
            Write-Host "[WARN] Cleanup could not stop PID $($ProcessInfo.ProcessId): $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
}

function Invoke-SmokeTest {
    $Launcher = Join-Path $ReleaseRoot "VirtualProductionPipeline.exe"
    $SmokeRoot = Join-Path $ProjectRoot "Build\SmokeTests"
    New-Item -ItemType Directory -Path $SmokeRoot -Force | Out-Null
    $Timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $LauncherLog = Join-Path $SmokeRoot "Shipping-$Timestamp-launcher.log"
    $LauncherProcess = $null
    $PreviousLogSetting = $env:VP_LOG_FILE

    try {
        if (-not (Test-PortAvailable 7000) -or -not (Test-PortAvailable 7001)) {
            Add-Result "FAIL" "Smoke preflight" "UDP 7000 or 7001 is already occupied."
            return
        }

        $env:VP_LOG_FILE = $LauncherLog
        $LauncherProcess = Start-Process -FilePath $Launcher -WorkingDirectory $ReleaseRoot -PassThru
        Add-Result "INFO" "Smoke launch" "Started PID $($LauncherProcess.Id); log $LauncherLog"

        $Deadline = (Get-Date).AddSeconds($SmokeTimeoutSeconds)
        $LauncherReady = $false
        $CameraReady = $false
        $TrackingSent = $false
        $SpoutReady = $false
        $SpoutInfo = $null
        while ((Get-Date) -lt $Deadline) {
            if (Test-Path -LiteralPath $LauncherLog -PathType Leaf) {
                $LauncherText = Get-Content -LiteralPath $LauncherLog -Raw -ErrorAction SilentlyContinue
                $LauncherReady = $LauncherText -match "Pipeline managed session started"
                $CameraReady = $LauncherText -match "\[CAM\] Webcam opened"
                $TrackingSent = $LauncherText -match "Sent:\s*\d+"
            }
            $SpoutInfo = Get-SpoutSenderInfo
            $SpoutReady = $null -ne $SpoutInfo -and
                $SpoutInfo.ShareHandle -ne 0 -and
                $SpoutInfo.Width -eq 1280 -and
                $SpoutInfo.Height -eq 720
            if ($LauncherReady -and $CameraReady -and $TrackingSent -and $SpoutReady) {
                break
            }
            if ($LauncherProcess.HasExited) {
                break
            }
            Start-Sleep -Milliseconds 500
        }

        if ($LauncherReady) {
            Add-Result "PASS" "Supervisor smoke" "Managed session reached ready state."
        }
        else {
            Add-Result "FAIL" "Supervisor smoke" "Managed session did not become ready within $SmokeTimeoutSeconds seconds."
        }
        if ($CameraReady -and $TrackingSent) {
            Add-Result "PASS" "Tracker smoke" "Webcam opened and tracking packets were produced."
        }
        else {
            Add-Result "FAIL" "Tracker smoke" "Webcam/tracking packet evidence was not found."
        }
        if ($SpoutReady) {
            Add-Result "PASS" "Spout smoke" "Sender registry reports 1280x720 with share handle $($SpoutInfo.ShareHandle)."
        }
        else {
            Add-Result "FAIL" "Spout smoke" "A valid 1280x720 sender was not found in the Spout shared-memory registry."
        }

        $CloseRequested = $false
        $ReleaseProcesses = @(Get-ReleaseProcesses)
        foreach ($ProcessInfo in $ReleaseProcesses | Sort-Object { $_.ExecutablePath.Length } -Descending) {
            if ($ProcessInfo.Name -notlike "VPPipeline*.exe") {
                continue
            }
            try {
                $Process = Get-Process -Id $ProcessInfo.ProcessId -ErrorAction Stop
                if ($Process.MainWindowHandle -ne 0 -and $Process.CloseMainWindow()) {
                    $CloseRequested = $true
                    break
                }
            }
            catch {
                continue
            }
        }
        if (-not $CloseRequested) {
            Add-Result "FAIL" "Window close" "Could not request a graceful close from the packaged Unreal window."
        }

        if ($CloseRequested -and -not $LauncherProcess.WaitForExit(45000)) {
            Add-Result "FAIL" "Lifecycle shutdown" "Launcher did not exit within 45 seconds after closing Unreal."
        }
        elseif ($CloseRequested) {
            Add-Result "PASS" "Lifecycle shutdown" "Closing Unreal ended the one-click launcher."
        }

        $ShutdownDeadline = (Get-Date).AddSeconds(15)
        $ShutdownLogged = $false
        while ((Get-Date) -lt $ShutdownDeadline) {
            if (Test-Path -LiteralPath $LauncherLog -PathType Leaf) {
                $LauncherText = Get-Content -LiteralPath $LauncherLog -Raw -ErrorAction SilentlyContinue
                $ShutdownLogged = $LauncherText -match "Pipeline stopped; managed webcam ownership released"
                if ($ShutdownLogged) {
                    break
                }
            }
            Start-Sleep -Milliseconds 250
        }
        if ($ShutdownLogged) {
            Add-Result "PASS" "Webcam release" "Supervisor recorded complete managed cleanup."
        }
        else {
            Add-Result "FAIL" "Webcam release" "Complete managed cleanup was not recorded."
        }
    }
    finally {
        if ($null -eq $PreviousLogSetting) {
            Remove-Item Env:VP_LOG_FILE -ErrorAction SilentlyContinue
        }
        else {
            $env:VP_LOG_FILE = $PreviousLogSetting
        }
        Stop-SmokeProcesses
        Start-Sleep -Milliseconds 500

        $Remaining = @(Get-ReleaseProcesses)
        if ($Remaining.Count -eq 0) {
            Add-Result "PASS" "Process cleanup" "No process remains under the release directory."
        }
        else {
            Add-Result "FAIL" "Process cleanup" "$($Remaining.Count) release process(es) remain."
        }
        $BusyPorts = @(@(7000, 7001) | Where-Object { -not (Test-PortAvailable $_) })
        if ($BusyPorts.Count -eq 0) {
            Add-Result "PASS" "UDP cleanup" "UDP 7000 and 7001 are available."
        }
        else {
            Add-Result "FAIL" "UDP cleanup" "Ports still occupied: $($BusyPorts -join ', ')"
        }
        if ($null -eq (Get-SpoutSenderInfo)) {
            Add-Result "PASS" "Spout cleanup" "Sender was removed from the Spout registry."
        }
        else {
            Add-Result "FAIL" "Spout cleanup" "Sender remains registered after application shutdown."
        }
    }
}

if (-not (Test-Path -LiteralPath $ReleaseRoot -PathType Container)) {
    throw "Release directory not found: $ReleaseRoot"
}

$AllFiles = @(Get-ChildItem -LiteralPath $ReleaseRoot -Recurse -File)
$TotalBytes = ($AllFiles | Measure-Object Length -Sum).Sum
Add-Result "INFO" "Release inventory" "$($AllFiles.Count) files, $([math]::Round($TotalBytes / 1MB, 2)) MiB"

$RequiredFiles = @(
    "VirtualProductionPipeline.exe",
    "Runtime\VPPipeline.exe",
    "Runtime\Engine\Extras\Redist\en-us\vc_redist.x64.exe",
    "START_HERE.txt",
    "README.md",
    "THIRD_PARTY_NOTICES.md"
)
foreach ($RelativePath in $RequiredFiles) {
    $Path = Join-Path $ReleaseRoot $RelativePath
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        Add-Result "PASS" "Required file" $RelativePath
    }
    else {
        Add-Result "FAIL" "Required file" "Missing: $RelativePath"
    }
}

$PakFiles = @($AllFiles | Where-Object { $_.Extension -in @(".pak", ".utoc", ".ucas") })
if (@($PakFiles | Where-Object Extension -eq ".pak").Count -gt 0 -and
    @($PakFiles | Where-Object Extension -eq ".utoc").Count -gt 0 -and
    @($PakFiles | Where-Object Extension -eq ".ucas").Count -gt 0) {
    Add-Result "PASS" "Unreal content" "Pak, IoStore TOC, and IoStore data are present."
}
else {
    Add-Result "FAIL" "Unreal content" "Pak/utoc/ucas output is incomplete."
}

foreach ($ModelName in @("face_landmarker.task", "pose_landmarker_full.task")) {
    if (@($AllFiles | Where-Object Name -eq $ModelName).Count -gt 0) {
        Add-Result "PASS" "MediaPipe model" $ModelName
    }
    else {
        Add-Result "FAIL" "MediaPipe model" "Missing: $ModelName"
    }
}

$ForbiddenChecks = @(
    [PSCustomObject]@{ Name = "User VRM"; Match = { param($File) $File.Extension -eq ".vrm" } },
    [PSCustomObject]@{ Name = "Environment secret"; Match = { param($File) $File.Name -like ".env*" } },
    [PSCustomObject]@{ Name = "OBS WebSocket module"; Match = { param($File) (Get-RelativeReleasePath $File.FullName) -match '(?i)(obsws|obs_control|obs_controller)' } },
    [PSCustomObject]@{ Name = "Project state document"; Match = { param($File) $File.Name -in @("PROJECT_STATE.md", "taskReport.md") } },
    [PSCustomObject]@{ Name = "Unreal project source"; Match = { param($File) $File.Extension -eq ".uproject" } }
)
if ($Configuration -eq "Shipping") {
    $ForbiddenChecks += [PSCustomObject]@{ Name = "ARM64 redistributable"; Match = { param($File) $File.Name -eq "vc_redist.arm64.exe" } }
    $ForbiddenChecks += [PSCustomObject]@{ Name = "Debug symbol"; Match = { param($File) $File.Extension -eq ".pdb" } }
}
foreach ($Rule in $ForbiddenChecks) {
    $Matches = @($AllFiles | Where-Object { & $Rule.Match $_ })
    if ($Matches.Count -eq 0) {
        Add-Result "PASS" "Forbidden content" "$($Rule.Name): none"
    }
    else {
        $Examples = @($Matches | Select-Object -First 3 | ForEach-Object { Get-RelativeReleasePath $_.FullName })
        Add-Result "FAIL" "Forbidden content" "$($Rule.Name): $($Matches.Count) found ($($Examples -join ', '))"
    }
}

$Launcher = Get-Item -LiteralPath (Join-Path $ReleaseRoot "VirtualProductionPipeline.exe") -ErrorAction SilentlyContinue
if ($null -ne $Launcher) {
    if ($Launcher.VersionInfo.ProductName -eq "Virtual Production Pipeline" -and
        $Launcher.VersionInfo.FileVersion -eq "0.1.0.0") {
        Add-Result "PASS" "Launcher metadata" "Virtual Production Pipeline 0.1.0.0"
    }
    else {
        Add-Result "FAIL" "Launcher metadata" "Product='$($Launcher.VersionInfo.ProductName)', FileVersion='$($Launcher.VersionInfo.FileVersion)'"
    }
}

$RuntimeBinary = $AllFiles | Where-Object {
    (Get-RelativeReleasePath $_.FullName) -match '(?i)^Runtime[\\/]VPPipeline[\\/]Binaries[\\/]Win64[\\/]VPPipeline.*\.exe$'
} | Select-Object -First 1
if ($null -ne $RuntimeBinary) {
    if ($RuntimeBinary.VersionInfo.ProductName -eq "Virtual Production Pipeline") {
        Add-Result "PASS" "Unreal metadata" "ProductName is Virtual Production Pipeline."
    }
    else {
        Add-Result "FAIL" "Unreal metadata" "ProductName='$($RuntimeBinary.VersionInfo.ProductName)'"
    }
}
else {
    Add-Result "FAIL" "Unreal metadata" "Packaged Win64 game binary was not found."
}

$ExecutablesToCheck = @($Launcher, (Get-Item -LiteralPath (Join-Path $ReleaseRoot "Runtime\VPPipeline.exe") -ErrorAction SilentlyContinue), $RuntimeBinary) | Where-Object { $null -ne $_ }
foreach ($Executable in $ExecutablesToCheck) {
    $Signature = Get-AuthenticodeSignature -LiteralPath $Executable.FullName
    $RelativePath = Get-RelativeReleasePath $Executable.FullName
    if ($Signature.Status -eq "Valid") {
        Add-Result "PASS" "Code signature" "${RelativePath}: valid"
    }
    elseif ($Signature.Status -eq "NotSigned") {
        Add-Result "WARN" "Code signature" "${RelativePath}: not signed"
    }
    else {
        Add-Result "FAIL" "Code signature" "${RelativePath}: $($Signature.Status)"
    }
}

if ($RunSmokeTest) {
    Invoke-SmokeTest
}

$ReportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$Report = [PSCustomObject]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    configuration = $Configuration
    releaseRoot = $ReleaseRoot
    fileCount = $AllFiles.Count
    totalBytes = $TotalBytes
    results = $Results
}
$Report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
Write-Host "Validation report: $ReportPath"

$FailureCount = @($Results | Where-Object status -eq "FAIL").Count
$WarningCount = @($Results | Where-Object status -eq "WARN").Count
if ($FailureCount -gt 0) {
    throw "Release validation failed: $FailureCount failure(s), $WarningCount warning(s)."
}
Write-Host "Release validation passed with $WarningCount warning(s)."

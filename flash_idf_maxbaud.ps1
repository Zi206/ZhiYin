<#
.SYNOPSIS
    Flash this project using the environment referenced by the IDF v5.5.4 PowerShell shortcut.

.PARAMETER Port
    Target CH340K serial port. If omitted, the script auto-selects the only
    connected USB-SERIAL CH340K device.

.PARAMETER Baud
    Flash baud rate. Defaults to 1152000.

.EXAMPLE
    .\flash_idf_maxbaud.ps1
    .\flash_idf_maxbaud.ps1 -Port COM14 -Baud 1152000
#>

param(
    [string]$Port = "",
    [ValidateSet(115200, 230400, 460800, 921600, 1152000)]
    [int]$Baud = 1152000
)

$ErrorActionPreference = "Stop"

$projectDir = $PSScriptRoot
$workspaceRoot = Split-Path (Split-Path $projectDir -Parent) -Parent
$buildDir = Join-Path $projectDir "build"
$cachePath = Join-Path $buildDir "CMakeCache.txt"
$binaryPath = Join-Path $buildDir "xiaozhi.bin"
$shortcutPath = Join-Path $workspaceRoot "06_Tools\Espressif\IDF_v5.5.4_Powershell.lnk"
$expectedHome = "CMAKE_HOME_DIRECTORY:INTERNAL=" + ($projectDir -replace "\\", "/")

foreach ($path in @($shortcutPath, $cachePath, $binaryPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required file is missing: $path"
    }
}

$cacheHome = (Select-String -LiteralPath $cachePath -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' -Encoding utf8).Line
if ($cacheHome -ne $expectedHome) {
    throw "The build directory belongs to a different project: $cacheHome"
}

$wch = New-Object -ComObject WScript.Shell
$shortcut = $wch.CreateShortcut($shortcutPath)
if ($shortcut.Arguments -notmatch "'([^']+PowerShell_profile\.ps1)'") {
    throw "Cannot resolve the IDF PowerShell profile from the shortcut: $($shortcut.Arguments)"
}
$idfProfile = $Matches[1]
if (-not (Test-Path -LiteralPath $idfProfile)) {
    throw "IDF PowerShell profile is missing: $idfProfile"
}

$ch340Devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match 'USB-SERIAL CH340K' })

if ($Port) {
    $portDevice = $ch340Devices |
        Where-Object { $_.FriendlyName -match "\($([regex]::Escape($Port))\)" } |
        Select-Object -First 1
    if (-not $portDevice) {
        throw "USB-SERIAL CH340K was not detected on $Port."
    }
} else {
    if ($ch340Devices.Count -eq 0) {
        throw "No USB-SERIAL CH340K device was detected. Connect the board and retry."
    }
    if ($ch340Devices.Count -gt 1) {
        $deviceList = ($ch340Devices.FriendlyName -join ', ')
        throw "Multiple USB-SERIAL CH340K devices were detected: $deviceList. Specify -Port explicitly."
    }
    $portDevice = $ch340Devices[0]
    if ($portDevice.FriendlyName -notmatch '\((COM\d+)\)') {
        throw "Cannot extract a COM port from: $($portDevice.FriendlyName)"
    }
    $Port = $Matches[1]
}

Write-Host "Project: $projectDir" -ForegroundColor Cyan
Write-Host "Binary: $binaryPath" -ForegroundColor Cyan
Write-Host "Port: $($portDevice.FriendlyName)" -ForegroundColor Cyan
Write-Host "Baud: $Baud" -ForegroundColor Cyan
Write-Host "Hold GPIO0 low and reset the board to enter ROM download mode." -ForegroundColor Yellow

. $idfProfile

$python = $env:IDF_PYTHON_ENV_PATH + "\Scripts\python.exe"
$idfPy = Join-Path $env:IDF_PATH "tools\idf.py"
& $python $idfPy -p $Port -b $Baud flash
if ($LASTEXITCODE -ne 0) {
    throw "Flash failed. idf.py exit code: $LASTEXITCODE"
}

Write-Host "Flash succeeded. The device has been reset." -ForegroundColor Green

# Rebuilds and redeploys whichever copy of the app is actually running.
#
# This asks the Service Control Manager where CecService.exe lives rather than
# assuming the repo, because once the installer has run the answer is Program
# Files. Building the repo's Debug binary and restarting the service then does
# nothing at all: the service starts the installed copy, the new code never
# runs, and the only clue is an absence in the log.
#
# Run from an ELEVATED PowerShell.

$ErrorActionPreference = 'Stop'

$root    = $PSScriptRoot
$name    = "CecService"

# Whichever Visual Studio is installed, found the way Microsoft recommends.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = $null
if (Test-Path $vswhere) {
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
}
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    throw "MSBuild not found. Install Visual Studio with the C++ desktop workload."
}

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated PowerShell."
}

# ---------------------------------------------------------------- where to
$svc = Get-CimInstance Win32_Service -Filter "Name='$name'" -ErrorAction SilentlyContinue
$registered = if ($svc) { ($svc.PathName -replace '"', '').Trim() } else { $null }
$installed  = $registered -and $registered -notlike "$root*"

if ($installed) {
    $target  = Split-Path $registered -Parent
    $config  = "Release"
    Write-Host "Service is installed at: $target" -ForegroundColor Yellow
    Write-Host "Building Release and deploying there." -ForegroundColor Yellow
} else {
    $target  = $null
    $config  = "Debug"
    Write-Host "Service runs from the repo. Building Debug in place." -ForegroundColor Cyan
}

# ---------------------------------------------------------------- build
$projects = @("CecTest\CecTest.vcxproj", "CecService\CecService.vcxproj")
if ($installed) { $projects += "CecTray\CecTray.vcxproj" }

foreach ($proj in $projects) {
    Write-Host "Building $proj ($config)..." -ForegroundColor Cyan
    & $msbuild (Join-Path $root $proj) -p:Configuration=$config -p:Platform=x64 `
        -v:minimal -nologo
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $proj" }
}

# ---------------------------------------------------------------- stop
$wasRunning = $svc -and $svc.State -eq 'Running'
if ($wasRunning) {
    Write-Host "Stopping $name..." -ForegroundColor Cyan
    Stop-Service -Name $name -Force
    (Get-Service -Name $name).WaitForStatus('Stopped', '00:00:30')
    Start-Sleep -Milliseconds 500   # let the file lock actually clear
}

$trayWasRunning = $false
if ($installed) {
    # The tray holds its own exe open, so it has to go before the copy.
    $tray = Get-Process CecTray -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -like "$target*" }
    if ($tray) {
        $trayWasRunning = $true
        Write-Host "Closing the tray app..." -ForegroundColor Cyan
        $tray | Stop-Process -Force
        Start-Sleep -Milliseconds 300
    }

    Copy-Item (Join-Path $root "CecService\x64\Release\CecService.exe") $target -Force
    Copy-Item (Join-Path $root "CecTray\x64\Release\CecTray.exe") $target -Force
    # The bench tool goes with them, so the diagnostic being run is always the
    # same build as the service being diagnosed. Nothing registers or launches
    # it; it is simply there when it is wanted.
    Copy-Item (Join-Path $root "CecTest\x64\Release\CecTest.exe") $target -Force
    Write-Host "Deployed to $target" -ForegroundColor Green
}

# ---------------------------------------------------------------- start
if ($svc) {
    Write-Host "Starting $name..." -ForegroundColor Cyan
    Start-Service -Name $name
    Write-Host "Running. Log: C:\ProgramData\CecService\service.log" -ForegroundColor Green
} else {
    Write-Host "$name is not installed; built only." -ForegroundColor Yellow
}

if ($trayWasRunning) {
    # Launched through explorer so it runs as the signed-in user rather than
    # inheriting this elevated shell's token, which would put its settings and
    # autostart entry in the wrong profile.
    Start-Process explorer.exe -ArgumentList (Join-Path $target "CecTray.exe")
    Write-Host "Tray app relaunched." -ForegroundColor Green
}

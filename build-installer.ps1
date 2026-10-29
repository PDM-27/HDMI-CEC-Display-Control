# Builds CecSetup.exe - the single file that installs and uninstalls everything.
#
# Order is not optional: CecSetup.rc embeds the Release builds of the service
# and the tray app as resources, so those have to exist before the resource
# compiler runs. Building the installer on its own after changing either one
# would silently ship the previous copy.
#
# Does NOT need elevation. The installer asks for it itself when run.

$ErrorActionPreference = 'Stop'

$root    = $PSScriptRoot
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

foreach ($project in @("CecTest\CecTest.vcxproj",
                       "CecService\CecService.vcxproj",
                       "CecTray\CecTray.vcxproj",
                       "CecSetup\CecSetup.vcxproj")) {
    Write-Host "Building $project (Release)..." -ForegroundColor Cyan
    & $msbuild (Join-Path $root $project) -p:Configuration=Release -p:Platform=x64 `
        -v:minimal -nologo
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $project" }
}

$setup = Join-Path $root "CecSetup\x64\Release\CecSetup.exe"
$size = [math]::Round((Get-Item $setup).Length / 1MB, 2)
Write-Host ""
Write-Host "Installer ready: $setup ($size MB)" -ForegroundColor Green

# Tests whether a GPU vendor's DisplayPort AUX channel works from Session 0 as
# SYSTEM, which is the environment a Windows Service actually runs in.
#
# The whole two-piece architecture (a SYSTEM service and a tray app) assumes this
# works.
# It was confirmed for NVAPI before any service code was written. That says
# nothing about another vendor: AMD's ADL and Intel's IGCL are separate
# libraries with their own ideas about whether they need a desktop, so each one
# has to be asked the same question rather than assumed to behave alike.
#
# Creates a temporary scheduled task, runs it, prints the output, removes it.
# Run from an ELEVATED PowerShell.
#
#   .\test-session0.ps1                            the repo build
#   .\test-session0.ps1 -Exe D:\CecTest.exe        a copy carried anywhere

param([string]$Exe)

$ErrorActionPreference = 'Stop'

# Release first: on a machine that is not the development one, that is the only
# build that will have been carried over.
$candidates = @(
    $Exe,
    (Join-Path $PSScriptRoot "CecTest\x64\Release\CecTest.exe"),
    (Join-Path $PSScriptRoot "CecTest\x64\Debug\CecTest.exe"),
    (Join-Path $PSScriptRoot "CecTest.exe")
) | Where-Object { $_ }

$exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { throw "CecTest.exe not found. Pass -Exe with the path to it." }
Write-Host "Testing: $exe" -ForegroundColor Cyan
$work = "C:\Windows\Temp\CecSession0"     # SYSTEM can always write here
$cmd  = Join-Path $work "probe.cmd"
$log  = Join-Path $work "probe.log"
$task = "CecSession0Test"

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run from an elevated PowerShell."
}

New-Item -ItemType Directory -Path $work -Force | Out-Null
Remove-Item $log -ErrorAction SilentlyContinue

# A batch file avoids all the nested-quoting problems of passing a compound
# command through the scheduled task action.
@"
@echo off
whoami > "$log" 2>&1
echo. >> "$log"
echo ===== list ===== >> "$log"
"$exe" list >> "$log" 2>&1
echo exitcode=%errorlevel% >> "$log"
echo. >> "$log"
echo ===== probe ===== >> "$log"
"$exe" probe >> "$log" 2>&1
echo exitcode=%errorlevel% >> "$log"
echo. >> "$log"
echo ===== cec-status ===== >> "$log"
"$exe" cec-status >> "$log" 2>&1
echo exitcode=%errorlevel% >> "$log"
"@ | Set-Content -Path $cmd -Encoding ASCII

Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue

$action    = New-ScheduledTaskAction -Execute $cmd
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" `
                                        -LogonType ServiceAccount `
                                        -RunLevel Highest
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null

Write-Host "Running probe as SYSTEM in Session 0..." -ForegroundColor Cyan
Start-ScheduledTask -TaskName $task

# Wait for it to actually enter Running before waiting for it to finish -
# a task reports Ready both before it starts and after it ends.
$started = $false
for ($i = 0; $i -lt 20; $i++) {
    if ((Get-ScheduledTask -TaskName $task).State -eq 'Running') { $started = $true; break }
    Start-Sleep -Milliseconds 500
}
if ($started) {
    for ($i = 0; $i -lt 60; $i++) {
        if ((Get-ScheduledTask -TaskName $task).State -ne 'Running') { break }
        Start-Sleep -Milliseconds 500
    }
} else {
    Start-Sleep -Seconds 5   # may have run and finished faster than we polled
}

$info = Get-ScheduledTaskInfo -TaskName $task
Write-Host ("Task last run: {0}   result: 0x{1:X8}" -f $info.LastRunTime, $info.LastTaskResult) `
           -ForegroundColor DarkGray

Unregister-ScheduledTask -TaskName $task -Confirm:$false
Write-Host "Temporary task removed.`n" -ForegroundColor DarkGray

Write-Host "===== OUTPUT FROM SESSION 0 =====" -ForegroundColor Yellow
if (Test-Path $log) { Get-Content $log } else { Write-Host "(log file was never created)" }
Write-Host "=================================" -ForegroundColor Yellow

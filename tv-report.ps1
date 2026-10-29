# tv-report.ps1 - collects one television's worth of CEC evidence.
#
# Run it at each television. It turns frame logging on, drives the switching over
# the service's own pipe so that every action is timestamped rather than
# remembered, asks what actually happened on screen, and writes one report file
# holding the answers, the frames, and what hardware it was all on. Then it puts
# frame logging back as it found it.
#
# Nothing here needs the source tree or the installation folder: every command
# goes down the same named pipe the tray app uses, so wherever the app lives and
# whatever the user profile is called, this works.
#
#   powershell -ExecutionPolicy Bypass -File tv-report.ps1
#
# It asks for administrator rights itself. Ctrl+C is safe at any point, and so is
# any error: the logging setting is put back either way.

$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------------ constants

$ServiceKey = 'HKLM:\SOFTWARE\CecService'
$LogPath    = Join-Path $env:ProgramData 'CecService\service.log'
$PipeName   = 'CecService'

# How long to leave a switch alone before asking what happened. Long enough that
# a television which is merely slow has finished, and longer than the app's own
# patience, so what is judged is the final outcome.
$SettleSeconds = 15

$script:PreviousLogging = 0
$script:LoggingChanged  = $false

# Whatever goes wrong, say so in a way that can be sent back, and put the one
# setting this touches back as it was.
trap {
    Write-Host ''
    Write-Host 'The script stopped with an error:' -ForegroundColor Red
    Write-Host "  $($_.Exception.Message)" -ForegroundColor Red
    Write-Host ("  line {0}: {1}" -f $_.InvocationInfo.ScriptLineNumber,
                $_.InvocationInfo.Line.Trim()) -ForegroundColor Red
    if ($script:LoggingChanged) {
        try {
            New-ItemProperty -Path $ServiceKey -Name LogCecTraffic -PropertyType DWord `
                -Value $script:PreviousLogging -Force | Out-Null
            Write-Host '  frame logging has been put back' -ForegroundColor DarkGray
        } catch { }
    }
    Write-Host ''
    Write-Host 'Send those red lines back and it can be fixed.' -ForegroundColor Yellow
    Write-Host ''
    break
}

# ------------------------------------------------------------------- helpers

# Asks for elevation itself rather than making somebody remember to, since this
# will be run at a television with a laptop balanced on something.
function Require-Admin {
    $me = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if ($me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { return }
    if (-not $PSCommandPath) {
        throw 'Run this from an elevated PowerShell. Changing the logging setting needs it.'
    }
    Write-Host 'Asking for administrator rights, which the logging setting needs...'
    # Quoted by hand, because Start-Process does not quote array arguments and
    # the path will have spaces in it sooner or later.
    $quoted = '"' + $PSCommandPath + '"'
    Start-Process -FilePath 'powershell' -Verb RunAs -ArgumentList @(
        '-NoExit', '-ExecutionPolicy', 'Bypass', '-File', $quoted)
    exit 0
}

# One request over the service's own pipe. Empty when the service did not answer.
#
# Everything this script asks for goes through here, the switching included:
# "CMD input 2" and "CMD tv-on" are the same requests the tray app and the
# command line send, so nothing has to be found on disk first. That matters in
# the field, where the app may have been installed anywhere or not at all.
function Send-Pipe([string]$request) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
        '.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect(5000)
        $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
        $bytes = [Text.Encoding]::ASCII.GetBytes("$request`n")
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
        $buffer = New-Object byte[] 4096
        $read = $pipe.Read($buffer, 0, $buffer.Length)
        if ($read -le 0) { return '' }
        return ([Text.Encoding]::ASCII.GetString($buffer, 0, $read)).Trim()
    } catch {
        return ''
    } finally {
        $pipe.Dispose()
    }
}

function Ask([string]$question) {
    while ($true) {
        $answer = (Read-Host "  $question (y/n)").Trim().ToLower()
        if ($answer -eq 'y' -or $answer -eq 'yes') { return 'yes' }
        if ($answer -eq 'n' -or $answer -eq 'no')  { return 'no' }
    }
}

# Every line the report keeps, in the order it happened.
$script:Notes = New-Object System.Collections.ArrayList
function Note([string]$text) {
    $stamp = (Get-Date).ToString('HH:mm:ss')
    [void]$script:Notes.Add("$stamp  $text")
    Write-Host "  $text" -ForegroundColor DarkGray
}

# A command to the service, recorded with whatever it answered.
function Run-Command([string]$command) {
    $reply = Send-Pipe "CMD $command"
    if (-not $reply) { $reply = '(no answer from the service)' }
    Note "$command -> $reply"
    return $reply
}

function Settle([int]$seconds, [string]$why) {
    Write-Host "  waiting $seconds seconds ($why)..." -ForegroundColor DarkGray
    Start-Sleep -Seconds $seconds
}

$script:Results = New-Object System.Collections.ArrayList
function Record([string]$test, [string]$outcome) {
    [void]$script:Results.Add([pscustomobject]@{ Test = $test; Outcome = $outcome })
}

# ---------------------------------------------------------------------- start

Require-Admin

Write-Host ''
Write-Host 'HDMI-CEC field report' -ForegroundColor Cyan
Write-Host '---------------------'
Write-Host ''

if (-not (Send-Pipe 'STATUS')) {
    throw 'The service is not answering on its pipe. Is it running? Try: sc query CecService'
}

$label = (Read-Host 'Television make and model, as printed on it').Trim()
if (-not $label) { $label = 'unnamed' }
$safeLabel = (($label -replace '[^A-Za-z0-9\-_ ]', '') -replace '\s+', '-')
if (-not $safeLabel) { $safeLabel = 'unnamed' }

Write-Host ''
Write-Host 'Which HDMI port is this PC plugged into? The report records what the' -ForegroundColor Gray
Write-Host 'app thinks as well, and the two disagreeing is itself worth knowing.' -ForegroundColor Gray
$claimedPort = (Read-Host 'HDMI port number for this PC').Trim()

# --------------------------------------------------- turn frame logging on

if (-not (Test-Path $ServiceKey)) { New-Item -Path $ServiceKey -Force | Out-Null }
try {
    $script:PreviousLogging =
        (Get-ItemProperty -Path $ServiceKey -Name LogCecTraffic -ErrorAction Stop).LogCecTraffic
} catch {
    $script:PreviousLogging = 0
}
New-ItemProperty -Path $ServiceKey -Name LogCecTraffic -PropertyType DWord -Value 1 -Force | Out-Null
$script:LoggingChanged = $true
# STATUS makes the service re-read its settings, so this takes effect at once.
Send-Pipe 'STATUS' | Out-Null

# Everything appended from here belongs to this session.
$logStart = 0
if (Test-Path $LogPath) { $logStart = (Get-Item $LogPath).Length }
$started = Get-Date

# ------------------------------------------------------------- what is there

$status  = Send-Pipe 'STATUS'
$devices = Send-Pipe 'DEVICES'
Note "STATUS: $status"
Note "DEVICES: $devices"

# "OK<tab>2.0.0.0|awake|onscreen||Chromecast<tab>..." - port, flags, then name.
$inputs = @()
foreach ($entry in ($devices -split "`t")) {
    if ($entry -notmatch '^\d+\.\d+\.\d+\.\d+\|') { continue }
    $fields = $entry -split '\|', 5
    $inputs += [pscustomobject]@{
        Port   = $fields[0]
        Digit  = $fields[0].Substring(0, 1)
        Awake  = $fields[1]
        ThisPc = $fields[3]
        Name   = if ($fields.Count -ge 5) { $fields[4] } else { '' }
    }
}

Write-Host ''
Write-Host 'The app can see these inputs:' -ForegroundColor Cyan
foreach ($one in $inputs) {
    $mine = if ($one.ThisPc -eq 'thispc') { ' (this PC)' } else { '' }
    Write-Host ("  HDMI {0}  {1}  [{2}]{3}" -f $one.Digit, $one.Name, $one.Awake, $mine)
}
if (-not $inputs) { Write-Host '  none, which is itself a result' -ForegroundColor Yellow }

Write-Host ''
$missing = (Read-Host 'Anything plugged in that is NOT listed above? (describe, or leave blank)').Trim()
if ($missing) { Note "plugged in but not listed: $missing" }

$others = @($inputs | Where-Object {
    $_.ThisPc -ne 'thispc' -and $_.Digit -match '^[1-4]$' })

# ------------------------------------------------------------------- tests
#
# One action, then a wait long enough that the answer is the outcome rather than
# a race, then a question about the picture - which is the one thing no log can
# see.

Write-Host ''
Write-Host 'Test 1: bring this PC on screen' -ForegroundColor Cyan
Run-Command 'tv-on' | Out-Null
Settle 8 'the set may be slow'
Record 'tv-on from another input' (Ask 'Is this PC on screen now?')

foreach ($one in $others) {
    Write-Host ''
    Write-Host ("Test 2: switch to HDMI {0} ({1}), ONE request" -f $one.Digit, $one.Name) -ForegroundColor Cyan
    Write-Host '  Do not touch anything. This is the measurement that matters most.' -ForegroundColor Yellow
    Run-Command ("input {0}" -f $one.Digit) | Out-Null
    Settle $SettleSeconds 'one request only'
    $moved = Ask ("Did the picture move to {0}?" -f $one.Name)
    Record ("single request to HDMI {0} ({1})" -f $one.Digit, $one.Name) $moved

    if ($moved -eq 'no') {
        Write-Host ''
        Write-Host '  Asking a second time, which is the workaround being investigated.' -ForegroundColor Cyan
        Run-Command ("input {0}" -f $one.Digit) | Out-Null
        Settle $SettleSeconds 'second request'
        Record ("second request to HDMI {0}" -f $one.Digit) `
               (Ask ("Did the picture move to {0} this time?" -f $one.Name))
    }

    Write-Host ''
    Write-Host '  Coming back to this PC.' -ForegroundColor Cyan
    Run-Command 'tv-on' | Out-Null
    Settle 10 'coming back'
    Record ("tv-on back from HDMI {0}" -f $one.Digit) (Ask 'Is this PC back on screen?')
}

# The case that always failed on the development set: a request sent while the
# television is still finishing the last one.
if ($others.Count -ge 1) {
    $target = $others[0]
    Write-Host ''
    Write-Host ("Test 3: switch to HDMI {0} the instant we leave this PC" -f $target.Digit) -ForegroundColor Cyan
    Write-Host '  No pause between the two, on purpose.' -ForegroundColor Yellow
    Run-Command 'tv-on' | Out-Null
    Start-Sleep -Milliseconds 300
    Run-Command ("input {0}" -f $target.Digit) | Out-Null
    Settle $SettleSeconds 'one request, sent while the set was busy'
    Record ("request sent while the set was mid-switch (HDMI {0})" -f $target.Digit) `
           (Ask ("Did the picture move to {0}?" -f $target.Name))
    Run-Command 'tv-on' | Out-Null
    Settle 10 'coming back'
}

Write-Host ''
Write-Host 'Test 4: does the TV pass its remote through?' -ForegroundColor Cyan
Write-Host '  With this PC on screen, press a few buttons on the TV remote:' -ForegroundColor Gray
Write-Host '  the arrows, OK, back, and play or pause if it has them.' -ForegroundColor Gray
Read-Host '  Press Enter when done' | Out-Null
Record 'TV remote passthrough' (Ask 'Did anything happen on the PC?')

# ------------------------------------------------------------------- collect

Write-Host ''
Write-Host 'Collecting...' -ForegroundColor Cyan

$logSection = ''
if (Test-Path $LogPath) {
    $stream = [System.IO.File]::Open($LogPath, 'Open', 'Read', 'ReadWrite')
    try {
        if ($logStart -gt $stream.Length) { $logStart = 0 }   # the log rotated
        [void]$stream.Seek($logStart, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream)
        $logSection = $reader.ReadToEnd()
    } finally {
        $stream.Dispose()
    }
}

$gpu = ((Get-CimInstance Win32_VideoController).Name) -join '; '
$osInfo = Get-CimInstance Win32_OperatingSystem
$os = "$($osInfo.Caption) build $($osInfo.BuildNumber)"
$monitors = 'unavailable'
try {
    $monitors = ((Get-CimInstance -Namespace 'root\wmi' -ClassName WmiMonitorID |
        ForEach-Object {
            $name = (($_.UserFriendlyName | Where-Object { $_ -gt 0 }) |
                     ForEach-Object { [char]$_ }) -join ''
            $make = (($_.ManufacturerName | Where-Object { $_ -gt 0 }) |
                     ForEach-Object { [char]$_ }) -join ''
            "$make $name"
        }) -join '; ')
} catch { }

# Which build produced this, taken from the log rather than from a path, since
# the app may live anywhere.
$version = 'unknown'
if (Test-Path $LogPath) {
    $starts = @(Select-String -Path $LogPath -Pattern 'service starting, version (.+)$' -AllMatches)
    if ($starts.Count -gt 0) {
        $version = $starts[-1].Matches[0].Groups[1].Value.Trim()
    }
}

$lines = $logSection -split "`r?`n"
# Routing frames the television sent of its own accord. Zero means the set never
# announces routing, so nothing can be waited for or confirmed and a device's own
# claim is the whole of the evidence - which an Amazon Fire TV proved is enough.
$tvRouting = @($lines | Where-Object { $_ -match '\[route\] 0->' })
$refusals  = @($lines | Where-Object { $_ -match 'NOT ACKNOWLEDGED' })
$malformed = @($lines | Where-Object { $_ -match 'operand count|receive overflow' })

$script:Report = New-Object System.Collections.ArrayList
function Out-Line([string]$text) { [void]$script:Report.Add($text) }

Out-Line 'HDMI-CEC field report'
Out-Line '====================='
Out-Line ''
Out-Line "television          : $label"
Out-Line "this PC is on HDMI  : $claimedPort (as reported by the person)"
Out-Line "run at              : $($started.ToString('yyyy-MM-dd HH:mm:ss')) local"
Out-Line "machine             : $env:COMPUTERNAME"
Out-Line "os                  : $os"
Out-Line "gpu                 : $gpu"
Out-Line "monitors per windows: $monitors"
Out-Line "app version         : $version (from the last service start in the log)"
Out-Line ''
Out-Line "service STATUS      : $status"
Out-Line "service DEVICES     : $devices"
if ($missing) { Out-Line "plugged in but not listed: $missing" }
Out-Line ''
Out-Line 'What the television did'
Out-Line '-----------------------'
foreach ($row in $script:Results) {
    Out-Line ("  {0,-52} {1}" -f $row.Test, $row.Outcome)
}
Out-Line ''
Out-Line "Routing frames the television sent itself : $($tvRouting.Count)"
Out-Line "Frames refused on the wire               : $($refusals.Count)"
Out-Line "Malformed frames or receive overflows    : $($malformed.Count)"
Out-Line ''
Out-Line 'Timeline of what the script did'
Out-Line '------------------------------'
foreach ($line in $script:Notes) { Out-Line "  $line" }
Out-Line ''
Out-Line 'Service log for this session'
Out-Line '---------------------------'
Out-Line $logSection

$folder = [Environment]::GetFolderPath('Desktop')
if (-not $folder) { $folder = $env:USERPROFILE }
$stamp = $started.ToString('yyyyMMdd-HHmmss')
$out = Join-Path $folder "tv-report-$safeLabel-$env:COMPUTERNAME-$stamp.txt"
($script:Report -join "`r`n") | Set-Content -Path $out -Encoding UTF8

# ------------------------------------------------- put the setting back

New-ItemProperty -Path $ServiceKey -Name LogCecTraffic -PropertyType DWord `
    -Value $script:PreviousLogging -Force | Out-Null
$script:LoggingChanged = $false
Send-Pipe 'STATUS' | Out-Null

Write-Host ''
Write-Host 'Report written to:' -ForegroundColor Green
Write-Host "  $out"
Write-Host ''
Write-Host 'Frame logging has been put back as it was. Keep the file and send it on.'
Write-Host ''

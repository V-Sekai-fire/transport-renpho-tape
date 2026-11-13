# Install renpho-tape as a logon task. WinRT Bluetooth needs the user's desktop
# session, so a session-0 Windows service cannot work; a logon task that restarts
# on failure is the equivalent.
#   powershell -ExecutionPolicy Bypass -File install-windows.ps1 [-Dir C:\renpho-tape]
param([string]$Dir = "$env:ProgramFiles\renpho-tape")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Dir | Out-Null
Copy-Item "$PSScriptRoot\..\renpho-tape.exe", "$PSScriptRoot\..\renpho_ble_win.dll" $Dir -Force
$log = Join-Path $env:LOCALAPPDATA 'renpho-tape.jsonl'
$exe = Join-Path $Dir 'renpho-tape.exe'
$action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"`"$exe`" serve >> `"$log`" 2>&1`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
  -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName 'renpho-tape' -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName 'renpho-tape'
Write-Host "installed; readings append to $log"

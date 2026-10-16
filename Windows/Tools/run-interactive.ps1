# Runs a command in the signed-in user's desktop session (from ssh, which has no desktop), e.g.
#   powershell -File run-interactive.ps1 -Command 'C:\path\AsyncMusic.exe'
# With -Wait, waits for it to exit (up to -Timeout seconds).
param(
    [Parameter(Mandatory = $true)][string]$Command,
    [string]$Arguments = "",
    [switch]$Wait,
    [int]$Timeout = 60
)
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$name = "AsyncMusic-Interactive-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
$actionParameters = @{ Execute = $Command; WorkingDirectory = (Split-Path -Parent $Command) }
if ($Arguments) { $actionParameters.Argument = $Arguments }
$action = New-ScheduledTaskAction @actionParameters
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 12)
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $name
if ($Wait) {
    $deadline = (Get-Date).AddSeconds($Timeout)
    Start-Sleep -Milliseconds 500
    while ((Get-ScheduledTask -TaskName $name).State -eq "Running" -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
}
Unregister-ScheduledTask -TaskName $name -Confirm:$false

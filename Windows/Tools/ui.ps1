# Drives the app through UI Automation, for testing from ssh. Needs the desktop session, so
# ui-remote.cmd runs it through run-interactive.ps1. Reads one request per line from
# %USERPROFILE%\ui-request.txt and writes what happened to %USERPROFILE%\ui.log:
#   invoke <name>     clicks the element with that automation name
#   dump              lists the named elements
#   rects             lists every element with its bounds (physical pixels)
#   scroll <percent>  scrolls the visible scroll viewer vertically
#   window <name>     acts on another top-level window from here on (e.g. a file dialog)
#   setvalue <automation id>|<text>   types into a field
#   press <automation id>             clicks a button by automation ID
param([string]$Window = "AsyncMusic")
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$out = @()
try {
    $root = [Windows.Automation.AutomationElement]::RootElement
    $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $Window)
    $app = $root.FindFirst([Windows.Automation.TreeScope]::Children, $condition)
    if ($null -eq $app) { throw "Window '$Window' not found" }
    foreach ($request in Get-Content -Encoding utf8 "$env:USERPROFILE\ui-request.txt") {
        if ($request.StartsWith("window ")) {
            $title = $request.Substring(7)
            $app = $null
            for ($attempt = 0; $attempt -lt 20 -and $null -eq $app; $attempt++) {
                $named = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $title)
                $isWindow = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Window)
                $condition = New-Object Windows.Automation.AndCondition($named, $isWindow)
                $app = $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
                if ($null -eq $app) { Start-Sleep -Milliseconds 250 }
            }
            if ($null -eq $app) { throw "Window '$title' not found" }
            $out += "Window $title"
        } elseif ($request.StartsWith("setvalue ")) {
            $id, $text = $request.Substring(9).Split("|", 2)
            $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
            $field = $null
            foreach ($candidate in $app.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)) {
                $pattern = $null
                if ($candidate.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$pattern)) { $field = $pattern; break }
            }
            if ($null -eq $field) { throw "No field '$id'" }
            $field.SetValue($text)
            $out += "Set $id"
        } elseif ($request.StartsWith("press ")) {
            $id = $request.Substring(6)
            $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
            $button = $app.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
            if ($null -eq $button) { throw "No button '$id'" }
            $button.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
            $out += "Pressed $id"
            Start-Sleep -Milliseconds 300
        } elseif ($request -eq "dump") {
            foreach ($element in $app.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
                $name = $element.Current.Name
                if ($name) { $out += "$($element.Current.ControlType.ProgrammaticName)`t$name" }
            }
        } elseif ($request -eq "rects") {
            $out += "Window`t$($app.Current.BoundingRectangle)"
            foreach ($element in $app.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
                $out += "$($element.Current.ControlType.ProgrammaticName)`t$($element.Current.ClassName)`t$($element.Current.Name)`t$($element.Current.BoundingRectangle)`t$(if ($element.Current.IsOffscreen) { 'offscreen' })"
            }
        } elseif ($request.StartsWith("scroll ")) {
            $percent = [double]$request.Substring(7)
            $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::IsScrollPatternAvailableProperty, $true)
            foreach ($candidate in $app.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)) {
                if (-not $candidate.Current.IsOffscreen) {
                    $candidate.GetCurrentPattern([Windows.Automation.ScrollPattern]::Pattern).SetScrollPercent(-1, $percent)
                    $out += "Scrolled to $percent%"
                    break
                }
            }
            Start-Sleep -Milliseconds 300
        } elseif ($request.StartsWith("invoke ")) {
            $target = $request.Substring(7)
            $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $target)
            # The first element of that name that can be clicked (tooltips share their owner's name).
            $element = $null
            foreach ($candidate in $app.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)) {
                $pattern = $null
                if ($candidate.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
                    $element = $pattern
                    break
                }
            }
            if ($null -eq $element) { throw "No clickable element '$target'" }
            $element.Invoke()
            $out += "Invoked $target"
            Start-Sleep -Milliseconds 300
        }
    }
} catch {
    $out += "ERROR $_"
}
$out | Out-File -Encoding utf8 "$env:USERPROFILE\ui.log"

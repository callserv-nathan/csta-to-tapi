# Read-only inventory; never invokes controls or reads password fields.
[CmdletBinding()]
param(
    [ValidateRange(1, 16)][int]$MaxDepth = 8,
    [ValidateRange(1, 2000)][int]$MaxElements = 500,
    [switch]$IncludeNames,
    [switch]$IncludeValues
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$processes = @(Get-Process -Name zac -ErrorAction SilentlyContinue)
$report = [ordered]@{
    SchemaVersion = 1
    Status = 'not-running'
    InspectorSessionId = (Get-Process -Id $PID).SessionId
    Processes = @()
    Elements = @()
    Truncated = $false
    Errors = @()
}
$items = New-Object 'System.Collections.Generic.List[object]'
$errors = New-Object 'System.Collections.Generic.List[string]'
$queue = New-Object 'System.Collections.Generic.Queue[object]'
$walker = [System.Windows.Automation.TreeWalker]::RawViewWalker

foreach ($process in $processes) {
    $report.Processes += [ordered]@{
        Id = $process.Id
        SessionId = $process.SessionId
        Path = $process.Path
        Version = $process.FileVersion
    }
    try {
        $condition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
            [int]$process.Id)
        $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        foreach ($window in $windows) {
            $queue.Enqueue(@{ Element = $window; Depth = 0; Parent = $null })
        }
    } catch {
        $errors.Add("Window enumeration failed: $($_.Exception.GetType().FullName)")
    }
}

while ($queue.Count -gt 0 -and $items.Count -lt $MaxElements) {
    $entry = $queue.Dequeue()
    try {
        $element = $entry.Element
        $current = $element.Current
        $index = $items.Count
        $item = [ordered]@{
            Index = $index
            Parent = $entry.Parent
            Depth = $entry.Depth
            ProcessId = $current.ProcessId
            ControlType = $current.ControlType.ProgrammaticName
            AutomationId = $current.AutomationId
            ClassName = $current.ClassName
            Enabled = $current.IsEnabled
            Offscreen = $current.IsOffscreen
            Password = $current.IsPassword
            Patterns = @($element.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName })
        }
        if ($IncludeNames -and -not $current.IsPassword) {
            $item.Name = $current.Name
            $item.HelpText = $current.HelpText
        }
        if ($IncludeValues -and -not $current.IsPassword) {
            $valuePattern = $null
            if ($element.TryGetCurrentPattern(
                    [System.Windows.Automation.ValuePattern]::Pattern,
                    [ref]$valuePattern)) {
                $item.Value = $valuePattern.Current.Value
                $item.ValueReadOnly = $valuePattern.Current.IsReadOnly
            }
        }
        $items.Add($item)
        $child = $walker.GetFirstChild($element)
        if ($null -ne $child -and $entry.Depth -ge $MaxDepth) {
            $report.Truncated = $true
        }
        while ($null -ne $child -and $entry.Depth -lt $MaxDepth) {
            if (($items.Count + $queue.Count) -ge $MaxElements) {
                $report.Truncated = $true
                break
            }
            # Stay inside ZAC even if another provider exposes an embedded window.
            if ($child.Current.ProcessId -eq $current.ProcessId) {
                $queue.Enqueue(@{ Element = $child; Depth = $entry.Depth + 1; Parent = $index })
            }
            $child = $walker.GetNextSibling($child)
        }
    } catch {
        $errors.Add("Element inspection failed: $($_.Exception.GetType().FullName)")
    }
}
if ($queue.Count -gt 0) { $report.Truncated = $true }
if ($processes.Count -gt 0) {
    $report.Status = if ($items.Count -gt 0) { 'inspected' } else { 'no-accessible-windows' }
}
$report.Elements = @($items.ToArray())
$report.Errors = @($errors.ToArray())
[pscustomobject]$report | ConvertTo-Json -Depth 8

# Observes the visible call UI. This is not a TAPI event source yet.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$raw = & (Join-Path $PSScriptRoot 'Inspect-ZacDesktop.ps1') -MaxDepth 16 -MaxElements 1000 -IncludeNames
$probe = $raw | ConvertFrom-Json
$elements = @($probe.Elements)
$rows = @()
$actions = @()
$destinations = @()

# Restrict call observations to SessionItemView; RecentItemView and the transfer
# directory contain people too, but must never be treated as current calls.
foreach ($table in @($elements | Where-Object ClassName -eq 'SessionItemView')) {
    foreach ($row in @($elements | Where-Object {
        $_.Parent -eq $table.Index -and $_.ControlType -eq 'ControlType.DataItem'
    })) {
        $rows += [ordered]@{
            ProcessId = $row.ProcessId
            DisplayText = $row.Name
            Visible = -not $row.Offscreen
            # Neither an element index nor a displayed number is a call ID.
            CallId = $null
            State = $null
            OriginalCalledNumber = $null
        }
    }
}
foreach ($panel in @($elements | Where-Object ClassName -eq 'DialCallPanelActionWidget')) {
    foreach ($button in @($elements | Where-Object {
        $_.Parent -eq $panel.Index -and $_.ClassName -eq 'ZCompositeButton'
    })) {
        $labels = @($elements | Where-Object {
            $_.Parent -eq $button.Index -and $_.ClassName -eq 'QLabel' -and $_.Name
        })
        foreach ($label in $labels) {
            $actions += [ordered]@{
                Label = $label.Name
                Enabled = $button.Enabled
                Visible = -not $button.Offscreen
                InvokeAdvertised = 'InvokePatternIdentifiers.Pattern' -in $button.Patterns
                InvocationTested = $false
            }
        }
    }
}
foreach ($panel in @($elements | Where-Object ClassName -eq 'CallOperationView')) {
    foreach ($edit in @($elements | Where-Object {
        $_.Parent -eq $panel.Index -and $_.ClassName -eq 'SearchEdit'
    })) {
        $destinations += [ordered]@{
            Enabled = $edit.Enabled
            Visible = -not $edit.Offscreen
            ValuePatternAdvertised = 'ValuePatternIdentifiers.Pattern' -in $edit.Patterns
            # This panel class alone does not identify blind vs attended transfer.
            Mode = $null
        }
    }
}
[ordered]@{
    SchemaVersion = 1
    Observation = 'visible-ui-snapshot'
    ProbeStatus = $probe.Status
    Truncated = $probe.Truncated
    Errors = @($probe.Errors)
    CallRows = $rows
    ActionCandidates = $actions
    CallOperationInputs = $destinations
    # Absence of a row may mean hidden UI; never emit DISCONNECTED from this alone.
    AuthoritativeCallState = $false
} | ConvertTo-Json -Depth 6

# Desktop feasibility probe

Run from WSL in this repository:

```bash
powershell.exe -NoProfile -NonInteractive -File "$(wslpath -w "$PWD/tools/Inspect-ZacDesktop.ps1")"
```

If the local script execution policy rejects this command, add
`-ExecutionPolicy Bypass` to this PowerShell invocation. This applies only to the
new process and does not change the persistent Windows execution policy. This
was required and tested on the development machine.

Or run `./tools/Inspect-ZacDesktop.ps1` in Windows PowerShell from a Windows
checkout. ZAC must be open in the same Windows session. The probe reports process
identity, accessible controls, and supported automation patterns as JSON. It does
not launch ZAC, invoke buttons, change focus, send pipe messages, or place calls.

Names are omitted by default because they can contain contact details. Add
`-IncludeNames` when deliberately inspecting a test screen to identify controls;
this includes help text. `-IncludeValues` additionally reads non-password value
properties. Password fields are never read. Keep real call captures out of source
control.

`not-running` means no ZAC process was found. `no-accessible-windows` can mean
another session, a hidden/tray-only application, or inaccessible controls; it does
not prove UI automation is impossible. `Truncated` reports the depth/element
limits. Individual UI Automation calls depend on the target application's
responsiveness; terminate the probe if ZAC's accessibility provider hangs.

An `InvokePattern` on a button only establishes an automation candidate. Stable
call identity, original DID, simultaneous-call handling, and transfer workflows
must still be validated before implementing the bridge.

## Call-only observation

`Get-ZacCallSnapshot.ps1` uses the probe to report visible `SessionItemView` rows,
labelled call-action candidates, and the call-operation destination input. It
excludes recent-call history and transfer-directory rows from its call list.

```bash
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$(wslpath -w "$PWD/tools/Get-ZacCallSnapshot.ps1")"
```

This reads displayed caller text, so its output can contain personal information.
It never invokes controls. `CallId`, `State`, and `OriginalCalledNumber` remain
null because they have not been established from the accessible UI. The output
is a snapshot of visible controls, not authoritative PBX state: changing tabs,
minimizing ZAC, or closing a panel must not be translated into a disconnected
call. `InvocationTested` is false even when Windows advertises `InvokePattern`.

Live validation on ZAC 8.4.34 detected one test call and one destination input
with the transfer panel open, without truncation or inspection errors. The
separate desktop probe also observed labelled transfer and hold controls before
the panel was opened. No action has been invoked by either script.

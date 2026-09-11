# Provider registration helper

`ZultysNCallProviderReg.exe` is the x64 elevated helper used by the MSI. It
uses the documented Windows TAPI APIs rather than editing TAPI registry state
directly.

- `install` verifies the sibling `ZultysNCallTsp.tsp` installed under
  `%ProgramFiles%\Zultys nCall Bridge\`, calls `lineAddProviderW` with its
  absolute path, and stores the returned permanent provider ID in
  `HKLM\SOFTWARE\Zultys\NCallBridge\ProviderId`.
- `uninstall` reads that ID, calls `lineRemoveProvider`, then clears the value.
- `rollback-install` uses the same removal path for a failed initial MSI
  transaction.

The helper refuses to overwrite a nonzero provider ID. This makes the MSI
rollback action safe: its install action only succeeds after registering a new
provider ID. It does not accept MX credentials or call data.

Build it on Windows x64 with the Windows SDK and MSVC:

```powershell
cmake -S src/ProviderRegistration -B out/ProviderRegistration -G "Visual Studio 17 2022" -A x64
cmake --build out/ProviderRegistration --config RelWithDebInfo
```

Do not invoke its commands on a working desktop during development. Registration
changes machine-wide TAPI state and belongs in the MSI validation VM.

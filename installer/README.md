# MSI package

This directory contains the x64, per-machine WiX Toolset 6 package for the
implemented bridge. A normal package installs the bridge service and native TSP,
then uses the registration helper to add the provider to Windows TAPI.

The package:

- installs the service, its self-contained .NET runtime, the configuration and
  diagnostics utilities, and the registration helper under
  %ProgramFiles%\Zultys nCall Bridge\;
- installs ZultysNCallTsp.tsp beside the service and registration helper under
  %ProgramFiles%\Zultys nCall Bridge\;
- creates the ProgramData directory used for config.json;
- creates ZultysNCallBridge as an automatic LocalService service with restart
  recovery;
- runs lineAddProvider through the deferred registration helper, records the
  permanent provider ID, starts the service, and schedules the required TAPI
  activation restart; and
- removes the provider by its recorded ID before removing its files during
  uninstall. An initial-install failure invokes the rollback helper.

Configuration is supplied after installation. The configuration utility protects
the MX password with machine-scope DPAPI and applies a protected DACL to the
configuration directory and file: SYSTEM and Administrators have full control;
LocalService has read and execute access. When Windows resolves the bridge
service SID, it receives the same access. The service checks for a completed
configuration every two seconds while it is unconfigured, so the first
configuration does not require a service restart.

## Payload contract

The WiX project deliberately has no source-tree payload defaults. It validates
the following MSBuild properties before packaging.

| Property | Required payload | Target location |
| --- | --- | --- |
| BridgeServiceExe | self-contained ZultysNCallBridge.exe | %ProgramFiles%\Zultys nCall Bridge\ |
| BridgeRuntimePayload | required service-runtime directory, excluding the service executable | %ProgramFiles%\Zultys nCall Bridge\ |
| ConfigExe | self-contained single-file ZultysNCallConfig.exe | %ProgramFiles%\Zultys nCall Bridge\ |
| DiagnosticsExe | self-contained single-file ZultysNCallDiag.exe | %ProgramFiles%\Zultys nCall Bridge\ |
| TspDll | x64 ZultysNCallTsp.tsp | %ProgramFiles%\Zultys nCall Bridge\ |
| ProviderRegistrationExe | x64 ZultysNCallProviderReg.exe | %ProgramFiles%\Zultys nCall Bridge\ |

EnableProviderRegistration defaults to true. Setting it to false creates a
package suitable only for packaging inspection; it does not register a usable
TAPI provider and should not be used for a functional install.

The registration helper owns
HKLM\Software\Zultys\NCallBridge\ProviderId. It rejects an attempt to
replace an existing nonzero ID, which makes rollback safe after a successful
initial registration. It receives no MX credentials, MX addresses, or call data.

## Windows build

Run [build.ps1](../build.ps1) from a 64-bit Windows build machine. It needs:

- the .NET SDK selected by [global.json](../global.json);
- Visual Studio Build Tools with the MSVC x64/x86 toolchain and a Windows SDK
  that contains TAPI headers and SignTool;
- CMake 3.25 or newer; and
- access to NuGet so the WiX Toolset SDK and its Util extension can restore.

Run the build from an NTFS checkout. If the source lives in WSL's Linux
filesystem, first copy or clone it into a Windows working directory; direct
builds through `\\wsl.localhost` can deny access to generated intermediates.

The script restores and tests every test project, publishes the service for
win-x64, publishes the configuration utility as a single-file executable,
builds both native x64 binaries, stages the payloads, and invokes WiX. It writes
the result to artifacts\ZultysNCall-<version>-x64.msi.

~~~powershell
.\build.ps1 -Version 0.1.0
~~~

That command makes an unsigned development MSI. A release build signs every
staged executable, DLL, and TSP before WiX packages them, then signs the MSI:

~~~powershell
.\build.ps1 -Version 0.1.0 -CertificateThumbprint '<certificate thumbprint>' -RequireSignedPayloads
~~~

The signing certificate must already be available in the build machine's
certificate store. -TimestampUrl can point to the release team's timestamp
service. The script does not install or register the output.

For a manually staged build, use the same properties explicitly:

~~~powershell
dotnet build .\installer\ZultysNCall.wixproj -c Release -p:ProductVersion=0.1.0 -p:BridgeServiceExe=C:\staging\bridge\ZultysNCallBridge.exe -p:BridgeRuntimePayload=C:\staging\bridge\runtime -p:ConfigExe=C:\staging\config\ZultysNCallConfig.exe -p:DiagnosticsExe=C:\staging\diagnostics\ZultysNCallDiag.exe -p:TspDll=C:\staging\tsp\ZultysNCallTsp.tsp -p:ProviderRegistrationExe=C:\staging\registration\ZultysNCallProviderReg.exe
~~~

## Windows validation

Run installation validation on a disposable x64 Windows VM that has the target
nCall release. The default MSI changes machine-wide TAPI provider state.

1. Install with msiexec /i ZultysNCall-<version>-x64.msi /l*v install.log,
   then restart Windows when setup requests it. A new TAPI provider is activated
   after the restart.
2. From an elevated terminal, run ZultysNCallConfig.exe set with the MX
   endpoint, operator username, and device ID. It prompts for the password.
3. Confirm the bridge health with ZultysNCallDiag health, enumerate the
   Zultys nCall Bridge line, then select that line in nCall's Generic TAPI
   configuration and restart nCall.
4. Exercise an inbound screen pop, outbound call, blind transfer, and assisted
   transfer. Record the Windows Event Log and MSI log for any failure.
5. Test repair, rollback, major upgrade, uninstall, and reinstall on fresh VMs.

The source has not yet been accepted through a clean-VM TAPI/nCall installation
test or a live upgraded MX endpoint. Those tests remain required before a
production release.

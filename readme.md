# Zultys to nCall TAPI bridge

This repository contains the implementation of a Windows TAPI bridge for nSolve
nCall. It exposes one native x64 TAPI Service Provider (TSP), while a separate
Windows service connects to the Zultys MX WebSocket API as a 3rdParty client.
ZAC remains the operator's desktop and media client.

The bridge is intended to give nCall the call events and controls it needs for:

- caller screen pops;
- outbound calls;
- hold and retrieve;
- blind transfer; and
- assisted transfer.

ZAC itself does not expose a TAPI provider. The bridge does not automate ZAC or
reuse its private desktop protocol. It controls the configured MX-bound device
through the documented MX WebSocket/CSTA interface.

## Current implementation

Milestones 1 and 2 now have working source:

- Bridge.Core implements CSTA framing, XML request and event handling, a
  deterministic call-state reducer, and the versioned local pipe contract.
- Bridge.Service is a .NET 10 Windows service that authenticates, monitors the
  configured device, executes supported call-control commands, publishes call
  snapshots and deltas, and accepts multiple local pipe clients.
- MxSimulator provides deterministic inbound-call and control scenarios without
  an MX server.
- Tsp.Provider is a native x64 TSPI provider. It subscribes to bridge call
  deltas, creates TAPI calls with caller identity and retains original-called
  identity when later events supply it, notifies TAPI of that identity update,
  maps outgoing and transfer operations to the service, and completes TSPI
  requests asynchronously.
- ProviderRegistration calls lineAddProvider and lineRemoveProvider and stores
  the permanent provider ID needed for uninstall and rollback.
- installer and build.ps1 stage the payloads and build a per-machine x64 WiX
  MSI with provider registration enabled by default.

The managed projects and simulator run under the pinned .NET SDK. The native
provider and registration helper have compiled with the Windows SDK and MSVC.
The bridge/simulator pipe path has also been exercised with a persistent
subscriber and a separate command client.

## Deployment target

The target application is the 32-bit nCall 5.4.4.477 executable at:

    C:\Program Files (x86)\nSolve\nCall\nCall.exe

Windows TAPI marshals that 32-bit client to the 64-bit TAPISRV process. TAPISRV
loads the provider in-process, so it must remain x64 even though nCall is
32-bit. The MSI installs it beside the bridge service and registers that
absolute path with TAPI.

The current MX 16.0.4 and ZAC 8.4.34 installation is a reference environment,
not the target integration. The live integration is planned for the latest
supported MX/ZAC release, currently MX Release 19 and ZAC 10.0.10, subject to
confirmation against the deployment system. The reference MXTSP can be used in
an isolated session to record nCall's public TAPI behavior; it is not a
dependency or redistributable component of this product.

## Architecture

    32-bit nCall
          |
    TAPI32 marshalling
          |
    64-bit TAPISRV
          |
    ZultysNCallTsp.tsp
          |
    \\.\pipe\Zultys.NCall.Bridge.v1
          |
    ZultysNCallBridge Windows service
          |
    TLS WebSocket, client type 3rdParty
          |
    Zultys MX

    ZAC, client type MXIE  ---------------------->  Zultys MX and the same bound device

The service has no desktop-session dependency. It starts automatically under LocalService,
stores its configuration in ProgramData, and checks for a completed first-run
configuration every two seconds. The configuration utility encrypts the MX
password with machine-scope DPAPI and restricts the configuration file and
directory to SYSTEM, Administrators, and LocalService. When Windows resolves
the bridge service SID, it receives the same read access.

## Build and validation

Build the MSI from 64-bit Windows with the .NET SDK, MSVC/Windows SDK, CMake,
and NuGet access:

If the source checkout is in WSL's Linux filesystem, copy or clone it to an
NTFS working directory before running the Windows build. Windows build tools can
lose access to generated intermediates through `\\wsl.localhost`; do not build
the MSI directly from that share.

~~~powershell
.\build.ps1 -Version 0.1.0
~~~

The build script creates an unsigned development MSI. Supply a signing
certificate thumbprint and RequireSignedPayloads for a release build. See the
[MSI build and validation guide](installer/README.md) for prerequisites, signing,
and clean-VM installation steps.

The MSI includes `ZultysNCallDiag.exe` for the installed bridge's health and
call-state checks. Initial installation registers a TAPI provider and schedules
a Windows restart so TAPI can activate it.

The simulator validates the service and local pipe protocol; it cannot validate
Windows TAPI marshalling, nCall behavior, ZAC coexistence, or the live MX
integration. Follow the [simulator exercise](docs/simulator-e2e.md) for the
current bridge-only run. The remaining checks must run on a Windows test machine
after the target MX upgrade is available.

## Remaining acceptance work

- Record MXTSP/nCall callback and identity traces in an isolated reference
  session, then compare the bridge's observable TAPI behavior.
- Build and install the MSI on a clean Windows VM; test provider enumeration,
  repair, rollback, upgrade, uninstall, and restart behavior.
- Configure nCall Generic TAPI against the new line and test caller screen
  pops, outbound calls, blind transfer, and assisted transfer.
- Test the documented WebSocket operations against a licensed, upgraded MX
  endpoint while ZAC controls the same device.

The detailed design, compatibility mapping, and rollout gates are in
[plan.md](plan.md). The bundled vendor documents are listed there.

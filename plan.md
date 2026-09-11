# ZAC to nCall TAPI provider plan

## Goal

Provide a Windows TAPI line that lets nSolve nCall receive caller screen pops,
place outbound calls, and transfer the operator's Zultys calls. ZAC remains the
operator's desktop and media client; the bridge uses the documented MX WebSocket
API to monitor and control the same bound device.

## Implementation status

The source now implements the Milestone 1 and Milestone 2 foundations:

- CSTA framing, XML command/event handling, the call-state reducer, and a
  deterministic MX simulator;
- the .NET Windows service, configuration utility, named-pipe server, and
  diagnostics client;
- the native x64 TSPI provider, persistent call-event subscription, and
  provider-registration helper; and
- WiX package source plus a Windows build script that stages the service,
  configuration utility, TSP, and registration helper.

The service/simulator protocol and concurrent local-pipe clients have been
exercised, and the native sources compile with the Windows SDK. The remaining
gates are Windows TAPI/nCall interoperability, clean-VM MSI lifecycle tests,
and a live upgraded-MX integration test. The milestones below therefore describe
the remaining validation and acceptance work as well as the source scope.

## Planning assumptions and constraints

- Plan for an upgrade to the latest supported release. As of September 2026,
  Zultys identifies MX Release 19 and ZAC 10.0.10 as its latest major release
  and desktop client combination. Confirm the exact supported point releases
  with the Zultys partner immediately before deployment.
- The upgraded MX must provide the documented WebSocket API, CSTA licensing, a
  valid TLS certificate, and Desktop Integration permission for each operator.
- The current MX 16.0.4 environment remains the reference environment until
  upgrade access is available. It cannot validate the target WebSocket adapter.
- ZAC does not provide TAPI; this project supplies the provider.
- ZAC remains responsible for signalling and audio.
- Development may be driven from WSL, but the provider and bridge must run
  as Windows components.
- The TAPI provider must be a native 64-bit DLL for 64-bit Windows and will be
  loaded by TAPISRV outside the interactive desktop session.
- The target application is the installed 32-bit nCall 5.4.4.477 executable at
  `C:\Program Files (x86)\nSolve\nCall\nCall.exe`. Its 32-bit TAPI client calls
  are marshalled to TAPISRV; they do not change the 64-bit provider requirement.
- The target architecture connects directly to the upgraded MX over TLS on TCP
  7779 using client type `3rdParty`. This connection is separate from ZAC's
  `MXIE` client session and is designed to coexist with it.
- Version 1 supports one configured Zultys operator identity and one exposed TAPI
  line per PC. Shared-PC identity switching can be added if deployment requires
  it.

## Confirmed findings

- Windows already has Zultys `MXTSP.tsp` version 16.0.4 installed in both
  `System32` and `SysWOW64`. Matching MXIE 16.0.4 files are also installed. The
  64-bit provider is registered and TAPI enumerates one address named
  `Zultys MX Line`.
- The target `nCall.exe` is file version 5.4.4.477 and PE32/Intel 80386. Its
  binary contains a dedicated `TdmPhoneSystemTAPI` module. TAPI symbols are not
  present as static imports, so runtime observation is still required to learn
  the functions and semantics it uses.
- The bundled nCall help describes a `Generic TAPI` phone-system mode. It selects
  an installed line under **View > Admin Options > Telephone System**, requires
  an nCall restart, and expects the provider to support inbound calls, outbound
  calls, transfers, and hold.
- The same help distinguishes assisted transfers from blind transfers. Contact
  actions and `dial:` commands use the assisted path; `blind:` commands request
  a blind transfer. Both paths belong in the target compatibility matrix.
- nCall Transfer Monitor is a separate SMDR-based billing/history component. It
  does not replace real-time TAPI call control for this project.
- MXTSP contains explicit dependencies on an MXIE peer: it searches for an MXIE
  port and provider-ready event and emits a request to start a required MXIE
  version when that peer is absent. ZAC does not implement this interface.
- ZAC's local command dispatcher accepts outbound `call=<number>` requests.
- The same dispatcher does not expose transfer commands or call-event
  subscriptions.
- The Office/Outlook bridge does not implement the conversation-management
  methods needed for transfer control.
- Windows UI Automation can inspect ZAC from a process in the same desktop
  session.
- During a live call, ZAC exposes a `SessionItemView` row containing the displayed
  caller name and number.
- The active-call panel exposes labelled `transfer`, `hold`, `mute`, and
  `new call` controls through its accessibility tree.
- The open transfer panel exposes a writable `SearchEdit` destination field and
  destination rows.
- Current observations do not expose a stable call ID, authoritative call state,
  or the original dialled DID.
- The supplied WebSocket guide documents stable call IDs, global call IDs,
  call-state events, allowed commands, make-call, single-step transfer, and
  two-step transfer.
- The MX homepage reports firmware 16.0.4. Although TCP 7779 accepts a WebSocket
  upgrade, the supplied API guide requires MX 17.0.10 or later. The endpoint
  handshake must not be treated as API compatibility.
- The supplied CAD guide documents `__CALLED_PARTY_ORIG__`, `__MX_CALL_ID__`,
  and `__FIRST_CALL_ID__`. These can be placed in a CAD template and displayed
  in ZAC's expanded session bar.

The existing PowerShell probes are read-only. They have not invoked a call action
or completed a transfer.

## Supplied-document review

### WebSocket API Guide, Revision 1.0, January 2025

This is the most consequential document. It defines a bidirectional TLS
WebSocket call-control API on port 7779 for MX 17.0.10 or later. It requires
CSTA licenses and a valid MX certificate. A user connection can monitor calls
and issue `MakeCall`, `ClearConnection`, `AnswerCall`, `HoldCall`,
`RetrieveCall`, `ParkCall`, `SingleStepTransferCall`, and two-step
`TransferCall` commands. Events include `Delivered`, `Established`,
`ConnectionCleared`, `Held`, and `Retrieved`.

Custom desktop applications should use client type `3rdParty`. ZAC uses client
type `MXIE`, so those connection types are designed to coexist for one user.
A `3rdParty` connection requires the Desktop Integration license and user-profile
permission. It controls the user's bound device and does not provide media.

The event examples contain call ID, device ID, global call IDs, direction, state,
allowed commands, calling/called/redirecting parties, and CAD. This is a stronger
source for a TAPI state model than accessibility inspection. The guide references
`simple-call-example.zip` and `call-control-example.zip`; those archives were not
included.

The current MX homepage reports 16.0.4, below the guide's minimum supported
version. This API is therefore a future upgrade path, not a current option. An
unauthenticated port-7779 upgrade succeeded, but that does not override the
documented firmware requirement.

### Call Attached Data, Revision 3, June 2020

This provides a supported way to surface the missing screen-pop data on
ZAC/MXIE-era systems. An MX administrator can define CAD fields using exact
system-variable names:

- `__CALLER_ID__`: calling party number.
- `__CALLED_PARTY__`: current called party, updated through transfers.
- `__CALLED_PARTY_ORIG__`: original called party, preserved through transfers.
- `__MX_CALL_ID__`: identifier for correlation with external data.
- `__FIRST_CALL_ID__`: global call identifier.

A template can display these fields immediately when ringing and show them in
ZAC's session bar. ZAC must be in expanded mode. This gives the UI fallback a
supported route to original DID and correlation, subject to an MX configuration
change and live validation.

### TCP and UDP Port Usage, Revision 15, March 2026

The port matrix distinguishes legacy desktop ZAC from web clients: ZAC uses the
bidirectional CSTA service on TCP 7778, while WebZAC and other WebSocket clients
use TCP 7779. This is consistent with ZAC 8.4 using the older CSTA transport. It
also identifies MXIE as end-of-life.

### MX Release 18 Feature Manual, April 2024

This does not define an integration API. It establishes that MX 16.x must first
upgrade to 17.0.10 before moving to release 18, and that moving from ZAC 8 to ZAC
9 requires a manual client installation.

### Application Integrations, June 2025

This is primarily a product overview. Flex and Flex Plus require MX 18.4 or later
and ZAC 9.4 or later, so they do not apply to this ZAC 8.4 deployment. Data
Connect can include originally dialled number, but it is post-call integration
rather than the required real-time bridge.

## Architecture decision

The selected target is the documented WebSocket adapter on the upgraded MX.
Connect as the operator with client type `3rdParty`, monitor documented events,
and issue documented call-control commands while ZAC remains connected as client
type `MXIE` and handles media.

MXTSP/MXIE is retained only as a TAPI behavioral oracle. MXIE is end-of-life and
shares ZAC's `MXIE` client type, so it is not the planned production sidecar.
Windows UI Automation and CAD-enhanced ZAC observation remain a contingency for
the current 16.0.4 system, not part of the upgraded target architecture.

## Phase 0: characterize the existing MX TAPI provider

Use the installed `MXTSP.tsp` as the behavioral reference for mapping Zultys calls
to TAPI. Mirror its externally observable semantics where nCall depends on them,
not its binary implementation, private protocol, defects, or undocumented quirks.

Create a read-only TAPI recorder that captures:

- Provider, line, and address capabilities.
- TAPI/API version negotiation and supported media modes.
- Call-state callback order for inbound, answered, held, transferred, and ended
  calls.
- Caller, called, connected, redirection, and redirecting identity fields.
- Which field carries the original client DID.
- Call IDs, related-call relationships, and consultation-call behavior.
- Asynchronous completion and error results for operations used by nCall.

Run the recorder against `Zultys MX Line` with MXIE if a compatible MXIE client is
available. Capture equivalent scenarios through the ZAC adapter and compare the
normalized results. Also run nCall against the reference provider, where safe, to
observe the exact behavior nCall accepts.

Run MXIE/MXTSP only in an isolated reference session where interrupting ZAC is
acceptable. Do not emulate MXIE's private IPC or ship MXTSP as part of the target
solution. The reference is used to learn the public TAPI contract that nCall
expects; the new provider will source its state and actions from WebSocket.

Exit criteria:

- A sanitized reference trace exists for every required workflow.
- Each relevant MXTSP capability, callback, and identity field has an explicit
  mapping to the proposed provider or a documented unsupported result.
- The new provider's observable behavior is compared with MXTSP for every nCall
  workflow, with intentional differences documented.

This comparison can be completed before upgraded-MX access and should inform the
provider simulator.

## Application design

### Installed components

| Component | Technology | Runs as | Responsibility |
| --- | --- | --- | --- |
| `ZultysNCallBridge.exe` | self-contained .NET 10 x64 Worker Service | Windows service | MX connection, authentication, CSTA protocol, call state, commands, configuration and diagnostics |
| `ZultysNCallTsp.tsp` | native C++20 x64 DLL | inside TAPISRV | TSPI implementation and translation to the local service protocol |
| `ZultysNCallConfig.exe` | .NET 10 x64 command-line utility | elevated administrator process | MX endpoint, operator identity, device ID, and DPAPI-protected credential entry |
| `ZultysNCallDiag.exe` | .NET 10 x64 command-line utility | administrator or support operator | Local bridge health, snapshot, call-event watch, and controlled command diagnostics |
| `ZultysNCallProviderReg.exe` | native C++ x64 console program | elevated MSI custom action | Calls `lineAddProvider` and `lineRemoveProvider` and records the permanent provider ID |
| `ZultysNCall.msi` | WiX Toolset 6 x64 MSI | Windows Installer | Per-machine install, upgrade, repair, registration and uninstall |

The service is published self-contained so target PCs do not need a separately
installed .NET runtime. The TSP has no CLR dependency and never loads the
WebSocket or XML stack into TAPISRV.

### Runtime architecture

```text
32-bit nCall 5.4.4.477
        |
TAPI32 marshalling -> 64-bit Windows TAPISRV
        |
ZultysNCallTsp.tsp (native TSPI provider)
        |
\\.\pipe\Zultys.NCall.Bridge.v1
        |
Zultys nCall Bridge (Windows service)
        |
TLS WebSocket wss://<mx-host>:7779, client type 3rdParty
        |
MX Release 19

ZAC 10 (client type MXIE) -> MX Release 19 -> bound device and media
```

One installation exposes one TAPI line for one configured Zultys operator. The
service starts at boot and does not interact with the desktop. ZAC may start,
stop, or restart independently; both clients control the same MX-bound device.

### Service identity, files and configuration

- Install the service for automatic start under `NT AUTHORITY\LocalService`,
  with Windows service recovery actions for unexpected exits.
- Install the application binaries and 64-bit TSP under
  `%ProgramFiles%\Zultys nCall Bridge\`.
- Store non-secret configuration under
  `%ProgramData%\Zultys nCall Bridge\config.json`.
- Encrypt the MX password with Windows DPAPI machine protection and restrict the
  encrypted secret and configuration ACLs to Administrators, SYSTEM, and the
  service identity.
- Do not accept the password as an MSI property or command-line argument. The
  configuration utility writes it after installation.
- Use the configured MX DNS name for certificate validation. Do not provide a
  permanent certificate-bypass option.

The current command-line configuration utility validates the endpoint form,
prompts for the password, saves it with machine-scope DPAPI, and applies the
configuration DACL. The service notices a completed first-run configuration
within two seconds. A guided connection-test UI remains later acceptance work.

## MX WebSocket/CSTA implementation

### Connection lifecycle

The service implements an explicit connection state machine:

```text
Unconfigured -> Disconnected -> Connecting -> Authenticating -> Monitoring -> Ready
                                      |              |              |
                                      +---------- Degraded <---------+
```

In `Ready`, the service has authenticated as the configured operator with client
type `3rdParty`, started monitoring the bound device, and published a complete
snapshot. It does not tell the provider that a line is in service before that
snapshot exists.

On transport loss, the service marks the line out of service, fails pending
commands once, reconnects with bounded exponential backoff and jitter, logs in,
starts a new monitor, rebuilds the snapshot, and then resumes events. It never
replays a command when the previous outcome is uncertain.

### Protocol engine

Implement the documented eight-byte CSTA envelope and XML payloads independently
of the WebSocket transport. The engine owns:

- Invoke-ID allocation, reserved event invoke ID handling, rollover and response
  correlation.
- Message-size bounds, fragmented WebSocket messages, partial CSTA frames,
  malformed XML and unknown message handling.
- Login, logout, monitor start/stop and keepalive behavior.
- `MakeCall`, `ClearConnection`, `HoldCall`, `RetrieveCall`,
  `SingleStepTransferCall` and two-step `TransferCall` requests.
- `Delivered`, `Established`, `Held`, `Retrieved` and `ConnectionCleared` events.
- Separate tracking of command acknowledgement and the later call-state events.

Create sanitized XML fixtures for every supported request, response, event and
documented error. Confirm the serializer and parser against the Zultys sample
archives when they become available.

### Normalized call model

For each call connection, retain:

- MX call ID, device ID, global call ID and global original call ID.
- Direction and current connection state.
- Calling, called, connected, redirecting and original-called identities.
- Allowed commands reported by MX.
- CAD values needed for screen-pop correlation.
- Parent/consultation relationship and monotonically increasing local revision.

The service publishes an immutable full snapshot on IPC connection and ordered
deltas afterward. `__CALLED_PARTY_ORIG__` is the preferred CAD fallback when the
event does not carry the original client DID. `__MX_CALL_ID__` and
`__FIRST_CALL_ID__` provide additional correlation. Caller identity must be
available before the TSP raises the incoming `OFFERING` state; original-called
identity is retained when a later event or CAD supplies it and is reported to
TAPI through `LINE_CALLINFO` for the effective CalledID.

## Local service protocol

Use byte-mode named pipes with explicit little-endian 32-bit framing and
versioned UTF-8 JSON messages. Cap each message at 64 KiB. The native provider uses a small,
statically-linked JSON parser; there is no extra runtime DLL inside TAPISRV.

The protocol includes:

- `Hello`/`HelloAck` with protocol version and capabilities.
- `GetSnapshot`/`Snapshot` for initial state and reconciliation.
- `Subscribe` and sequenced `CallChanged` events.
- `Command`/`CommandAccepted`/`CommandCompleted` using request IDs.
- `GetHealth` for configuration and support tooling.
- Explicit `Unavailable`, `Unsupported`, `InvalidState`, `Rejected`, `Timeout`
  and `OutcomeUnknown` results.

The pipe DACL admits SYSTEM, Administrators, and TAPISRV's service identity.
The provider keeps one shared subscription connection and never blocks a
TAPISRV callback thread on network I/O. The service gives an MX command ten
seconds to complete, and the provider uses the same ten-second deadline for
command IPC. Commands complete asynchronously and exactly once.

## TAPI provider surface

The provider initially advertises one owner-capable line and only voice media.
It implements the smallest TAPI 2.x/TSPI surface demonstrated by nCall and the
reference recorder. Expected mappings are:

| nCall/TAPI operation | Bridge/MX behavior |
| --- | --- |
| Incoming call | Populate available caller identity, then raise `LINECALLSTATE_OFFERING` from `Delivered`; retain original-called identity when later supplied |
| `lineMakeCall` | `MakeCall`; report asynchronous completion separately from dialing/ringback/connected events |
| `lineDrop` | `ClearConnection` |
| `lineHold` / `lineUnhold` | `HoldCall` / `RetrieveCall` |
| `lineBlindTransfer` | `SingleStepTransferCall` |
| `lineSetupTransfer` | Hold the original call and create a consultation call object |
| Consultation dial | `MakeCall` for the consultation destination |
| `lineCompleteTransfer` | Two-step `TransferCall` using the original and consultation connections |

`Established`, `Held`, `Retrieved` and `ConnectionCleared` drive connected,
on-hold, restored, disconnected and idle transitions. The exact dialing,
proceeding, ringback, call-info flags and callback ordering will follow the
MXTSP/nCall traces rather than assumptions in this table.

The current provider does not advertise Answer. ZAC owns media and the bridge
does not map an answer operation; a direct TSPI answer call returns the
unavailable result.

Required TSPI areas include provider initialization/shutdown, version
negotiation, line and address capabilities, line open/close, call status and call
information, make/drop, hold/retrieve, blind transfer, setup/complete transfer,
asynchronous request completion and provider install/remove. Unsupported
functions return the appropriate TAPI error and are not advertised.

All call objects use reference-counted internal handles. Shutdown stops the
provider workers before releasing pipe resources. The subscription callback
updates provider state directly; TSPI entry points remain short and do not wait
on network or pipe I/O.

## Windows service behavior

The service exposes no listening TCP port. It owns five internal modules:

1. `MxTransport`: TLS/WebSocket connection and bounded framing.
2. `CstaSession`: authentication, monitoring, invoke IDs and command correlation.
3. `CallStore`: deterministic state reducer and snapshot history.
4. `TapiGateway`: named-pipe clients, subscriptions and command dispatch.
5. `Health`: Event Log, redacted rolling logs, counters and diagnostic bundle.

Routine logs contain call-state names, local sequence numbers, error categories
and hashed correlation values. They exclude passwords, authentication payloads,
phone numbers, caller names and CAD. A diagnostic bundle requires an explicit
administrator action and redacts the same fields by default.

## MSI design

Build a signed, per-machine x64 MSI with WiX Toolset 6. The package has stable
component GUIDs, a stable UpgradeCode and major-upgrade handling. It supports
interactive, passive and silent installation.

Installation sequence:

1. Require administrative elevation and a supported 64-bit Windows release.
2. Stop the bridge service during upgrade and install versioned application files.
3. Install `ZultysNCallTsp.tsp` beside the x64 bridge service and register its
   absolute path with TAPI.
4. Install the service with automatic start and recovery actions.
5. Run a deferred elevated registration helper that calls `lineAddProvider`, then
   store the returned permanent provider ID in an MSI-owned registry value.
6. Schedule a Windows restart so TAPI activates the newly added provider.
7. Start the service. It reports `Unconfigured` until credentials are supplied.
8. In an interactive install, offer to launch the configuration utility after
   MSI completion. Silent deployment configures the service separately.

The registration custom action has a rollback action that calls
`lineRemoveProvider`. Uninstall stops the service, removes the provider using the
stored permanent ID, then removes binaries and encrypted credentials. Upgrade and
repair preserve configuration. The current helper treats a TAPI
registration/removal failure as an MSI failure and invokes rollback where
applicable; exact provider reinitialization or restart behavior remains a
clean-VM validation item.

Every executable, DLL and MSI is Authenticode-signed. Release validation includes
install, repair, major upgrade, rollback, uninstall and reinstall on a clean VM.

## Repository layout

```text
src/
  Bridge.Service/          .NET Windows service host
  Bridge.Core/             CSTA protocol, call model and state reducer
  Config/                  administrator configuration utility
  Diagnostics/             local pipe health, snapshot, and watch client
  Tsp.Provider/            native x64 TSPI provider
  ProviderRegistration/    lineAddProvider/lineRemoveProvider helper
  MxSimulator/             deterministic WebSocket/CSTA simulator
  Shared/                  configuration file ACL helper
installer/
  ZultysNCall.wixproj
  Package.wxs
build.ps1                  Windows build, stage, and package script
tests/
  Bridge.Core.Tests/
docs/
```

Use one Visual Studio solution with pinned SDK/tool versions and reproducible
Release builds. CI builds on Windows x64, runs unit/integration tests, produces
the MSI, verifies signatures and installs it in a disposable Windows VM.

## Delivery plan

### Milestone 1: reference contract and simulator

- Build the TAPI recorder and capture MXTSP behavior with installed nCall
  5.4.4.477 for incoming, outgoing, hold, assisted transfer, blind transfer and
  disconnect.
- The CSTA serializers, parsers, state reducer, IPC v1, and MX simulator are
  implemented. Continue adding sanitized fixtures as traces become available.
- Freeze the initial TAPI compatibility matrix from the reference traces.

Exit: sanitized traces and deterministic tests define both sides of the bridge.

### Milestone 2: service and provider against simulation

- The Windows service, command-line configuration utility, named-pipe server,
  native TSP, and registration helper are implemented.
- Validate provider enumeration with a TAPI diagnostic client.
- Configure nCall's `Generic TAPI` mode, restart nCall, and pass simulated screen
  pop, outbound, hold, assisted-transfer and blind-transfer workflows.

Exit: nCall completes every required workflow without a live MX.

### Milestone 3: live upgraded-MX integration

- Confirm MX Release 19 API compatibility, CSTA/Desktop Integration licensing,
  user permission, DNS and trusted certificate.
- Validate simultaneous `3rdParty` service and `MXIE` ZAC sessions.
- Validate original DID directly and through CAD fallback.
- Exercise every command and event, including rejection, timeout, ZAC restart,
  MX restart and network loss.

Exit: the service controls the bound device without disrupting ZAC or media and
recovers to an accurate snapshot after each failure test.

### Milestone 4: MSI and acceptance

- The WiX source and Windows staging/build script are implemented. Complete
  signing, clean-VM lifecycle validation, and any fixes found in that testing.
- Test on a clean Windows PC with the target nCall and ZAC versions.
- Run simultaneous-call, long-running stability and credential-rotation tests.
- Produce administrator installation/configuration instructions and a support
  runbook.

Exit: a signed MSI installs, configures, upgrades and removes the complete product,
and nCall passes the agreed live acceptance scenarios.

## Work that can start before MX upgrade access

1. Capture the installed MXTSP/nCall public TAPI behavior in an isolated Windows
   session.
2. Implement the protocol core and simulator from the supplied API guide.
3. Build the named-pipe contract, Windows service shell and call-state reducer.
4. Implement and register the TSP against the simulator.
5. Exercise nCall's `Generic TAPI` workflows against simulated calls.
6. Build and test the MSI lifecycle on clean Windows VMs.
7. Obtain the Zultys sample archives and an MX 19-specific compatibility statement
   while the upgrade is being arranged.

The current ZAC UI Automation probes remain legacy investigation tools and are
not dependencies of the product.

## Source documents

- [Zultys WebSocket API Guide, Revision 1.0](docs/Zultys_WebSocket_API_Guide_R1_0.pdf)
- [Call Attached Data, Revision 3](<docs/Call_Attached_Data July 2020.pdf>)
- [TCP and UDP Port Usage, Revision 15](<docs/TCP_and UDP_Port_Usage_by_Zultys_Equipment_and_Applications_Mar 2026.pdf>)
- [MX Release 18 Feature Manual](docs/R18_Feature_Manual_April_2024_2.pdf)
- [Zultys Application Integrations](docs/96-35211-Zultys-Application-Integrations_vJune2025.pdf)

## Implementation references

- [Microsoft: create a Windows Service using `BackgroundService`](https://learn.microsoft.com/en-us/dotnet/core/extensions/windows-service)
- [Microsoft: TAPI `lineAddProvider`](https://learn.microsoft.com/en-us/windows/win32/api/tapi/nf-tapi-lineaddprovider)
- [Microsoft: TAPI `lineRemoveProvider`](https://learn.microsoft.com/en-us/windows/win32/api/tapi/nf-tapi-lineremoveprovider)
- [WiX Toolset 6: create an MSI package](https://docs.firegiant.com/quick-start/)

## Current artifacts

- `src/Bridge.Core`: CSTA protocol, call model, reducer, and pipe envelopes.
- `src/Bridge.Service`: Windows service and local pipe server.
- `src/MxSimulator`: deterministic simulator for bridge/pipe testing.
- `src/Tsp.Provider`: native x64 TSPI provider.
- `src/ProviderRegistration`: provider registration helper.
- `installer/` and `build.ps1`: WiX source and Windows package build.
- `tools/Inspect-ZacDesktop.ps1`: legacy read-only accessibility inventory.
- `tools/Get-ZacCallSnapshot.ps1`: legacy read-only visible-call snapshot.
- `tools/README.md`: probe usage and limitations.

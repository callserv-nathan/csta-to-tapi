# Zultys to nCall TAPI bridge

This project will expose Zultys call state and controls to nSolve nCall through a
native Windows TAPI telephony service provider. Its required workflows are caller
screen pops, outbound calls, and transfers while ZAC remains the operator's
desktop and media client.

The delivery plan assumes an upgrade to the latest supported Zultys release. As
of September 2026, the planning target is MX Release 19 with ZAC 10.0.10, subject
to confirmation of the exact supported point releases before deployment. The
bridge will use the documented MX WebSocket API on TCP 7779 as client type
`3rdParty`, alongside ZAC's `MXIE` session.

The current MX 16.0.4 and ZAC 8.4.34 installation is a reference environment. It
cannot validate the target WebSocket integration. Its installed MXTSP/MXIE pair
will be used in an isolated test session to record the public TAPI behavior that
nCall expects. Existing ZAC UI Automation probes are legacy investigation tools,
not part of the planned production path.

The target application is 32-bit nCall 5.4.4.477 at
`C:\Program Files (x86)\nSolve\nCall\nCall.exe`. Binary inspection confirms a
dedicated TAPI phone-system module. Its exact TAPI calls and callback expectations
will be captured at runtime against MXTSP and the simulator.

The bundled nCall help confirms a selectable `Generic TAPI` mode and expects its
chosen line to support inbound calls, outbound calls, hold, assisted transfer,
and blind transfer. nCall must be restarted after changing the selected line.

No provider or bridge service has been implemented yet. The planned deliverable
is a signed per-machine x64 MSI containing a self-contained .NET 10 Windows
service, native C++ x64 TSP, administrator configuration utility, and TAPI
registration helper. Development can be driven from WSL, but compilation,
installation, TAPI interoperability, ZAC coexistence, and end-to-end testing must
run on Windows. See the [implementation plan](plan.md) for the binary boundaries,
runtime protocols, MSI behavior, milestones, and work that can start before
upgrade access is available.

## Proposed architecture

```text
nCall (TAPI client)
        |
Windows Telephony service (TAPISRV)
        |
Native C++ TSPI provider DLL (.tsp)
        |
Authenticated local named pipe
        |
Windows bridge service
        |
TLS WebSocket (client type 3rdParty, TCP 7779)
        |
Upgraded Zultys MX

ZAC (client type MXIE) -> upgraded MX -> bound device and media
```

The provider exposes an operator line and translates TSPI commands and callbacks.
The bridge service owns MX authentication, monitoring, call correlation,
reconnection, command execution, and diagnostics. The service and provider use a
versioned IPC contract with request IDs, asynchronous results, snapshots, and
ordered call-state events.

For 64-bit Windows, the provider must be a 64-bit DLL because Windows loads it
inside TAPISRV. The 32-bit nCall client communicates with TAPISRV through the
Windows TAPI marshalling layer. A standalone Windows service does not itself
implement a TAPI provider.

A Windows service cannot depend on ZAC's interactive desktop session. It controls
the user's bound device through MX; audio remains with ZAC or the existing Zultys
endpoint.

## Validation and delivery

Build a simulator first to exercise call identity, asynchronous completion,
multiple simultaneous calls, transfer legs, duplicate events, timeouts, and
connection loss. After reconnect, reconcile a backend snapshot if supported;
never replay uncertain dial or transfer requests automatically.

Test provider registration, line enumeration, call events, and controls on Windows
with a TAPI diagnostic client, then test nCall's line selection, client screen
pops, and call handling against a real Zultys test endpoint. A Linux build or
simulator cannot establish Windows TAPI or Zultys interoperability.

Deliver an installer that registers and removes the provider through the Windows
TAPI provider APIs and installs and removes the bridge service. Restrict IPC to
authorized local principals, protect stored credentials with Windows facilities,
and keep credentials and full call payloads out of routine logs.

The MSI installs the service for delayed automatic start, registers the TSP with
`lineAddProvider`, supports repair and major upgrades, and removes the provider
with `lineRemoveProvider` during uninstall. MX credentials are entered afterward
through the configuration utility and never passed through MSI properties.

## References

- [Zultys: ZAC 10.0 User Manual, April 2026](https://www.zultys.com/wp-content/uploads/2026/04/Zultys-Advanced-Communicator-10-User-Manual_April_2026.pdf)
- [Zultys WebSocket API Guide](docs/Zultys_WebSocket_API_Guide_R1_0.pdf)
- [Microsoft: Service Providers](https://learn.microsoft.com/en-us/windows/win32/tapi/service-providers)
- [Microsoft: About the Telephony Service Provider](https://learn.microsoft.com/en-us/windows/win32/tapi/about-the-telephony-service-provider-tsp-)
- [Microsoft: TSPI](https://learn.microsoft.com/en-us/windows/win32/tapi/telephony-service-provider-interface-tspi-)
- [nSolve: nCall integration and supported systems](https://www.nsolve.com/telephone-answering-service-software/)
- [nSolve: nCall help](https://www.nsolve.com/docs/nCall.pdf)
- [Zultys: MX Release 19.0 and ZAC 10.0.10 announcement](https://www.zultys.com/release/zultys-mx-release-19-0-zac-10-0-10-ai-powered-productivity-tools/)

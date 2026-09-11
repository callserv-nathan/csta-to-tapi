# Simulator end-to-end exercise

This exercise validates the bridge service, CSTA/WebSocket framing, call-state
reducer, and local named-pipe protocol without an MX server. It does not load
the TSP into TAPISRV or drive nCall. Run that final interoperability test from a
clean Windows VM after building the MSI.

Run the commands from the repository root in elevated PowerShell windows. The
pipe DACL admits Administrators, SYSTEM, and TapiSrv. The examples use separate
windows so the persistent watch client remains connected while another client
issues commands.

## 1. Start the simulator

~~~powershell
dotnet run --project .\src\MxSimulator\MxSimulator.csproj
~~~

The simulator listens at http://127.0.0.1:7780 and serves its WebSocket endpoint
at ws://127.0.0.1:7780/ws.

## 2. Create simulator configuration

In an elevated second window, create an isolated configuration directory and
file:

~~~powershell
$configRoot = Join-Path $env:LOCALAPPDATA 'ZultysNCallBridgeSimulator'
New-Item -ItemType Directory -Force -Path $configRoot
$config = Join-Path $configRoot 'config.json'
dotnet run --project .\src\Config\Config.csproj -- simulator --config $config
~~~

This configuration deliberately contains no production credential. The bridge
requires the simulator password through an environment variable instead.

## 3. Start the bridge in console mode

In a third window:

~~~powershell
$configRoot = Join-Path $env:LOCALAPPDATA 'ZultysNCallBridgeSimulator'
$config = Join-Path $configRoot 'config.json'
$env:ZULTYS_NCALL_TEST_PASSWORD = 'simulator'
dotnet run --project .\src\Bridge.Service\Bridge.Service.csproj -- --console --config $config
~~~

Wait for the log entry that reports the configured MX device is being monitored.
The bridge has then completed simulator login and MonitorStart.

## 4. Keep a pipe subscription open

In a fourth window:

~~~powershell
dotnet run --project .\src\Diagnostics\Diagnostics.csproj -- watch
~~~

It first prints a Subscribed envelope and remains connected. Leave it running.
This is the same persistent-subscription shape used by the native TSP.

## 5. Generate and control an inbound call

In another window, create an inbound call:

~~~powershell
$call = Invoke-RestMethod -Method Post -Uri http://127.0.0.1:7780/scenario/inbound -ContentType 'application/json' -Body '{"caller":"0412345678","callerName":"Test Caller","originalCalled":"1300555123","deviceId":"210"}'
$call.callId
~~~

The watch window should print a CallChanged envelope with an incoming Offering
call and its caller/called values. Confirm the bridge snapshot:

~~~powershell
dotnet run --project .\src\Diagnostics\Diagnostics.csproj -- snapshot
~~~

Hold, retrieve, establish, and clear the call while the watch client remains
connected:

~~~powershell
dotnet run --project .\src\Diagnostics\Diagnostics.csproj -- command HoldCall --call $call.callId --device 210
dotnet run --project .\src\Diagnostics\Diagnostics.csproj -- command RetrieveCall --call $call.callId --device 210
Invoke-RestMethod -Method Post -Uri ('http://127.0.0.1:7780/scenario/establish/' + $call.callId)
Invoke-RestMethod -Method Post -Uri ('http://127.0.0.1:7780/scenario/clear/' + $call.callId)
~~~

Each command should return a CommandCompleted envelope. The watch stream should
show Held, Connected, and then a removed CallChanged notification. The
Established update includes the simulator original-called value. This also checks
that one command connection does not interrupt a subscribed client.

## 6. Exercise an outgoing bridge command

Make an outbound simulated call:

~~~powershell
dotnet run --project .\src\Diagnostics\Diagnostics.csproj -- command MakeCall --destination 0412000000 --device 210
~~~

The response includes the simulator call ID and device ID. The watch window
receives an outgoing Offering update; establish or clear it using the same
scenario endpoints if needed.

## What this proves

The exercise proves that:

- the service can log in and monitor the simulator;
- inbound caller/called identity, followed by original-called identity after
  establishment, reaches a persistent local subscriber;
- the service accepts a concurrent command client;
- command results return separately from later call-state deltas; and
- call changes maintain their state through hold, retrieve, establish, and
  clear transitions.

It does not prove the Windows TAPI callback order, TAPISRV loading, MSI provider
registration, nCall screen-pop behavior, or live MX/ZAC coexistence. Those are
the next Windows acceptance tests.

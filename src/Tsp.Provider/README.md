# Zultys nCall TSPI provider

ZultysNCallTsp.tsp is the native x64 TAPI Service Provider loaded by the
64-bit TAPISRV process. It deliberately contains no MX/WebSocket client and no
CLR dependency. The bridge service owns MX authentication, CSTA state,
reconnection, and credential handling.

The provider targets TAPI/TSPI 2.2. A 32-bit nCall client can use it because
TAPI32 marshals calls to the 64-bit TAPISRV process that loads this provider.

## Implemented behavior

The provider exposes one owner-capable interactive-voice line named Zultys nCall
Bridge. Its command worker communicates with the local bridge through a
length-prefixed UTF-8 JSON pipe contract. Commands receive asynchronous TSPI
completion; the TAPI callback thread never waits on MX or pipe I/O. The bridge
and TSP both use a ten-second command deadline.

A separate persistent pipe subscription consumes CallChanged events. It:

- creates a TAPI call through LINE_NEWCALL for an active bridge call;
- sends the initial offering or dialing state only after TAPI has assigned the
  call handle;
- fills caller name and caller number before the incoming offering callback,
  then retains original-called identity when a later MX event or CAD supplies
  it and raises `LINE_CALLINFO` for the updated CalledID; and
- maps later connected, held, retrieved, and cleared events into TAPI call
  state changes.

The current command mappings are:

| TAPI operation | Bridge command |
| --- | --- |
| lineMakeCall | MakeCall |
| lineDrop | ClearConnection |
| lineHold | HoldCall |
| lineUnhold | RetrieveCall |
| lineBlindTransfer | SingleStepTransferCall |
| lineSetupTransfer | HoldCall, then a local consultation call |
| consultation lineDial | MakeCall |
| lineCompleteTransfer | TransferCall |

Answer is intentionally not advertised. ZAC owns media, and this bridge does
not map MX AnswerCall. The provider returns the standard unavailable result if
a client calls it directly.

## Build

Build on 64-bit Windows with MSVC, a Windows SDK that includes tapi.h and
tspi.h, and CMake 3.25 or newer. Use an installed Visual Studio generator:

~~~powershell
cmake -S src\Tsp.Provider -B out\Tsp.Provider -G "Visual Studio 17 2022" -A x64
cmake --build out\Tsp.Provider --config Release
~~~

On systems that provide a newer generator, replace the generator name with the
one reported by cmake -E capabilities. The output is
out\Tsp.Provider\Release\ZultysNCallTsp.tsp.

The root [build script](../../build.ps1) detects an installed Visual Studio
generator, builds this target as x64, stages it, and passes it to WiX.

## Registration and test use

Do not copy or register the provider manually on a working desktop. The MSI
installs it beside the bridge service, then invokes the x64 registration helper,
which passes that absolute path to lineAddProvider and records the permanent
provider ID for removal.

The bridge pipe ACL must grant TAPISRV's service identity read/write access.
TAPISRV and the bridge service are different identities; the interactive user
is not a substitute for either one.

The source has compiled with the Windows SDK, but it has not yet completed a
clean-VM TAPISRV/nCall interoperability run. Validate it with the MSI on a
disposable Windows VM before using it with a production nCall or MX system.

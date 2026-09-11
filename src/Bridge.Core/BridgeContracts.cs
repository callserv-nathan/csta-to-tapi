using Zultys.NCall.Bridge.Calls;

namespace Zultys.NCall.Bridge.Contracts;

public enum BridgeConnectionState
{
    Unconfigured,
    Disconnected,
    Connecting,
    Authenticating,
    Monitoring,
    Ready,
    Degraded,
}

public sealed record BridgeHealth(
    BridgeConnectionState State,
    string? Detail,
    DateTimeOffset UpdatedAt,
    long SnapshotRevision);

public sealed record BridgeCommandRequest(
    string Operation,
    string? CallId = null,
    string? DeviceId = null,
    string? Destination = null,
    string? ActiveCallId = null,
    string? ActiveDeviceId = null);

public sealed record BridgeCommandResult(
    bool Succeeded,
    string Code,
    string? Detail = null,
    string? CallId = null,
    string? DeviceId = null);

/// <summary>
/// A deliberately flat, versioned representation of a call sent to the native
/// TSP. It avoids making TAPISRV parse the bridge's richer internal snapshot.
/// </summary>
public sealed record BridgeCallNotification(
    string CallId,
    string DeviceId,
    string State,
    string Direction,
    string? CallingNumber,
    string? CallingName,
    string? CalledNumber,
    string? OriginalCalledNumber,
    long Revision,
    bool Removed = false);

public sealed record BridgeSnapshot(BridgeHealth Health, CallSnapshot Calls);

namespace Zultys.NCall.Bridge.Calls;

public enum BridgeCallState
{
    Offering,
    Connected,
    Held,
    Disconnected,
}

public enum CallDirection
{
    Unknown,
    Incoming,
    Outgoing,
}

public sealed record CallKey(string CallId, string DeviceId)
{
    public override string ToString() => $"{CallId}@{DeviceId}";
}

public sealed record PartyIdentity(string? Number, string? DisplayName)
{
    public static PartyIdentity Empty { get; } = new(null, null);
}

public sealed record BridgeCall(
    CallKey Key,
    BridgeCallState State,
    CallDirection Direction,
    PartyIdentity Calling,
    PartyIdentity Called,
    PartyIdentity Connected,
    PartyIdentity Redirecting,
    string? OriginalCalledNumber,
    string? GlobalCallId,
    string? GlobalOriginalCallId,
    IReadOnlySet<string> AllowedCommands,
    IReadOnlyDictionary<string, string> Cad,
    long Revision,
    CallKey? ParentCall);

public sealed record CallSnapshot(long Revision, IReadOnlyList<BridgeCall> Calls)
{
    public static CallSnapshot Empty { get; } = new(0, Array.Empty<BridgeCall>());
}

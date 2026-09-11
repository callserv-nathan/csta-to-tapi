using System.Collections.Immutable;
using System.Xml.Linq;
using Zultys.NCall.Bridge.Csta;

namespace Zultys.NCall.Bridge.Calls;

/// <summary>
/// Converts documented CSTA call events into an immutable, ordered bridge snapshot.
/// </summary>
public sealed class CallStateReducer
{
    private readonly Dictionary<CallKey, BridgeCall> _calls = new();
    private long _revision;

    public CallSnapshot Snapshot() => new(
        _revision,
        _calls.Values.OrderBy(call => call.Key.ToString(), StringComparer.Ordinal).ToArray());

    public CallSnapshot Apply(MxEvent cstaEvent)
    {
        ArgumentNullException.ThrowIfNull(cstaEvent);

        switch (cstaEvent.Name)
        {
            case "DeliveredEvent":
                ApplyDelivered(cstaEvent.Root);
                break;
            case "EstablishedEvent":
                ApplyEstablished(cstaEvent.Root);
                break;
            case "HeldEvent":
                ApplyState(cstaEvent.Root, "heldConnection", BridgeCallState.Held);
                break;
            case "RetrievedEvent":
                ApplyState(cstaEvent.Root, "retrievedConnection", BridgeCallState.Connected);
                break;
            case "ConnectionClearedEvent":
                ApplyCleared(cstaEvent.Root);
                break;
        }

        return Snapshot();
    }

    private void ApplyDelivered(XElement root)
    {
        var key = ReadConnection(root, "connection");
        var calling = ReadParty(root.ElementByLocalName("callingDevice"), root.ValueByLocalName("callingDisplayName"));
        var called = ReadParty(root.ElementByLocalName("calledDevice"), root.ValueByLocalName("alertingDisplayName"));
        var current = GetOrCreate(key);
        var direction = Enum.TryParse<CallDirection>(root.ValueByLocalName("direction"), true, out var parsedDirection)
            ? parsedDirection
            : current.Direction;

        _calls[key] = current with
        {
            State = BridgeCallState.Offering,
            Direction = direction,
            Calling = calling,
            Called = called,
            Revision = NextRevision(),
        };
    }

    private void ApplyEstablished(XElement root)
    {
        var key = ReadConnection(root, "establishedConnection");
        var current = GetOrCreate(key);
        var cad = root.Elements().Where(element => element.Name.LocalName == "cad")
            .Where(element => !string.IsNullOrWhiteSpace(element.Attribute("name")?.Value))
            .ToImmutableDictionary(
                element => element.Attribute("name")!.Value,
                element => element.Value.Trim(),
                StringComparer.Ordinal);
        var commands = root.ElementByLocalName("cmdsAllowed")?.Elements()
            .Where(element => element.Name.LocalName == "cmd")
            .Select(element => element.Value.Trim())
            .Where(value => !string.IsNullOrWhiteSpace(value))
            .ToImmutableHashSet(StringComparer.Ordinal)
            ?? ImmutableHashSet<string>.Empty;

        var direction = Enum.TryParse<CallDirection>(root.ValueByLocalName("direction"), true, out var parsedDirection)
            ? parsedDirection
            : current.Direction;
        var originalCalled = cad.GetValueOrDefault("__CALLED_PARTY_ORIG__") ?? current.OriginalCalledNumber;

        _calls[key] = current with
        {
            State = BridgeCallState.Connected,
            Direction = direction,
            Calling = ReadParty(root.ElementByLocalName("callingDevice"), root.ValueByLocalName("callingDisplayName")),
            Called = ReadParty(root.ElementByLocalName("calledDevice"), null),
            Connected = ReadParty(root.ElementByLocalName("answeringDevice"), root.ValueByLocalName("answeringDisplayName")),
            Redirecting = ReadParty(root.ElementByLocalName("lastRedirectionDevice"), null),
            OriginalCalledNumber = originalCalled,
            GlobalCallId = root.ElementByLocalName("establishedConnection")?.ValueByLocalName("globalCallID") ?? current.GlobalCallId,
            GlobalOriginalCallId = root.ElementByLocalName("establishedConnection")?.ValueByLocalName("globalOrigCallID") ?? current.GlobalOriginalCallId,
            AllowedCommands = commands,
            Cad = cad,
            Revision = NextRevision(),
        };
    }

    private void ApplyState(XElement root, string connectionElement, BridgeCallState state)
    {
        var key = ReadConnection(root, connectionElement);
        var current = GetOrCreate(key);
        _calls[key] = current with { State = state, Revision = NextRevision() };
    }

    private void ApplyCleared(XElement root)
    {
        var key = ReadConnection(root, "droppedConnection");
        if (_calls.Remove(key))
        {
            NextRevision();
        }
    }

    private BridgeCall GetOrCreate(CallKey key) => _calls.TryGetValue(key, out var existing)
        ? existing
        : new BridgeCall(
            key,
            BridgeCallState.Offering,
            CallDirection.Unknown,
            PartyIdentity.Empty,
            PartyIdentity.Empty,
            PartyIdentity.Empty,
            PartyIdentity.Empty,
            null,
            null,
            null,
            ImmutableHashSet<string>.Empty,
            ImmutableDictionary<string, string>.Empty,
            _revision,
            null);

    private long NextRevision() => ++_revision;

    private static CallKey ReadConnection(XElement root, string connectionName)
    {
        var connection = root.ElementByLocalName(connectionName)
            ?? throw new CstaProtocolException($"The CSTA event is missing {connectionName}.");
        var callId = connection.ValueByLocalName("callID");
        var deviceId = connection.ValueByLocalName("deviceID");
        if (string.IsNullOrWhiteSpace(callId) || string.IsNullOrWhiteSpace(deviceId))
        {
            throw new CstaProtocolException($"The CSTA {connectionName} has no call ID or device ID.");
        }

        return new CallKey(callId, deviceId);
    }

    private static PartyIdentity ReadParty(XElement? element, string? displayName)
    {
        if (element is null)
        {
            return PartyIdentity.Empty;
        }

        var party = element.ElementByLocalName("party");
        var number = party?.ValueByLocalName("addr") ?? element.ValueByLocalName("deviceIdentifier");
        var display = displayName ?? party?.ValueByLocalName("disp");
        return new PartyIdentity(number, display);
    }
}

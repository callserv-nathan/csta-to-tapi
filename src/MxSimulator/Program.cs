using Microsoft.AspNetCore.Http.HttpResults;
using System.Collections.Concurrent;
using System.Net.WebSockets;
using System.Text;
using System.Xml.Linq;
using Zultys.NCall.Bridge.Calls;
using Zultys.NCall.Bridge.Csta;
using Zultys.NCall.MxSimulator;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.UseUrls(Environment.GetEnvironmentVariable("ZULTYS_SIMULATOR_URL") ?? "http://127.0.0.1:7780");
builder.Services.AddSingleton<SimulatorHub>();

var app = builder.Build();
app.UseWebSockets();

app.MapGet("/health", (SimulatorHub hub) => Results.Ok(hub.Status));

app.Map("/ws", async (HttpContext context, SimulatorHub hub, CancellationToken cancellationToken) =>
{
    if (!context.WebSockets.IsWebSocketRequest)
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return;
    }

    using var socket = await context.WebSockets.AcceptWebSocketAsync().ConfigureAwait(false);
    await hub.AcceptAsync(socket, cancellationToken).ConfigureAwait(false);
});

app.MapPost("/scenario/inbound", async Task<Ok<SimulatorCall>> (InboundScenario scenario, SimulatorHub hub, CancellationToken cancellationToken) =>
{
    var call = await hub.CreateInboundAsync(scenario, cancellationToken).ConfigureAwait(false);
    return TypedResults.Ok(call);
});

app.MapPost("/scenario/establish/{callId}", async Task<Results<Ok<SimulatorCall>, NotFound>> (string callId, SimulatorHub hub, CancellationToken cancellationToken) =>
{
    var call = await hub.EstablishAsync(callId, cancellationToken).ConfigureAwait(false);
    return call is null ? TypedResults.NotFound() : TypedResults.Ok(call);
});

app.MapPost("/scenario/clear/{callId}", async Task<Results<Ok, NotFound>> (string callId, SimulatorHub hub, CancellationToken cancellationToken) =>
{
    return await hub.ClearAsync(callId, cancellationToken).ConfigureAwait(false)
        ? TypedResults.Ok()
        : TypedResults.NotFound();
});

app.Run();

namespace Zultys.NCall.MxSimulator
{

public sealed record InboundScenario(
    string Caller = "0412345678",
    string CallerName = "Simulator Caller",
    string OriginalCalled = "1300000000",
    string DeviceId = "210");

public sealed record SimulatorCall(
    string CallId,
    string DeviceId,
    string Caller,
    string CallerName,
    string Called,
    string State,
    string Direction,
    string GlobalCallId);

public sealed record SimulatorStatus(int ConnectedClients, int MonitoringClients, int ActiveCalls);

public sealed class SimulatorHub
{
    private readonly ConcurrentDictionary<Guid, SimulatorSession> _sessions = new();
    private readonly ConcurrentDictionary<string, SimulatorCall> _calls = new();
    private long _nextCallId = 100;

    public SimulatorStatus Status => new(
        _sessions.Count,
        _sessions.Values.Count(session => session.IsMonitoring),
        _calls.Count);

    public async Task AcceptAsync(WebSocket socket, CancellationToken cancellationToken)
    {
        var session = new SimulatorSession(Guid.NewGuid(), socket, this);
        if (!_sessions.TryAdd(session.Id, session))
        {
            throw new InvalidOperationException("Could not track simulator WebSocket client.");
        }

        try
        {
            await session.RunAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _sessions.TryRemove(session.Id, out _);
            session.Dispose();
        }
    }

    public async Task<SimulatorCall> CreateInboundAsync(InboundScenario scenario, CancellationToken cancellationToken)
    {
        var call = NewCall(
            scenario.DeviceId,
            scenario.Caller,
            scenario.CallerName,
            scenario.OriginalCalled,
            "Offering",
            "Incoming");
        await BroadcastEventAsync(DeliveredXml(call), cancellationToken).ConfigureAwait(false);
        return call;
    }

    public async Task<SimulatorCall?> EstablishAsync(string callId, CancellationToken cancellationToken)
    {
        if (!_calls.TryGetValue(callId, out var call))
        {
            return null;
        }

        call = call with { State = "Connected" };
        _calls[callId] = call;
        await BroadcastEventAsync(EstablishedXml(call), cancellationToken).ConfigureAwait(false);
        return call;
    }

    public async Task<bool> ClearAsync(string callId, CancellationToken cancellationToken)
    {
        if (!_calls.TryRemove(callId, out var call))
        {
            return false;
        }

        await BroadcastEventAsync(ClearedXml(call), cancellationToken).ConfigureAwait(false);
        return true;
    }

    internal async Task HandlePacketAsync(SimulatorSession session, CstaPacket request, CancellationToken cancellationToken)
    {
        var root = XDocument.Parse(request.Xml).Root
            ?? throw new CstaProtocolException("Simulator received XML with no root element.");
        var name = root.Name.LocalName;
        switch (name)
        {
            case "loginRequest":
                session.IsAuthenticated = true;
                await session.SendAsync(request.InvokeId, "<loginResponce Code=\"0\" sn=\"SIM\" ext=\"210\" userId=\"sim-user\" wwwUuid=\"sim-token\">Login OK</loginResponce>", cancellationToken).ConfigureAwait(false);
                break;
            case "MonitorStart":
                EnsureAuthenticated(session);
                session.IsMonitoring = true;
                await session.SendAsync(request.InvokeId, "<MonitorStartResponse><monitorCrossRefID>1</monitorCrossRefID><actualMonitorMediaClass><voice>true</voice></actualMonitorMediaClass></MonitorStartResponse>", cancellationToken).ConfigureAwait(false);
                break;
            case "MonitorStop":
                session.IsMonitoring = false;
                await session.SendAsync(request.InvokeId, "<MonitorStopResponse></MonitorStopResponse>", cancellationToken).ConfigureAwait(false);
                break;
            case "logout":
                session.IsMonitoring = false;
                await session.SendAsync(request.InvokeId, "<logoutResponse></logoutResponse>", cancellationToken).ConfigureAwait(false);
                break;
            case "MakeCall":
                await HandleMakeCallAsync(session, request.InvokeId, root, cancellationToken).ConfigureAwait(false);
                break;
            case "HoldCall":
                await HandleConnectionCommandAsync(session, request.InvokeId, root, "callToBeHeld", "HeldEvent", cancellationToken).ConfigureAwait(false);
                break;
            case "RetrieveCall":
                await HandleConnectionCommandAsync(session, request.InvokeId, root, "callToBeRetrieved", "RetrievedEvent", cancellationToken).ConfigureAwait(false);
                break;
            case "ClearConnection":
                await HandleClearAsync(session, request.InvokeId, root, cancellationToken).ConfigureAwait(false);
                break;
            case "SingleStepTransferCall":
                await HandleTransferAsync(session, request.InvokeId, root, cancellationToken).ConfigureAwait(false);
                break;
            case "TransferCall":
                await HandleTwoStepTransferAsync(session, request.InvokeId, root, cancellationToken).ConfigureAwait(false);
                break;
            default:
                await session.SendAsync(request.InvokeId, $"<errorResponse Error=\"Unsupported command: {Escape(name)}\" />", cancellationToken).ConfigureAwait(false);
                break;
        }
    }

    private async Task HandleMakeCallAsync(SimulatorSession session, int invokeId, XElement root, CancellationToken cancellationToken)
    {
        EnsureAuthenticated(session);
        var deviceId = Required(root.ValueByLocalName("callingDevice"), "callingDevice");
        var destination = Required(root.ValueByLocalName("calledDirectoryNumber"), "calledDirectoryNumber");
        var call = NewCall(deviceId, deviceId, "Simulator Operator", destination, "Offering", "Outgoing");
        await session.SendAsync(invokeId, $"<MakeCallResponse><callingDevice><callID>{Escape(call.CallId)}</callID><deviceID>{Escape(deviceId)}</deviceID></callingDevice></MakeCallResponse>", cancellationToken).ConfigureAwait(false);
        await BroadcastEventAsync(DeliveredXml(call), cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleConnectionCommandAsync(SimulatorSession session, int invokeId, XElement root, string connectionName, string eventName, CancellationToken cancellationToken)
    {
        var connection = root.ElementByLocalName(connectionName)
            ?? throw new CstaProtocolException($"Missing {connectionName}.");
        var callId = Required(connection.ValueByLocalName("callID"), "callID");
        var deviceId = Required(connection.ValueByLocalName("deviceID"), "deviceID");
        if (!_calls.TryGetValue(callId, out var call) || call.DeviceId != deviceId)
        {
            await session.SendAsync(invokeId, "<errorResponse Error=\"Unknown call\" />", cancellationToken).ConfigureAwait(false);
            return;
        }

        call = call with { State = eventName == "HeldEvent" ? "Held" : "Connected" };
        _calls[callId] = call;
        await session.SendAsync(invokeId, $"<{eventName[..^5]}Response></{eventName[..^5]}Response>", cancellationToken).ConfigureAwait(false);
        var connectionElement = eventName == "HeldEvent" ? "heldConnection" : "retrievedConnection";
        var deviceElement = eventName == "HeldEvent" ? "holdingDevice" : "retrievingDevice";
        await BroadcastEventAsync($"<{eventName}><monitorCrossRefID>1</monitorCrossRefID><{connectionElement}><callID>{Escape(call.CallId)}</callID><deviceID>{Escape(call.DeviceId)}</deviceID></{connectionElement}><{deviceElement}><deviceIdentifier>{Escape(call.DeviceId)}</deviceIdentifier></{deviceElement}><cause>normal</cause></{eventName}>", cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleClearAsync(SimulatorSession session, int invokeId, XElement root, CancellationToken cancellationToken)
    {
        var connection = root.ElementByLocalName("connectionToBeCleared")
            ?? throw new CstaProtocolException("Missing connectionToBeCleared.");
        var callId = Required(connection.ValueByLocalName("callID"), "callID");
        await session.SendAsync(invokeId, "<ClearConnectionResponse></ClearConnectionResponse>", cancellationToken).ConfigureAwait(false);
        await ClearAsync(callId, cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleTransferAsync(SimulatorSession session, int invokeId, XElement root, CancellationToken cancellationToken)
    {
        var active = root.ElementByLocalName("activeCall") ?? throw new CstaProtocolException("Missing activeCall.");
        var callId = Required(active.ValueByLocalName("callID"), "callID");
        var destination = Required(root.ValueByLocalName("transferredTo"), "transferredTo");
        await session.SendAsync(invokeId, $"<SingleStepTransferCallResponse><transferredCall><callID>{Escape(callId)}</callID><deviceID>{Escape(destination)}</deviceID></transferredCall></SingleStepTransferCallResponse>", cancellationToken).ConfigureAwait(false);
        await ClearAsync(callId, cancellationToken).ConfigureAwait(false);
    }

    private async Task HandleTwoStepTransferAsync(SimulatorSession session, int invokeId, XElement root, CancellationToken cancellationToken)
    {
        var held = root.ElementByLocalName("heldCall") ?? throw new CstaProtocolException("Missing heldCall.");
        var callId = Required(held.ValueByLocalName("callID"), "callID");
        await session.SendAsync(invokeId, "<TransferCall></TransferCall>", cancellationToken).ConfigureAwait(false);
        await ClearAsync(callId, cancellationToken).ConfigureAwait(false);
    }

    private SimulatorCall NewCall(string deviceId, string caller, string callerName, string called, string state, string direction)
    {
        var callId = Interlocked.Increment(ref _nextCallId).ToString(System.Globalization.CultureInfo.InvariantCulture);
        var call = new SimulatorCall(callId, deviceId, caller, callerName, called, state, direction, $"SIM-00-{callId}");
        if (!_calls.TryAdd(callId, call))
        {
            throw new InvalidOperationException("Simulator call ID collision.");
        }

        return call;
    }

    private async Task BroadcastEventAsync(string xml, CancellationToken cancellationToken)
    {
        var monitoring = _sessions.Values.Where(session => session.IsMonitoring).ToArray();
        await Task.WhenAll(monitoring.Select(session => session.SendAsync(CstaPacket.EventInvokeId, xml, cancellationToken))).ConfigureAwait(false);
    }

    private static string DeliveredXml(SimulatorCall call) =>
        $"<DeliveredEvent><monitorCrossRefID>1</monitorCrossRefID><connection><callID>{Escape(call.CallId)}</callID><deviceID>{Escape(call.DeviceId)}</deviceID></connection><callingDevice><deviceIdentifier>{Escape(call.Caller)}</deviceIdentifier></callingDevice><calledDevice><deviceIdentifier>{Escape(call.Called)}</deviceIdentifier></calledDevice><alertingDevice><deviceIdentifier>{Escape(call.DeviceId)}</deviceIdentifier></alertingDevice><callingDisplayName>{Escape(call.CallerName)}</callingDisplayName><localConnectionInfo>alerting</localConnectionInfo><cause>normal</cause><direction>{Escape(call.Direction)}</direction></DeliveredEvent>";

    private static string EstablishedXml(SimulatorCall call) =>
        $"<EstablishedEvent><monitorCrossRefID>1</monitorCrossRefID><establishedConnection><callID>{Escape(call.CallId)}</callID><deviceID>{Escape(call.DeviceId)}</deviceID><globalCallID>{Escape(call.GlobalCallId)}</globalCallID><globalOrigCallID>{Escape(call.GlobalCallId)}</globalOrigCallID></establishedConnection><callingDevice><deviceIdentifier>{Escape(call.Caller)}</deviceIdentifier><party><addr>{Escape(call.Caller)}</addr><disp>{Escape(call.CallerName)}</disp></party></callingDevice><calledDevice><deviceIdentifier>{Escape(call.Called)}</deviceIdentifier></calledDevice><answeringDevice><deviceIdentifier>{Escape(call.DeviceId)}</deviceIdentifier></answeringDevice><answeringDisplayName>Simulator Operator</answeringDisplayName><callingDisplayName>{Escape(call.CallerName)}</callingDisplayName><cause>normal</cause><state>Connected</state><direction>{Escape(call.Direction)}</direction><cmdsAllowed><cmd>Disconnect</cmd><cmd>Transfer</cmd><cmd>Park</cmd></cmdsAllowed><cad name=\"__CALLED_PARTY_ORIG__\" type=\"string\">{Escape(call.Called)}</cad><cad name=\"__FIRST_CALL_ID__\" type=\"string\">{Escape(call.GlobalCallId)}</cad></EstablishedEvent>";

    private static string ClearedXml(SimulatorCall call) =>
        $"<ConnectionClearedEvent><monitorCrossRefID>1</monitorCrossRefID><droppedConnection><callID>{Escape(call.CallId)}</callID><deviceID>{Escape(call.DeviceId)}</deviceID></droppedConnection><releasingDevice><deviceIdentifier>{Escape(call.DeviceId)}</deviceIdentifier></releasingDevice><cause>normal</cause></ConnectionClearedEvent>";

    private static void EnsureAuthenticated(SimulatorSession session)
    {
        if (!session.IsAuthenticated)
        {
            throw new CstaProtocolException("The simulator requires Login before this command.");
        }
    }

    private static string Required(string? value, string name) =>
        !string.IsNullOrWhiteSpace(value) ? value : throw new CstaProtocolException($"Missing {name}.");

    private static string Escape(string value) => System.Security.SecurityElement.Escape(value) ?? string.Empty;
}

public sealed class SimulatorSession : IDisposable
{
    private readonly WebSocket _socket;
    private readonly SimulatorHub _hub;
    private readonly SemaphoreSlim _sendLock = new(1, 1);

    internal SimulatorSession(Guid id, WebSocket socket, SimulatorHub hub)
    {
        Id = id;
        _socket = socket;
        _hub = hub;
    }

    public Guid Id { get; }

    public bool IsAuthenticated { get; set; }

    public bool IsMonitoring { get; set; }

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        while (_socket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
        {
            var text = await ReceiveTextAsync(cancellationToken).ConfigureAwait(false);
            if (text is null)
            {
                return;
            }

            var packet = ZultysPacketCodec.Decode(text);
            if (packet.InvokeId == CstaPacket.EventInvokeId)
            {
                throw new CstaProtocolException("Clients cannot send CSTA events.");
            }

            await _hub.HandlePacketAsync(this, packet, cancellationToken).ConfigureAwait(false);
        }
    }

    public async Task SendAsync(int invokeId, string xml, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(ZultysPacketCodec.Encode(new CstaPacket(invokeId, xml)));
        await _sendLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await _socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _sendLock.Release();
        }
    }

    private async Task<string?> ReceiveTextAsync(CancellationToken cancellationToken)
    {
        using var message = new MemoryStream();
        var buffer = new byte[8192];
        WebSocketReceiveResult result;
        do
        {
            result = await _socket.ReceiveAsync(new ArraySegment<byte>(buffer), cancellationToken).ConfigureAwait(false);
            if (result.MessageType == WebSocketMessageType.Close)
            {
                return null;
            }

            if (result.MessageType != WebSocketMessageType.Text)
            {
                throw new CstaProtocolException("Simulator supports text WebSocket messages only.");
            }

            message.Write(buffer, 0, result.Count);
        }
        while (!result.EndOfMessage);

        return Encoding.UTF8.GetString(message.GetBuffer(), 0, checked((int)message.Length));
    }

    public void Dispose() => _sendLock.Dispose();
}

}

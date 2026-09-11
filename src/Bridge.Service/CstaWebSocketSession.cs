using System.Collections.Concurrent;
using System.Net.WebSockets;
using System.Text;
using System.Xml.Linq;
using Zultys.NCall.Bridge.Csta;

namespace Zultys.NCall.Bridge.Service;

internal sealed class CstaWebSocketSession : IAsyncDisposable
{
    private readonly ClientWebSocket _socket = new();
    private readonly ConcurrentDictionary<int, TaskCompletionSource<CstaPacket>> _pending = new();
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private readonly CancellationTokenSource _stopped = new();
    private readonly ILogger<CstaWebSocketSession> _logger;
    private int _nextInvokeId;
    private Task? _receiveTask;

    public CstaWebSocketSession(ILogger<CstaWebSocketSession> logger)
    {
        _logger = logger;
    }

    public event Func<MxEvent, Task>? EventReceived;

    public Task Completion => _receiveTask ?? Task.CompletedTask;

    public string? MonitorCrossRefId { get; private set; }

    public async Task StartAsync(BridgeConfiguration configuration, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(configuration);
        if (!Uri.TryCreate(configuration.Endpoint, UriKind.Absolute, out var endpoint))
        {
            throw new InvalidOperationException("The MX endpoint is not a valid absolute URI.");
        }

        if (endpoint.Scheme == "ws" && !configuration.SimulatorMode)
        {
            throw new InvalidOperationException("An unencrypted ws endpoint is allowed only in SimulatorMode.");
        }

        if (endpoint.Scheme is not ("wss" or "ws"))
        {
            throw new InvalidOperationException("The MX endpoint must use wss, or ws in SimulatorMode.");
        }

        await _socket.ConnectAsync(endpoint, cancellationToken).ConfigureAwait(false);
        _receiveTask = ReceiveLoopAsync(_stopped.Token);

        var login = await SendRequestAsync(
            CstaXml.LoginRequest(configuration.Username, BridgeConfigurationStore.GetPassword(configuration)),
            cancellationToken).ConfigureAwait(false);
        EnsureSuccess(login);

        var monitor = await SendRequestAsync(CstaXml.MonitorStartRequest(), cancellationToken).ConfigureAwait(false);
        EnsureSuccess(monitor);
        MonitorCrossRefId = CstaXml.ReadMonitorCrossRefId(monitor.Xml);
        if (string.IsNullOrWhiteSpace(MonitorCrossRefId))
        {
            throw new CstaProtocolException("MX accepted MonitorStart without a monitorCrossRefID.");
        }
    }

    public async Task<CstaPacket> SendRequestAsync(string xml, CancellationToken cancellationToken)
    {
        if (_socket.State != WebSocketState.Open)
        {
            throw new InvalidOperationException("The MX WebSocket is not open.");
        }

        await _sendLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        var invokeId = -1;
        try
        {
            invokeId = AllocateInvokeId();
            var completion = new TaskCompletionSource<CstaPacket>(TaskCreationOptions.RunContinuationsAsynchronously);
            if (!_pending.TryAdd(invokeId, completion))
            {
                throw new InvalidOperationException($"Invoke ID {invokeId} is already pending.");
            }

            try
            {
                var wireMessage = ZultysPacketCodec.Encode(new CstaPacket(invokeId, xml));
                var bytes = Encoding.UTF8.GetBytes(wireMessage);
                await _socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, cancellationToken)
                    .ConfigureAwait(false);
            }
            catch
            {
                _pending.TryRemove(invokeId, out _);
                throw;
            }

            try
            {
                return await completion.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                // A cancelled caller must not leave an invoke ID permanently
                // reserved while the MX response is absent or arrives late.
                _pending.TryRemove(invokeId, out _);
            }
        }
        finally
        {
            _sendLock.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        _stopped.Cancel();
        foreach (var pending in _pending.Values)
        {
            pending.TrySetException(new OperationCanceledException("The MX WebSocket session has stopped."));
        }

        if (_socket.State is WebSocketState.Open or WebSocketState.CloseReceived)
        {
            try
            {
                await _socket.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "Bridge stopping", CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (WebSocketException)
            {
                // The peer may already be unavailable during shutdown.
            }
        }

        if (_receiveTask is not null)
        {
            try
            {
                await _receiveTask.ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (_stopped.IsCancellationRequested)
            {
            }
        }

        _socket.Dispose();
        _sendLock.Dispose();
        _stopped.Dispose();
    }

    private async Task ReceiveLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var wireMessage = await ReceiveTextMessageAsync(cancellationToken).ConfigureAwait(false);
                if (wireMessage is null)
                {
                    break;
                }

                var packet = ZultysPacketCodec.Decode(wireMessage);
                if (packet.InvokeId == CstaPacket.EventInvokeId)
                {
                    await DispatchEventAsync(packet.Xml).ConfigureAwait(false);
                }
                else if (_pending.TryRemove(packet.InvokeId, out var completion))
                {
                    completion.TrySetResult(packet);
                }
                else
                {
                    BridgeLog.UnexpectedMxResponse(_logger);
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            foreach (var pending in _pending.Values)
            {
                pending.TrySetException(exception);
            }

            throw;
        }
        finally
        {
            foreach (var pending in _pending.Values)
            {
                pending.TrySetException(new IOException("The MX WebSocket receive loop ended."));
            }
        }
    }

    private async Task<string?> ReceiveTextMessageAsync(CancellationToken cancellationToken)
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
                throw new CstaProtocolException("MX sent a non-text WebSocket message.");
            }

            message.Write(buffer, 0, result.Count);
            if (message.Length > ZultysPacketCodec.MaxPacketBytes + 128)
            {
                throw new CstaProtocolException("MX sent a WebSocket message larger than the CSTA limit.");
            }
        }
        while (!result.EndOfMessage);

        return Encoding.UTF8.GetString(message.GetBuffer(), 0, checked((int)message.Length));
    }

    private async Task DispatchEventAsync(string xml)
    {
        var cstaEvent = CstaXml.ParseEvent(xml);
        if (string.Equals(cstaEvent.Name, "keepalive", StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        var callbacks = EventReceived;
        if (callbacks is null)
        {
            return;
        }

        foreach (var callback in callbacks.GetInvocationList().Cast<Func<MxEvent, Task>>())
        {
            await callback(cstaEvent).ConfigureAwait(false);
        }
    }

    private int AllocateInvokeId()
    {
        for (var attempt = 0; attempt < CstaPacket.EventInvokeId; attempt++)
        {
            var candidate = Interlocked.Increment(ref _nextInvokeId) - 1;
            candidate %= CstaPacket.EventInvokeId;
            if (!_pending.ContainsKey(candidate))
            {
                return candidate;
            }
        }

        throw new InvalidOperationException("All CSTA command invoke IDs are currently in use.");
    }

    private static void EnsureSuccess(CstaPacket response)
    {
        if (!CstaXml.IsSuccessfulResponse(response.Xml, out var responseName, out var error))
        {
            throw new CstaProtocolException($"MX {responseName} failed: {error ?? "unknown error"}.");
        }
    }
}

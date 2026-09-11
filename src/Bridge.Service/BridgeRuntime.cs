using Zultys.NCall.Bridge.Calls;
using Zultys.NCall.Bridge.Contracts;
using Zultys.NCall.Bridge.Csta;

namespace Zultys.NCall.Bridge.Service;

/// <summary>
/// Owns the current MX session and the normalized call snapshot exposed to TAPI.
/// </summary>
internal sealed class BridgeRuntime
{
    private readonly object _gate = new();
    private readonly ILoggerFactory _loggerFactory;
    private readonly ILogger<BridgeRuntime> _logger;
    private readonly BridgeProgramOptions _options;
    private readonly CallStateReducer _calls = new();
    private BridgeHealth _health = new(BridgeConnectionState.Unconfigured, "No configuration loaded.", DateTimeOffset.UtcNow, 0);
    private BridgeConfiguration? _configuration;
    private CstaWebSocketSession? _session;

    public BridgeRuntime(
        ILoggerFactory loggerFactory,
        ILogger<BridgeRuntime> logger,
        BridgeProgramOptions options)
    {
        _loggerFactory = loggerFactory;
        _logger = logger;
        _options = options;
    }

    public event Action<BridgeSnapshot>? Changed;
    public event Action<BridgeCallNotification>? CallChanged;

    public BridgeSnapshot Snapshot()
    {
        lock (_gate)
        {
            return new BridgeSnapshot(_health, _calls.Snapshot());
        }
    }

    public async Task RunAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested)
        {
            var configuration = LoadConfiguration();
            if (configuration is null || !configuration.IsComplete)
            {
                lock (_gate)
                {
                    _configuration = null;
                }

                SetHealth(BridgeConnectionState.Unconfigured, "Configuration is missing or incomplete; waiting for configuration.");
                await Task.Delay(TimeSpan.FromSeconds(2), stoppingToken).ConfigureAwait(false);
                continue;
            }

            lock (_gate)
            {
                _configuration = configuration;
            }

            CstaWebSocketSession? session = null;
            try
            {
                SetHealth(BridgeConnectionState.Connecting, "Connecting to MX.");
                session = new CstaWebSocketSession(_loggerFactory.CreateLogger<CstaWebSocketSession>());
                session.EventReceived += ApplyEventAsync;

                SetHealth(BridgeConnectionState.Authenticating, "Authenticating to MX.");
                await session.StartAsync(configuration, stoppingToken).ConfigureAwait(false);

                lock (_gate)
                {
                    _session = session;
                }

                SetHealth(BridgeConnectionState.Ready, "Monitoring the configured MX device.");
                await session.Completion.WaitAsync(stoppingToken).ConfigureAwait(false);
                throw new IOException("The MX WebSocket connection closed.");
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception exception)
            {
                BridgeLog.MxConnectionUnavailable(_logger, exception);
                SetHealth(BridgeConnectionState.Degraded, "MX connection is unavailable; retrying.");
                await Task.Delay(TimeSpan.FromSeconds(3), stoppingToken).ConfigureAwait(false);
            }
            finally
            {
                lock (_gate)
                {
                    if (ReferenceEquals(_session, session))
                    {
                        _session = null;
                    }
                }

                if (session is not null)
                {
                    await session.DisposeAsync().ConfigureAwait(false);
                }
            }
        }

        SetHealth(BridgeConnectionState.Disconnected, "Bridge service stopped.");
    }

    public async Task<BridgeCommandResult> ExecuteAsync(BridgeCommandRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        CstaWebSocketSession? session;
        lock (_gate)
        {
            session = _session;
        }

        if (session is null)
        {
            return new BridgeCommandResult(false, "Unavailable", "MX monitoring is not ready.");
        }

        try
        {
            var xml = BuildCommand(request, GetConfiguredDeviceId());
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            using var commandCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                timeout.Token);
            var response = await session.SendRequestAsync(xml, commandCancellation.Token).ConfigureAwait(false);
            if (!CstaXml.IsSuccessfulResponse(response.Xml, out var responseName, out var error))
            {
                return new BridgeCommandResult(false, "Rejected", error ?? responseName);
            }

            if (string.Equals(responseName, "MakeCallResponse", StringComparison.Ordinal) &&
                CstaXml.TryReadMakeCallConnection(response.Xml, out var connection) &&
                connection is not null)
            {
                return new BridgeCommandResult(true, "Completed", responseName, connection.CallId, connection.DeviceId);
            }

            return new BridgeCommandResult(true, "Completed", responseName);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return new BridgeCommandResult(false, "Timeout", "The command was cancelled or timed out.");
        }
        catch (OperationCanceledException)
        {
            return new BridgeCommandResult(false, "OutcomeUnknown", "MX did not respond before the command timeout.");
        }
        catch (CstaProtocolException exception)
        {
            return new BridgeCommandResult(false, "ProtocolError", exception.Message);
        }
        catch (Exception exception)
        {
            BridgeLog.MxCommandFailed(_logger, exception);
            return new BridgeCommandResult(false, "OutcomeUnknown", "The MX command outcome could not be determined.");
        }
    }

    private BridgeConfiguration? LoadConfiguration()
    {
        try
        {
            return BridgeConfigurationStore.Load(_options.ConfigPath);
        }
        catch (Exception exception)
        {
            BridgeLog.ConfigurationLoadFailed(_logger, exception);
            return null;
        }
    }

    private Task ApplyEventAsync(MxEvent cstaEvent)
    {
        try
        {
            CallSnapshot before;
            CallSnapshot snapshot;
            BridgeHealth health;
            IReadOnlyList<BridgeCallNotification> changes;
            lock (_gate)
            {
                before = _calls.Snapshot();
                snapshot = _calls.Apply(cstaEvent);
                _health = _health with { UpdatedAt = DateTimeOffset.UtcNow, SnapshotRevision = snapshot.Revision };
                health = _health;
                changes = GetCallChanges(before, snapshot);
            }

            Publish(new BridgeSnapshot(health, snapshot));
            foreach (var change in changes)
            {
                PublishCallChange(change);
            }
        }
        catch (CstaProtocolException exception)
        {
            BridgeLog.MalformedCstaEvent(_logger, exception, cstaEvent.Name);
        }

        return Task.CompletedTask;
    }

    private string GetConfiguredDeviceId()
    {
        lock (_gate)
        {
            return _configuration?.DeviceId
                ?? throw new InvalidOperationException("No MX device is configured.");
        }
    }

    private static string BuildCommand(BridgeCommandRequest request, string defaultDeviceId)
    {
        return Required(request.Operation, "operation").ToLowerInvariant() switch
        {
            "makecall" => CstaXml.MakeCallRequest(
                request.DeviceId ?? defaultDeviceId,
                Required(request.Destination, "destination")),
            "clearconnection" => CstaXml.ConnectionRequest(MxCommand.ClearConnection, ReadCallKey(request.CallId, request.DeviceId, defaultDeviceId)),
            "holdcall" => CstaXml.ConnectionRequest(MxCommand.HoldCall, ReadCallKey(request.CallId, request.DeviceId, defaultDeviceId)),
            "retrievecall" => CstaXml.ConnectionRequest(MxCommand.RetrieveCall, ReadCallKey(request.CallId, request.DeviceId, defaultDeviceId)),
            "singlesteptransfercall" => CstaXml.SingleStepTransferRequest(ReadCallKey(request.CallId, request.DeviceId, defaultDeviceId), Required(request.Destination, "destination")),
            "transfercall" => CstaXml.TransferCallRequest(
                ReadCallKey(request.CallId, request.DeviceId, defaultDeviceId),
                ReadCallKey(request.ActiveCallId, request.ActiveDeviceId, defaultDeviceId)),
            _ => throw new CstaProtocolException($"Unsupported bridge command: {request.Operation}.")
        };
    }

    private static CallKey ReadCallKey(string? callId, string? deviceId, string defaultDeviceId) =>
        new(Required(callId, "callId"), string.IsNullOrWhiteSpace(deviceId) ? defaultDeviceId : deviceId);

    private static string Required(string? value, string name) =>
        !string.IsNullOrWhiteSpace(value)
            ? value
            : throw new CstaProtocolException($"The command requires {name}.");

    private void SetHealth(BridgeConnectionState state, string detail)
    {
        BridgeSnapshot snapshot;
        lock (_gate)
        {
            _health = new BridgeHealth(state, detail, DateTimeOffset.UtcNow, _calls.Snapshot().Revision);
            snapshot = new BridgeSnapshot(_health, _calls.Snapshot());
        }

        Publish(snapshot);
    }

    private void Publish(BridgeSnapshot snapshot)
    {
        try
        {
            Changed?.Invoke(snapshot);
        }
        catch (Exception exception)
        {
            BridgeLog.SnapshotSubscriberFailed(_logger, exception);
        }
    }

    private void PublishCallChange(BridgeCallNotification change)
    {
        try
        {
            CallChanged?.Invoke(change);
        }
        catch (Exception exception)
        {
            BridgeLog.CallSubscriberFailed(_logger, exception);
        }
    }

    private static List<BridgeCallNotification> GetCallChanges(CallSnapshot before, CallSnapshot after)
    {
        var previous = before.Calls.ToDictionary(call => call.Key);
        var current = after.Calls.ToDictionary(call => call.Key);
        var changes = new List<BridgeCallNotification>();

        foreach (var call in after.Calls)
        {
            if (!previous.TryGetValue(call.Key, out var oldCall) || oldCall.Revision != call.Revision)
            {
                changes.Add(ToNotification(call));
            }
        }

        foreach (var call in before.Calls)
        {
            if (!current.ContainsKey(call.Key))
            {
                changes.Add(ToNotification(call) with { Removed = true, Revision = after.Revision });
            }
        }

        return changes;
    }

    internal static BridgeCallNotification ToNotification(BridgeCall call) => new(
        call.Key.CallId,
        call.Key.DeviceId,
        call.State.ToString(),
        call.Direction.ToString(),
        call.Calling.Number,
        call.Calling.DisplayName,
        call.Called.Number,
        call.OriginalCalledNumber,
        call.Revision);
}

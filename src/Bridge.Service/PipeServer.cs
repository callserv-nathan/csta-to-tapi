using System.Collections.Concurrent;
using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Threading.Channels;
using System.Text.Json;
using Zultys.NCall.Bridge.Contracts;
using Zultys.NCall.Bridge.Ipc;

namespace Zultys.NCall.Bridge.Service;

internal sealed class PipeServer
{
    public const string PipeName = "Zultys.NCall.Bridge.v1";

    private readonly BridgeRuntime _runtime;
    private readonly ILogger<PipeServer> _logger;

    public PipeServer(BridgeRuntime runtime, ILogger<PipeServer> logger)
    {
        _runtime = runtime;
        _logger = logger;
    }

    public async Task RunAsync(CancellationToken stoppingToken)
    {
        var clients = new ConcurrentDictionary<long, Task>();
        var nextClientId = 0L;
        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                var pipe = CreatePipe();
                try
                {
                    await pipe.WaitForConnectionAsync(stoppingToken).ConfigureAwait(false);
                    var clientId = Interlocked.Increment(ref nextClientId);
                    var client = HandleConnectionAsync(pipe, stoppingToken);
                    if (!clients.TryAdd(clientId, client))
                    {
                        throw new InvalidOperationException("Could not track named-pipe client.");
                    }

                    _ = client.ContinueWith(
                        _ =>
                        {
                            clients.TryRemove(clientId, out Task? ignored);
                        },
                        CancellationToken.None,
                        TaskContinuationOptions.ExecuteSynchronously,
                        TaskScheduler.Default);
                }
                catch
                {
                    pipe.Dispose();
                    throw;
                }
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
        }
        finally
        {
            await Task.WhenAll(clients.Values).ConfigureAwait(false);
        }
    }

    private async Task HandleConnectionAsync(NamedPipeServerStream pipe, CancellationToken stoppingToken)
    {
        using (pipe)
        {
            try
            {
                await HandleClientAsync(pipe, stoppingToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
            }
            catch (IOException exception)
            {
                BridgeLog.PipeClientDisconnected(_logger, exception);
            }
            catch (Exception exception)
            {
                BridgeLog.PipeClientFailed(_logger, exception);
            }
        }
    }

    private async Task HandleClientAsync(NamedPipeServerStream pipe, CancellationToken stoppingToken)
    {
        var outgoing = Channel.CreateUnbounded<PipeEnvelope>(new UnboundedChannelOptions
        {
            SingleReader = true,
            SingleWriter = false,
        });
        var sequence = 0L;
        var subscribed = 0;
        void PublishCallChange(BridgeCallNotification change)
        {
            if (Volatile.Read(ref subscribed) != 0)
            {
                outgoing.Writer.TryWrite(PipeProtocol.Create("CallChanged", Interlocked.Increment(ref sequence), null, change));
            }
        }

        _runtime.CallChanged += PublishCallChange;
        var writer = Task.Run(async () =>
        {
            await foreach (var message in outgoing.Reader.ReadAllAsync(stoppingToken).ConfigureAwait(false))
            {
                await PipeProtocol.WriteAsync(pipe, message, stoppingToken).ConfigureAwait(false);
            }
        }, stoppingToken);

        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                var request = await PipeProtocol.ReadAsync(pipe, stoppingToken).ConfigureAwait(false);
                if (request is null)
                {
                    break;
                }

                var response = await HandleRequestAsync(request, Interlocked.Increment(ref sequence), stoppingToken).ConfigureAwait(false);
                if (response is not null)
                {
                    await outgoing.Writer.WriteAsync(response, stoppingToken).ConfigureAwait(false);
                }

                if (string.Equals(request.Type, "Subscribe", StringComparison.Ordinal))
                {
                    var snapshot = _runtime.Snapshot();
                    Interlocked.Exchange(ref subscribed, 1);
                    foreach (var call in snapshot.Calls.Calls)
                    {
                        await outgoing.Writer.WriteAsync(
                            PipeProtocol.Create("CallChanged", Interlocked.Increment(ref sequence), null, BridgeRuntime.ToNotification(call)),
                            stoppingToken).ConfigureAwait(false);
                    }
                }
            }
        }
        finally
        {
            _runtime.CallChanged -= PublishCallChange;
            outgoing.Writer.TryComplete();
            try
            {
                await writer.ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
            }
        }
    }

    private async Task<PipeEnvelope?> HandleRequestAsync(PipeEnvelope request, long sequence, CancellationToken cancellationToken)
    {
        return request.Type switch
        {
            "Hello" => PipeProtocol.Create("HelloAck", sequence, request.RequestId, new
            {
                protocolVersion = PipeProtocol.Version,
                lineName = "Zultys nCall Bridge",
                health = _runtime.Snapshot().Health,
            }),
            "GetSnapshot" => PipeProtocol.Create("Snapshot", sequence, request.RequestId, _runtime.Snapshot()),
            "Subscribe" => PipeProtocol.Create("Subscribed", sequence, request.RequestId, new { protocolVersion = PipeProtocol.Version }),
            "GetHealth" => PipeProtocol.Create("Health", sequence, request.RequestId, _runtime.Snapshot().Health),
            "Command" => await ExecuteCommandAsync(request, sequence, cancellationToken).ConfigureAwait(false),
            _ => PipeProtocol.Create("Unsupported", sequence, request.RequestId, new { detail = "Unknown pipe message type." }),
        };
    }

    private async Task<PipeEnvelope> ExecuteCommandAsync(PipeEnvelope request, long sequence, CancellationToken cancellationToken)
    {
        BridgeCommandRequest? command;
        try
        {
            command = PipeProtocol.DeserializePayload<BridgeCommandRequest>(request.Payload);
        }
        catch (Exception exception) when (exception is JsonException or InvalidOperationException)
        {
            return PipeProtocol.Create("InvalidState", sequence, request.RequestId, new { detail = "Command payload is invalid." });
        }

        if (command is null)
        {
            return PipeProtocol.Create("InvalidState", sequence, request.RequestId, new { detail = "Command payload is missing." });
        }

        return PipeProtocol.Create(
            "CommandCompleted",
            sequence,
            request.RequestId,
            await _runtime.ExecuteAsync(command, cancellationToken).ConfigureAwait(false));
    }

    private static NamedPipeServerStream CreatePipe()
    {
        if (!OperatingSystem.IsWindows())
        {
            return new NamedPipeServerStream(
                PipeName,
                PipeDirection.InOut,
                NamedPipeServerStream.MaxAllowedServerInstances,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous);
        }

        var security = new PipeSecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        AddAccess(security, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null));
        AddAccess(security, new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
        AddAccess(security, new NTAccount("NT SERVICE", "TapiSrv").Translate(typeof(SecurityIdentifier)) as SecurityIdentifier
            ?? throw new InvalidOperationException("Could not resolve the TapiSrv service SID."));

        return NamedPipeServerStreamAcl.Create(
            PipeName,
            PipeDirection.InOut,
            NamedPipeServerStream.MaxAllowedServerInstances,
            PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous,
            inBufferSize: 0,
            outBufferSize: 0,
            security);
    }

    private static void AddAccess(PipeSecurity security, IdentityReference identity) =>
        security.AddAccessRule(new PipeAccessRule(identity, PipeAccessRights.FullControl, AccessControlType.Allow));
}

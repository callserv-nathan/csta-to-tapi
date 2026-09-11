namespace Zultys.NCall.Bridge.Service;

internal sealed class BridgeWorker : BackgroundService
{
    private readonly BridgeRuntime _runtime;
    private readonly PipeServer _pipeServer;
    private readonly ILogger<BridgeWorker> _logger;

    public BridgeWorker(BridgeRuntime runtime, PipeServer pipeServer, ILogger<BridgeWorker> logger)
    {
        _runtime = runtime;
        _pipeServer = pipeServer;
        _logger = logger;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        BridgeLog.Starting(_logger);
        await Task.WhenAll(
            _pipeServer.RunAsync(stoppingToken),
            _runtime.RunAsync(stoppingToken)).ConfigureAwait(false);
    }
}

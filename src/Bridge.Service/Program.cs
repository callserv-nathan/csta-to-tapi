using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

namespace Zultys.NCall.Bridge.Service;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var options = BridgeProgramOptions.Parse(args);
        var builder = Host.CreateApplicationBuilder(args);

        if (!options.RunAsConsole)
        {
            builder.Services.AddWindowsService(serviceOptions =>
            {
                serviceOptions.ServiceName = BridgeProgramOptions.ServiceName;
            });
        }

        if (OperatingSystem.IsWindows())
        {
            builder.Logging.AddEventLog(eventLogSettings =>
            {
                eventLogSettings.SourceName = BridgeProgramOptions.ServiceName;
            });
        }

        builder.Services.AddSingleton(options);
        builder.Services.AddSingleton<BridgeRuntime>();
        builder.Services.AddSingleton<PipeServer>();
        builder.Services.AddHostedService<BridgeWorker>();

        await builder.Build().RunAsync().ConfigureAwait(false);
    }
}

internal sealed record BridgeProgramOptions(bool RunAsConsole, string ConfigPath)
{
    public const string ServiceName = "ZultysNCallBridge";

    public static BridgeProgramOptions Parse(IReadOnlyList<string> args)
    {
        var runAsConsole = args.Contains("--console", StringComparer.OrdinalIgnoreCase);
        var configIndex = Enumerable.Range(0, args.Count)
            .FirstOrDefault(index => string.Equals(args[index], "--config", StringComparison.OrdinalIgnoreCase), -1);
        var configPath = configIndex >= 0 && configIndex + 1 < args.Count
            ? args[configIndex + 1]
            : Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                "Zultys nCall Bridge",
                "config.json");

        return new BridgeProgramOptions(runAsConsole, Path.GetFullPath(configPath));
    }
}

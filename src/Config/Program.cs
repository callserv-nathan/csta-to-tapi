using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Zultys.NCall.Bridge.Configuration;

namespace Zultys.NCall.Bridge.Config;

internal static class Program
{
    private static readonly JsonSerializerOptions SerializerOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
    };

    public static int Main(string[] args)
    {
        try
        {
            var command = args.FirstOrDefault()?.ToLowerInvariant();
            return command switch
            {
                "set" => Set(args[1..]),
                "simulator" => WriteSimulatorConfiguration(args[1..]),
                "show" => Show(args[1..]),
                _ => Usage(),
            };
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"Configuration failed: {exception.Message}");
            return 1;
        }
    }

    private static int Set(IReadOnlyList<string> args)
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("Saving an MX password requires Windows DPAPI.");
        }

        var options = OptionMap.Parse(args);
        var endpoint = options.Required("endpoint");
        var username = options.Required("username");
        var deviceId = options.Required("device");
        ValidateProductionEndpoint(endpoint);
        Console.Write("MX password: ");
        var password = ReadSecret();
        Console.WriteLine();
        if (string.IsNullOrWhiteSpace(password))
        {
            throw new ArgumentException("The MX password cannot be empty.");
        }

        var path = options.ConfigPath;
        Directory.CreateDirectory(Path.GetDirectoryName(path) ?? throw new InvalidOperationException("Configuration path has no parent directory."));
        var protectedPassword = Convert.ToBase64String(ProtectedData.Protect(
            Encoding.UTF8.GetBytes(password),
            optionalEntropy: null,
            DataProtectionScope.LocalMachine));
        var configuration = new PersistedConfiguration(endpoint, username, deviceId, protectedPassword, SimulatorMode: false);
        File.WriteAllText(path, JsonSerializer.Serialize(configuration, SerializerOptions));
        ConfigurationFileSecurity.Apply(path);
        Console.WriteLine($"Saved configuration to {path}.");
        return 0;
    }

    private static int WriteSimulatorConfiguration(IReadOnlyList<string> args)
    {
        var options = OptionMap.Parse(args);
        var endpoint = options.Optional("endpoint") ?? "ws://127.0.0.1:7780/ws";
        if (!Uri.TryCreate(endpoint, UriKind.Absolute, out var uri) || uri.Scheme != "ws")
        {
            throw new ArgumentException("Simulator endpoint must be an absolute ws:// URI.");
        }

        var configuration = new PersistedConfiguration(
            endpoint,
            options.Optional("username") ?? "simulator",
            options.Optional("device") ?? "210",
            ProtectedPassword: null,
            SimulatorMode: true);
        var path = options.ConfigPath;
        Directory.CreateDirectory(Path.GetDirectoryName(path) ?? throw new InvalidOperationException("Configuration path has no parent directory."));
        File.WriteAllText(path, JsonSerializer.Serialize(configuration, SerializerOptions));
        ConfigurationFileSecurity.Apply(path);
        Console.WriteLine($"Saved simulator configuration to {path}.");
        Console.WriteLine("Set ZULTYS_NCALL_TEST_PASSWORD before running the bridge in simulator mode.");
        return 0;
    }

    private static int Show(IReadOnlyList<string> args)
    {
        var path = OptionMap.Parse(args).ConfigPath;
        if (!File.Exists(path))
        {
            Console.WriteLine("No configuration exists.");
            return 2;
        }

        var configuration = JsonSerializer.Deserialize<PersistedConfiguration>(File.ReadAllText(path), SerializerOptions)
            ?? throw new InvalidDataException("Configuration is empty.");
        Console.WriteLine($"Endpoint: {configuration.Endpoint}");
        Console.WriteLine($"Username: {configuration.Username}");
        Console.WriteLine($"Device: {configuration.DeviceId}");
        Console.WriteLine($"Simulator mode: {configuration.SimulatorMode}");
        Console.WriteLine($"Password: {(string.IsNullOrWhiteSpace(configuration.ProtectedPassword) ? "not configured" : "configured")}");
        return 0;
    }

    private static int Usage()
    {
        Console.Error.WriteLine("Usage:");
        Console.Error.WriteLine("  ZultysNCallConfig set --endpoint wss://mx.example:7779 --username user --device 210 [--config path]");
        Console.Error.WriteLine("  ZultysNCallConfig simulator [--endpoint ws://127.0.0.1:7780/ws] [--username simulator] [--device 210] [--config path]");
        Console.Error.WriteLine("  ZultysNCallConfig show [--config path]");
        return 2;
    }

    private static void ValidateProductionEndpoint(string endpoint)
    {
        if (!Uri.TryCreate(endpoint, UriKind.Absolute, out var uri) || uri.Scheme != "wss" || uri.Port != 7779)
        {
            throw new ArgumentException("MX endpoint must be an absolute wss:// URI on TCP port 7779.");
        }
    }

    private static string ReadSecret()
    {
        var result = new StringBuilder();
        ConsoleKeyInfo key;
        while ((key = Console.ReadKey(intercept: true)).Key != ConsoleKey.Enter)
        {
            if (key.Key == ConsoleKey.Backspace)
            {
                if (result.Length > 0)
                {
                    result.Length--;
                    Console.Write("\b \b");
                }

                continue;
            }

            if (!char.IsControl(key.KeyChar))
            {
                result.Append(key.KeyChar);
                Console.Write('*');
            }
        }

        return result.ToString();
    }
}

internal sealed record PersistedConfiguration(
    string Endpoint,
    string Username,
    string DeviceId,
    string? ProtectedPassword,
    bool SimulatorMode);

internal sealed class OptionMap
{
    private readonly IReadOnlyDictionary<string, string> _values;

    private OptionMap(IReadOnlyDictionary<string, string> values)
    {
        _values = values;
    }

    public string ConfigPath => Path.GetFullPath(Optional("config") ?? Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
        "Zultys nCall Bridge",
        "config.json"));

    public static OptionMap Parse(IReadOnlyList<string> args)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (var index = 0; index < args.Count; index += 2)
        {
            if (!args[index].StartsWith("--", StringComparison.Ordinal) || index + 1 >= args.Count)
            {
                throw new ArgumentException($"Invalid option: {args[index]}.");
            }

            values.Add(args[index][2..], args[index + 1]);
        }

        return new OptionMap(values);
    }

    public string Required(string name) => Optional(name) ?? throw new ArgumentException($"Missing --{name}.");

    public string? Optional(string name) => _values.GetValueOrDefault(name);
}

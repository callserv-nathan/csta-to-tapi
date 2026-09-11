using System.Security.Cryptography;
using System.Text.Json;
using Zultys.NCall.Bridge.Configuration;

namespace Zultys.NCall.Bridge.Service;

public sealed record BridgeConfiguration(
    string Endpoint,
    string Username,
    string DeviceId,
    string? ProtectedPassword,
    bool SimulatorMode = false)
{
    public bool IsComplete =>
        Uri.TryCreate(Endpoint, UriKind.Absolute, out var endpoint) &&
        endpoint.Scheme is "wss" or "ws" &&
        !string.IsNullOrWhiteSpace(Username) &&
        !string.IsNullOrWhiteSpace(DeviceId) &&
        (!string.IsNullOrWhiteSpace(ProtectedPassword) || SimulatorMode);
}

internal static class BridgeConfigurationStore
{
    private static readonly JsonSerializerOptions SerializerOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
    };

    public static BridgeConfiguration? Load(string path)
    {
        if (!File.Exists(path))
        {
            return null;
        }

        return JsonSerializer.Deserialize<BridgeConfiguration>(File.ReadAllText(path), SerializerOptions);
    }

    public static void Save(string path, BridgeConfiguration configuration, string password)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(password);
        Directory.CreateDirectory(Path.GetDirectoryName(path) ?? throw new InvalidOperationException("Configuration path has no parent directory."));

        var protectedPassword = Convert.ToBase64String(ProtectedData.Protect(
            System.Text.Encoding.UTF8.GetBytes(password),
            optionalEntropy: null,
            DataProtectionScope.LocalMachine));
        var persisted = configuration with { ProtectedPassword = protectedPassword };
        File.WriteAllText(path, JsonSerializer.Serialize(persisted, SerializerOptions));
        ConfigurationFileSecurity.Apply(path);
    }

    public static string GetPassword(BridgeConfiguration configuration)
    {
        if (configuration.SimulatorMode)
        {
            var testPassword = Environment.GetEnvironmentVariable("ZULTYS_NCALL_TEST_PASSWORD");
            if (!string.IsNullOrWhiteSpace(testPassword))
            {
                return testPassword;
            }
        }

        if (string.IsNullOrWhiteSpace(configuration.ProtectedPassword))
        {
            throw new InvalidOperationException("No encrypted MX password is configured.");
        }

        var protectedBytes = Convert.FromBase64String(configuration.ProtectedPassword);
        return System.Text.Encoding.UTF8.GetString(ProtectedData.Unprotect(
            protectedBytes,
            optionalEntropy: null,
            DataProtectionScope.LocalMachine));
    }
}

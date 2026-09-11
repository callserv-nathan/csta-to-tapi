using System.IO.Pipes;
using System.Text.Json;
using Zultys.NCall.Bridge.Contracts;
using Zultys.NCall.Bridge.Ipc;

namespace Zultys.NCall.Bridge.Diagnostics;

internal static class Program
{
    private const string PipeName = "Zultys.NCall.Bridge.v1";
    private static readonly JsonSerializerOptions WriteOptions = new(JsonSerializerDefaults.Web) { WriteIndented = true };

    public static async Task<int> Main(string[] args)
    {
        var command = args.FirstOrDefault()?.ToLowerInvariant() ?? "health";
        if (command is not ("health" or "snapshot" or "watch" or "command"))
        {
            Console.Error.WriteLine("Usage: ZultysNCallDiag [health|snapshot|watch|command]");
            return 2;
        }

        try
        {
            await using var pipe = new NamedPipeClientStream(
                ".",
                PipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);
            await pipe.ConnectAsync(TimeSpan.FromSeconds(5), CancellationToken.None).ConfigureAwait(false);

            var sequence = 0L;
            await RequestAsync(pipe, PipeProtocol.Create("Hello", ++sequence, Guid.NewGuid().ToString("N"), new { protocolVersion = PipeProtocol.Version }), CancellationToken.None).ConfigureAwait(false);

            if (command == "watch")
            {
                var subscribed = await RequestAsync(pipe, PipeProtocol.Create("Subscribe", ++sequence, Guid.NewGuid().ToString("N"), new { }), CancellationToken.None).ConfigureAwait(false);
                Write(subscribed);
                while (await PipeProtocol.ReadAsync(pipe, CancellationToken.None).ConfigureAwait(false) is { } update)
                {
                    Write(update);
                }

                return 0;
            }

            if (command == "command")
            {
                var bridgeCommand = ParseCommand(args[1..]);
                var commandResponse = await RequestAsync(
                    pipe,
                    PipeProtocol.Create("Command", ++sequence, Guid.NewGuid().ToString("N"), bridgeCommand),
                    CancellationToken.None).ConfigureAwait(false);
                Write(commandResponse);
                return 0;
            }

            var type = command == "health" ? "GetHealth" : "GetSnapshot";
            var response = await RequestAsync(pipe, PipeProtocol.Create(type, ++sequence, Guid.NewGuid().ToString("N"), new { }), CancellationToken.None).ConfigureAwait(false);
            Write(response);
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"Bridge diagnostic failed: {exception.Message}");
            return 1;
        }
    }

    private static async Task<PipeEnvelope> RequestAsync(NamedPipeClientStream pipe, PipeEnvelope request, CancellationToken cancellationToken)
    {
        await PipeProtocol.WriteAsync(pipe, request, cancellationToken).ConfigureAwait(false);
        while (await PipeProtocol.ReadAsync(pipe, cancellationToken).ConfigureAwait(false) is { } response)
        {
            if (string.Equals(response.RequestId, request.RequestId, StringComparison.Ordinal))
            {
                return response;
            }
        }

        throw new EndOfStreamException("Bridge pipe closed before replying.");
    }

    private static void Write(PipeEnvelope envelope) =>
        Console.WriteLine(JsonSerializer.Serialize(envelope, WriteOptions));

    private static BridgeCommandRequest ParseCommand(string[] args)
    {
        if (args.Length == 0)
        {
            throw new ArgumentException("Usage: ZultysNCallDiag command <operation> [--call id] [--device id] [--destination number] [--active-call id] [--active-device id]");
        }

        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (var index = 1; index < args.Length; index += 2)
        {
            if (!args[index].StartsWith("--", StringComparison.Ordinal) || index + 1 >= args.Length)
            {
                throw new ArgumentException($"Invalid command option: {args[index]}.");
            }

            values.Add(args[index][2..], args[index + 1]);
        }

        return new BridgeCommandRequest(
            args[0],
            values.GetValueOrDefault("call"),
            values.GetValueOrDefault("device"),
            values.GetValueOrDefault("destination"),
            values.GetValueOrDefault("active-call"),
            values.GetValueOrDefault("active-device"));
    }
}

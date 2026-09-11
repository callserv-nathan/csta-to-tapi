using System.Buffers.Binary;
using System.Text.Json;

namespace Zultys.NCall.Bridge.Ipc;

public sealed record PipeEnvelope(
    string Type,
    long Sequence,
    string? RequestId,
    JsonElement Payload);

public static class PipeProtocol
{
    public const int Version = 1;
    public const int MaxMessageBytes = 64 * 1024;

    private static readonly JsonSerializerOptions SerializerOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = false,
    };

    public static async ValueTask WriteAsync(Stream stream, PipeEnvelope envelope, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(stream);
        ArgumentNullException.ThrowIfNull(envelope);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(envelope, SerializerOptions);
        if (bytes.Length > MaxMessageBytes)
        {
            throw new InvalidOperationException($"IPC message exceeds {MaxMessageBytes} bytes.");
        }

        Span<byte> length = stackalloc byte[sizeof(int)];
        BinaryPrimitives.WriteInt32LittleEndian(length, bytes.Length);
        await stream.WriteAsync(length.ToArray(), cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    public static async ValueTask<PipeEnvelope?> ReadAsync(Stream stream, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(stream);
        var header = new byte[sizeof(int)];
        if (!await ReadExactlyOrEndAsync(stream, header, cancellationToken).ConfigureAwait(false))
        {
            return null;
        }

        var length = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (length is <= 0 or > MaxMessageBytes)
        {
            throw new InvalidDataException($"IPC message length {length} is invalid.");
        }

        var payload = new byte[length];
        await stream.ReadExactlyAsync(payload, cancellationToken).ConfigureAwait(false);
        return JsonSerializer.Deserialize<PipeEnvelope>(payload, SerializerOptions)
            ?? throw new InvalidDataException("IPC message was empty.");
    }

    public static PipeEnvelope Create<T>(string type, long sequence, string? requestId, T payload) =>
        new(type, sequence, requestId, JsonSerializer.SerializeToElement(payload, SerializerOptions));

    public static T DeserializePayload<T>(JsonElement payload) =>
        payload.Deserialize<T>(SerializerOptions)
        ?? throw new InvalidDataException("IPC payload was empty.");

    private static async ValueTask<bool> ReadExactlyOrEndAsync(Stream stream, Memory<byte> buffer, CancellationToken cancellationToken)
    {
        var offset = 0;
        while (offset < buffer.Length)
        {
            var read = await stream.ReadAsync(buffer[offset..], cancellationToken).ConfigureAwait(false);
            if (read == 0)
            {
                if (offset == 0)
                {
                    return false;
                }

                throw new EndOfStreamException("IPC stream ended in the length header.");
            }

            offset += read;
        }

        return true;
    }
}

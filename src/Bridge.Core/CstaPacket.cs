using System.Globalization;
using System.Text;

namespace Zultys.NCall.Bridge.Csta;

/// <summary>
/// A Zultys WebSocket API packet. The API reserves invoke ID 9999 for events.
/// </summary>
public sealed record CstaPacket(int InvokeId, string Xml)
{
    public const int EventInvokeId = 9999;
}

/// <summary>
/// Implements the packet wrapper documented in Zultys WebSocket API Guide 1.0,
/// sections 4.3 and 4.4. The WebSocket payload is text: one character holding
/// the Base64-header length, then the Base64 encoded 8-byte CSTA header, then XML.
/// </summary>
public static class ZultysPacketCodec
{
    public const int HeaderBytes = 8;
    public const int MaxPacketBytes = ushort.MaxValue;
    private const int MinInvokeId = 0;

    public static string Encode(CstaPacket packet)
    {
        ArgumentNullException.ThrowIfNull(packet);
        ValidateInvokeId(packet.InvokeId);

        var xml = EnsureXmlDeclaration(packet.Xml);
        var xmlBytes = Encoding.UTF8.GetByteCount(xml);
        var totalLength = checked(xmlBytes + HeaderBytes);

        if (totalLength > MaxPacketBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(packet),
                $"The CSTA packet is {totalLength} bytes; the Zultys limit is {MaxPacketBytes} bytes.");
        }

        Span<byte> header = stackalloc byte[HeaderBytes];
        header[0] = 0;
        header[1] = 0;
        header[2] = (byte)(totalLength >> 8);
        header[3] = (byte)totalLength;
        Encoding.ASCII.GetBytes(packet.InvokeId.ToString("D4", CultureInfo.InvariantCulture), header[4..]);

        var headerBase64 = Convert.ToBase64String(header);
        if (headerBase64.Length > char.MaxValue)
        {
            throw new InvalidOperationException("The Base64 CSTA header length cannot be represented in one character.");
        }

        return string.Concat((char)headerBase64.Length, headerBase64, xml);
    }

    public static CstaPacket Decode(string wireMessage)
    {
        ArgumentException.ThrowIfNullOrEmpty(wireMessage);

        var base64Length = wireMessage[0];
        if (base64Length == 0 || wireMessage.Length <= base64Length)
        {
            throw new CstaProtocolException("The CSTA header-length prefix is invalid.");
        }

        var headerBase64 = wireMessage.AsSpan(1, base64Length);
        byte[] header;
        try
        {
            header = Convert.FromBase64String(headerBase64.ToString());
        }
        catch (FormatException exception)
        {
            throw new CstaProtocolException("The CSTA header is not valid Base64.", exception);
        }

        if (header.Length != HeaderBytes || header[0] != 0 || header[1] != 0)
        {
            throw new CstaProtocolException("The CSTA header has an unsupported format.");
        }

        var xml = wireMessage[(base64Length + 1)..];
        var actualLength = checked(Encoding.UTF8.GetByteCount(xml) + HeaderBytes);
        var declaredLength = (header[2] << 8) | header[3];
        if (declaredLength != actualLength)
        {
            throw new CstaProtocolException(
                $"The CSTA packet length is {declaredLength}, but the payload is {actualLength} bytes.");
        }

        var invokeText = Encoding.ASCII.GetString(header, 4, 4);
        if (!int.TryParse(invokeText, NumberStyles.None, CultureInfo.InvariantCulture, out var invokeId))
        {
            throw new CstaProtocolException("The CSTA invoke ID is not four decimal digits.");
        }

        ValidateInvokeId(invokeId);
        return new CstaPacket(invokeId, xml);
    }

    private static string EnsureXmlDeclaration(string xml)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(xml);
        return xml.TrimStart().StartsWith("<?xml", StringComparison.Ordinal)
            ? xml
            : $"<?xml version=\"1.0\" encoding=\"utf-8\"?>{xml}";
    }

    private static void ValidateInvokeId(int invokeId)
    {
        if (invokeId is < MinInvokeId or > CstaPacket.EventInvokeId)
        {
            throw new ArgumentOutOfRangeException(nameof(invokeId), "Invoke IDs must be between 0 and 9999.");
        }
    }
}

public sealed class CstaProtocolException : Exception
{
    public CstaProtocolException(string message)
        : base(message)
    {
    }

    public CstaProtocolException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}

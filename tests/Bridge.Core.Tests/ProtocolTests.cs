using System.Text;
using System.Text.Json;
using Zultys.NCall.Bridge.Calls;
using Zultys.NCall.Bridge.Csta;
using Zultys.NCall.Bridge.Ipc;
using Xunit;

namespace Zultys.NCall.Bridge.Tests;

public sealed class ProtocolTests
{
    [Fact]
    public void PacketCodecRoundTripsDocumentedHeaderWrapper()
    {
        const string xml = "<MonitorStart><monitorObject /></MonitorStart>";

        var wire = ZultysPacketCodec.Encode(new CstaPacket(42, xml));
        var headerLength = wire[0];
        var header = Convert.FromBase64String(wire.Substring(1, headerLength));

        Assert.Equal(8, header.Length);
        Assert.Equal((byte)0, header[0]);
        Assert.Equal((byte)0, header[1]);
        Assert.Equal("0042", Encoding.ASCII.GetString(header, 4, 4));

        var decoded = ZultysPacketCodec.Decode(wire);
        Assert.Equal(42, decoded.InvokeId);
        Assert.Contains("<MonitorStart>", decoded.Xml, StringComparison.Ordinal);
        Assert.StartsWith("<?xml", decoded.Xml, StringComparison.Ordinal);
    }

    [Fact]
    public void PacketCodecRejectsDeclaredLengthThatDoesNotMatchUtf8Payload()
    {
        var wire = ZultysPacketCodec.Encode(new CstaPacket(1, "<keepalive/>"));
        var headerLength = wire[0];
        var header = Convert.FromBase64String(wire.Substring(1, headerLength));
        header[3]++;
        var malformed = string.Concat((char)headerLength, Convert.ToBase64String(header), wire[(headerLength + 1)..]);

        Assert.Throws<CstaProtocolException>(() => ZultysPacketCodec.Decode(malformed));
    }

    [Fact]
    public void ReducerPreservesOriginalDidFromCadAtEstablished()
    {
        var reducer = new CallStateReducer();
        reducer.Apply(CstaXml.ParseEvent("""
            <DeliveredEvent>
              <connection><callID>16</callID><deviceID>210</deviceID></connection>
              <callingDevice><deviceIdentifier>0412345678</deviceIdentifier></callingDevice>
              <calledDevice><deviceIdentifier>1300000000</deviceIdentifier></calledDevice>
              <callingDisplayName>Caller</callingDisplayName>
            </DeliveredEvent>
            """));
        var snapshot = reducer.Apply(CstaXml.ParseEvent("""
            <EstablishedEvent>
              <establishedConnection><callID>16</callID><deviceID>210</deviceID><globalCallID>SIM-16</globalCallID><globalOrigCallID>SIM-16</globalOrigCallID></establishedConnection>
              <callingDevice><deviceIdentifier>0412345678</deviceIdentifier></callingDevice>
              <calledDevice><deviceIdentifier>1300000000</deviceIdentifier></calledDevice>
              <answeringDevice><deviceIdentifier>210</deviceIdentifier></answeringDevice>
              <direction>Incoming</direction>
              <cad name="__CALLED_PARTY_ORIG__" type="string">1300000000</cad>
              <cmdsAllowed><cmd>Transfer</cmd></cmdsAllowed>
            </EstablishedEvent>
            """));

        var call = Assert.Single(snapshot.Calls);
        Assert.Equal(BridgeCallState.Connected, call.State);
        Assert.Equal(CallDirection.Incoming, call.Direction);
        Assert.Equal("1300000000", call.OriginalCalledNumber);
        Assert.Contains("Transfer", call.AllowedCommands);
        Assert.Equal("SIM-16", call.GlobalCallId);
    }

    [Fact]
    public void ReadsMakeCallConnectionFromMxResponse()
    {
        const string response = """
            <MakeCallResponse>
              <callingDevice><callID>94</callID><deviceID>210</deviceID></callingDevice>
            </MakeCallResponse>
            """;

        Assert.True(CstaXml.TryReadMakeCallConnection(response, out var connection));
        Assert.NotNull(connection);
        Assert.Equal("94", connection.CallId);
        Assert.Equal("210", connection.DeviceId);
    }

    [Fact]
    public void TreatsErrorResponseWithoutCodeAsFailure()
    {
        Assert.False(CstaXml.IsSuccessfulResponse(
            "<errorResponse Error=\"Unknown call\" />",
            out var responseName,
            out var error));
        Assert.Equal("errorResponse", responseName);
        Assert.Equal("Unknown call", error);
    }

    [Fact]
    public async Task PipeProtocolRoundTripsLengthPrefixedJson()
    {
        await using var stream = new MemoryStream();
        var payload = JsonSerializer.SerializeToElement(new { value = "test" });
        var source = new PipeEnvelope("Hello", 7, "request-1", payload);

        await PipeProtocol.WriteAsync(stream, source, CancellationToken.None);
        stream.Position = 0;
        var result = await PipeProtocol.ReadAsync(stream, CancellationToken.None);

        Assert.NotNull(result);
        Assert.Equal(source.Type, result.Type);
        Assert.Equal(source.Sequence, result.Sequence);
        Assert.Equal(source.RequestId, result.RequestId);
        Assert.Equal("test", result.Payload.GetProperty("value").GetString());
    }
}

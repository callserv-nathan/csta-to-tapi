using System.Security;
using System.Xml.Linq;
using Zultys.NCall.Bridge.Calls;

namespace Zultys.NCall.Bridge.Csta;

public enum MxCommand
{
    Login,
    Logout,
    MonitorStart,
    MonitorStop,
    MakeCall,
    ClearConnection,
    HoldCall,
    RetrieveCall,
    SingleStepTransferCall,
    TransferCall,
}

public sealed record MxEvent(string Name, XElement Root);

public static class CstaXml
{
    private const string CstaNamespace = "http://www.ecma-international.org/standards/ecma-323/csta/ed4";

    public static string LoginRequest(string username, string password)
    {
        return $"<loginRequest type=\"User\" platform=\"3rdParty\" version=\"1.0.0.1\" forced=\"false\" " +
               "loginCapab=\"Audio|Video|Im|911Support|WebChat|ScreenSharing|VideoConf|SchedConf|HttpFileTransfer\" " +
               "mediaCapab=\"Voicemail|Fax|CallRec\" dcmode=\"phone\" webToken=\"\">" +
               $"<userName>{Escape(username)}</userName><pwd>{Escape(password)}</pwd></loginRequest>";
    }

    public static string LogoutRequest() => "<logout></logout>";

    public static string MonitorStartRequest() =>
        $"<MonitorStart xmlns=\"{CstaNamespace}\"><monitorObject><deviceObject></deviceObject></monitorObject><confEvents>true</confEvents></MonitorStart>";

    public static string MonitorStopRequest(string monitorCrossRefId) =>
        $"<MonitorStop xmlns=\"{CstaNamespace}\"><monitorCrossRefID>{Escape(monitorCrossRefId)}</monitorCrossRefID></MonitorStop>";

    public static string MakeCallRequest(string deviceId, string destination) =>
        $"<MakeCall xmlns=\"{CstaNamespace}\"><callingDevice typeOfNumber=\"deviceID\">{Escape(deviceId)}</callingDevice><calledDirectoryNumber>{Escape(destination)}</calledDirectoryNumber></MakeCall>";

    public static string ConnectionRequest(MxCommand command, CallKey connection)
    {
        var (element, connectionElement) = command switch
        {
            MxCommand.ClearConnection => ("ClearConnection", "connectionToBeCleared"),
            MxCommand.HoldCall => ("HoldCall", "callToBeHeld"),
            MxCommand.RetrieveCall => ("RetrieveCall", "callToBeRetrieved"),
            _ => throw new ArgumentOutOfRangeException(nameof(command)),
        };

        return $"<{element}><{connectionElement}><callID>{Escape(connection.CallId)}</callID><deviceID>{Escape(connection.DeviceId)}</deviceID></{connectionElement}></{element}>";
    }

    public static string SingleStepTransferRequest(CallKey connection, string destination) =>
        $"<SingleStepTransferCall><activeCall><callID>{Escape(connection.CallId)}</callID><deviceID>{Escape(connection.DeviceId)}</deviceID></activeCall><transferredTo>{Escape(destination)}</transferredTo></SingleStepTransferCall>";

    public static string TransferCallRequest(CallKey held, CallKey active) =>
        $"<TransferCall><heldCall><callID>{Escape(held.CallId)}</callID><deviceID>{Escape(held.DeviceId)}</deviceID></heldCall><activeCall><callID>{Escape(active.CallId)}</callID><deviceID>{Escape(active.DeviceId)}</deviceID></activeCall></TransferCall>";

    public static MxEvent ParseEvent(string xml)
    {
        var root = XDocument.Parse(xml, LoadOptions.None).Root
            ?? throw new CstaProtocolException("The CSTA XML document has no root element.");
        return new MxEvent(root.Name.LocalName, root);
    }

    public static bool IsSuccessfulResponse(string xml, out string rootName, out string? error)
    {
        var root = XDocument.Parse(xml, LoadOptions.None).Root
            ?? throw new CstaProtocolException("The CSTA XML response has no root element.");
        rootName = root.Name.LocalName;
        var code = root.Attribute("Code")?.Value;
        error = root.Attribute("Error")?.Value ?? root.ElementByLocalName("error")?.Value;
        if (!string.IsNullOrWhiteSpace(error) ||
            rootName.Contains("error", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        return string.IsNullOrWhiteSpace(code) || string.Equals(code, "0", StringComparison.Ordinal);
    }

    public static string? ReadMonitorCrossRefId(string xml)
    {
        var root = XDocument.Parse(xml, LoadOptions.None).Root;
        return root?.ElementByLocalName("monitorCrossRefID")?.Value.Trim();
    }

    public static bool TryReadMakeCallConnection(string xml, out CallKey? connection)
    {
        var root = XDocument.Parse(xml, LoadOptions.None).Root;
        var candidate = root?.ElementByLocalName("callingDevice")
            ?? root?.ElementByLocalName("connection")
            ?? root?.ElementByLocalName("call");
        var callId = candidate?.ValueByLocalName("callID");
        var deviceId = candidate?.ValueByLocalName("deviceID");
        if (string.IsNullOrWhiteSpace(callId) || string.IsNullOrWhiteSpace(deviceId))
        {
            connection = null;
            return false;
        }

        connection = new CallKey(callId, deviceId);
        return true;
    }

    public static XElement? ElementByLocalName(this XContainer node, string name) =>
        node.Elements().FirstOrDefault(element => string.Equals(element.Name.LocalName, name, StringComparison.Ordinal));

    public static string? ValueByLocalName(this XContainer node, string name) =>
        node.ElementByLocalName(name)?.Value.Trim();

    private static string Escape(string value) => SecurityElement.Escape(value) ?? string.Empty;
}

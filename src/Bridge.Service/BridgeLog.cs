using Microsoft.Extensions.Logging;

namespace Zultys.NCall.Bridge.Service;

internal static partial class BridgeLog
{
    [LoggerMessage(EventId = 1, Level = LogLevel.Information, Message = "Zultys nCall Bridge is starting.")]
    public static partial void Starting(ILogger logger);

    [LoggerMessage(EventId = 2, Level = LogLevel.Warning, Message = "MX connection is unavailable.")]
    public static partial void MxConnectionUnavailable(ILogger logger, Exception exception);

    [LoggerMessage(EventId = 3, Level = LogLevel.Warning, Message = "MX command failed.")]
    public static partial void MxCommandFailed(ILogger logger, Exception exception);

    [LoggerMessage(EventId = 4, Level = LogLevel.Error, Message = "Could not load bridge configuration.")]
    public static partial void ConfigurationLoadFailed(ILogger logger, Exception exception);

    [LoggerMessage(EventId = 5, Level = LogLevel.Warning, Message = "Ignored malformed CSTA event {EventName}.")]
    public static partial void MalformedCstaEvent(ILogger logger, Exception exception, string eventName);

    [LoggerMessage(EventId = 6, Level = LogLevel.Warning, Message = "A bridge snapshot subscriber failed.")]
    public static partial void SnapshotSubscriberFailed(ILogger logger, Exception exception);

    [LoggerMessage(EventId = 7, Level = LogLevel.Warning, Message = "A bridge call subscriber failed.")]
    public static partial void CallSubscriberFailed(ILogger logger, Exception exception);

    [LoggerMessage(EventId = 8, Level = LogLevel.Debug, Message = "Named-pipe client disconnected.")]
    public static partial void PipeClientDisconnected(ILogger logger, Exception exception);

    [LoggerMessage(EventId = 9, Level = LogLevel.Warning, Message = "Named-pipe client failed.")]
    public static partial void PipeClientFailed(ILogger logger, Exception exception);

    [LoggerMessage(EventId = 10, Level = LogLevel.Warning, Message = "Received an MX response with no pending invoke ID.")]
    public static partial void UnexpectedMxResponse(ILogger logger);
}

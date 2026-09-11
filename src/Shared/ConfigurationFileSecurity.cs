using System.Security.AccessControl;
using System.Security.Principal;

namespace Zultys.NCall.Bridge.Configuration;

/// <summary>
/// Applies the access control required for the machine-wide bridge
/// configuration. The bridge only needs to read this file after it has been
/// written by an elevated administrator.
/// </summary>
internal static class ConfigurationFileSecurity
{
    private const string BridgeServiceName = "ZultysNCallBridge";

    public static void Apply(string configurationPath)
    {
        if (!OperatingSystem.IsWindows())
        {
            return;
        }

        ArgumentException.ThrowIfNullOrWhiteSpace(configurationPath);
        var directoryPath = Path.GetDirectoryName(configurationPath)
            ?? throw new InvalidOperationException("Configuration path has no parent directory.");
        if (!File.Exists(configurationPath))
        {
            throw new FileNotFoundException("Configuration file must exist before its access control is applied.", configurationPath);
        }

        var localService = new SecurityIdentifier(WellKnownSidType.LocalServiceSid, null);
        var directorySecurity = new DirectorySecurity();
        directorySecurity.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        AddFullControl(directorySecurity, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null));
        AddFullControl(directorySecurity, new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
        AddReadAccess(directorySecurity, localService);
        AddServiceReadAccess(directorySecurity);
        new DirectoryInfo(directoryPath).SetAccessControl(directorySecurity);

        var fileSecurity = new FileSecurity();
        fileSecurity.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        AddFullControl(fileSecurity, new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null));
        AddFullControl(fileSecurity, new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null));
        AddReadAccess(fileSecurity, localService);
        AddServiceReadAccess(fileSecurity);
        new FileInfo(configurationPath).SetAccessControl(fileSecurity);
    }

    private static void AddServiceReadAccess(FileSystemSecurity security)
    {
        try
        {
            var serviceIdentity = new NTAccount("NT SERVICE", BridgeServiceName)
                .Translate(typeof(SecurityIdentifier));
            AddReadAccess(security, serviceIdentity);
        }
        catch (IdentityNotMappedException)
        {
            // The LocalService ACE remains sufficient when the service SID is
            // unavailable, including before the MSI creates the service.
        }
    }

    private static void AddFullControl(FileSystemSecurity security, IdentityReference identity) =>
        security.AddAccessRule(new FileSystemAccessRule(identity, FileSystemRights.FullControl, AccessControlType.Allow));

    private static void AddReadAccess(FileSystemSecurity security, IdentityReference identity) =>
        security.AddAccessRule(new FileSystemAccessRule(identity, FileSystemRights.ReadAndExecute, AccessControlType.Allow));
}

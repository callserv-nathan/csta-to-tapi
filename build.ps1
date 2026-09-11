[CmdletBinding()]
param(
    [ValidatePattern('^\d{1,3}\.\d{1,3}\.\d{1,5}$')]
    [string]$Version = '0.1.0',

    [ValidateSet('Release')]
    [string]$Configuration = 'Release',

    [string]$CmakeGenerator,

    [string]$CertificateThumbprint,

    [string]$TimestampUrl = 'http://timestamp.digicert.com',

    [switch]$RequireSignedPayloads,

    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter()]
        [string[]]$ArgumentList = @()
    )

    Write-Host ('> {0} {1}' -f $FilePath, ($ArgumentList -join ' '))
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $($LASTEXITCODE): $FilePath"
    }
}

function Reset-Directory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }

    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Require-File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description was not produced: $Path"
    }

    return $Path
}

function Resolve-Cmake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        $command = Get-Command cmake -ErrorAction SilentlyContinue
    }
    if ($null -ne $command) {
        return $command.Source
    }

    $programFilesX86 = ${env:ProgramFiles(x86)}
    if (-not [string]::IsNullOrWhiteSpace($programFilesX86)) {
        $vsWhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vsWhere -PathType Leaf) {
            $installPath = (& $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
            if (-not [string]::IsNullOrWhiteSpace($installPath)) {
                $bundledCmake = Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
                if (Test-Path -LiteralPath $bundledCmake -PathType Leaf) {
                    return $bundledCmake
                }
            }
        }
    }

    throw 'CMake was not found. Install CMake 3.25 or later, or Visual Studio Build Tools with CMake support.'
}

function Resolve-VisualStudioGenerator {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CmakePath,

        [Parameter()]
        [string]$RequestedGenerator
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedGenerator)) {
        return $RequestedGenerator
    }

    $capabilitiesJson = (& $CmakePath -E capabilities) -join [Environment]::NewLine
    $capabilities = $capabilitiesJson | ConvertFrom-Json
    $generators = @(
        $capabilities.generators |
            Where-Object { $_.name -like 'Visual Studio *' -and $_.platformSupport } |
            Sort-Object -Property name -Descending
    )
    if ($generators.Count -eq 0) {
        throw 'No Visual Studio CMake generator is available. Install the MSVC x64/x86 build tools and Windows SDK.'
    }

    return [string]$generators[0].name
}

function Resolve-RepositoryRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptRoot
    )

    if (-not $ScriptRoot.StartsWith('\\')) {
        return $ScriptRoot
    }

    # Windows tools invoked from a WSL UNC path can report successful output
    # without materializing all intermediate reference assemblies. Map the UNC
    # share for the duration of the build so MSBuild, CMake, and WiX all receive
    # conventional drive-qualified paths.
    $segments = $ScriptRoot.TrimStart('\').Split('\')
    if ($segments.Count -lt 3) {
        throw "Cannot map repository root '$ScriptRoot' to a temporary drive."
    }

    $shareRoot = '\\' + $segments[0] + '\' + $segments[1]
    $driveName = @('Z', 'Y', 'X', 'W', 'V', 'U', 'T', 'S', 'R', 'Q', 'O', 'N', 'M', 'L', 'I', 'H', 'G', 'F', 'E', 'D') |
        Where-Object { $null -eq (Get-PSDrive -Name $_ -ErrorAction SilentlyContinue) } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($driveName)) {
        throw "No free drive letter is available to map '$shareRoot'."
    }

    New-PSDrive -Name $driveName -PSProvider FileSystem -Root $shareRoot -Persist -Scope Script | Out-Null
    $script:mappedRepositoryDrive = $driveName
    $relativePath = ($segments | Select-Object -Skip 2) -join '\'
    return ('{0}:\{1}' -f $driveName, $relativePath)
}

function Resolve-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $programFilesX86 = ${env:ProgramFiles(x86)}
    if (-not [string]::IsNullOrWhiteSpace($programFilesX86)) {
        $sdkBin = Join-Path $programFilesX86 'Windows Kits\10\bin'
        if (Test-Path -LiteralPath $sdkBin -PathType Container) {
            $candidate = Get-ChildItem -LiteralPath $sdkBin -Filter signtool.exe -File -Recurse |
                Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
                Sort-Object -Property FullName -Descending |
                Select-Object -First 1
            if ($null -ne $candidate) {
                return $candidate.FullName
            }
        }
    }

    throw 'SignTool was not found. Install the Windows SDK signing tools or put signtool.exe on PATH.'
}

function Sign-File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SignToolPath,

        [Parameter(Mandatory = $true)]
        [string]$Thumbprint,

        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter()]
        [string]$TimestampServer
    )

    $arguments = @('sign', '/fd', 'SHA256', '/sha1', $Thumbprint)
    if (-not [string]::IsNullOrWhiteSpace($TimestampServer)) {
        $arguments += @('/tr', $TimestampServer, '/td', 'SHA256')
    }
    $arguments += $Path
    Invoke-Checked -FilePath $SignToolPath -ArgumentList $arguments
}

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'This build must run on 64-bit Windows because it builds and packages a 64-bit TAPI provider.'
}

$mappedRepositoryDrive = $null
trap {
    $failure = $_
    if ($null -ne $script:mappedRepositoryDrive) {
        Remove-PSDrive -Name $script:mappedRepositoryDrive -Force -ErrorAction SilentlyContinue
        $script:mappedRepositoryDrive = $null
    }

    throw $failure
}

$versionFields = $Version.Split('.') | ForEach-Object { [int]$_ }
if ($versionFields[0] -gt 255 -or $versionFields[1] -gt 255 -or $versionFields[2] -gt 65535) {
    throw 'MSI version fields must be 0-255.0-255.0-65535.'
}

$repositoryRoot = Resolve-RepositoryRoot -ScriptRoot $PSScriptRoot
Set-Location -LiteralPath $repositoryRoot
$dotnet = (Get-Command dotnet.exe -ErrorAction Stop).Source
$cmake = Resolve-Cmake
$generator = Resolve-VisualStudioGenerator -CmakePath $cmake -RequestedGenerator $CmakeGenerator

$artifactsRoot = Join-Path $repositoryRoot 'artifacts'
$publishRoot = Join-Path $artifactsRoot 'publish'
$stageRoot = Join-Path $artifactsRoot 'stage'
$nativeBuildRoot = Join-Path $artifactsRoot 'cmake'
$installerRoot = Join-Path $repositoryRoot 'installer'
$installerBin = Join-Path $repositoryRoot 'installer\bin'
$installerObj = Join-Path $repositoryRoot 'installer\obj'
New-Item -ItemType Directory -Path $artifactsRoot -Force | Out-Null
Reset-Directory -Path $publishRoot
Reset-Directory -Path $stageRoot
Reset-Directory -Path $nativeBuildRoot
foreach ($installerBuildDirectory in @($installerBin, $installerObj)) {
    if (Test-Path -LiteralPath $installerBuildDirectory) {
        Remove-Item -LiteralPath $installerBuildDirectory -Recurse -Force
    }
}

$serviceProject = Join-Path $repositoryRoot 'src\Bridge.Service\Bridge.Service.csproj'
$configProject = Join-Path $repositoryRoot 'src\Config\Config.csproj'
$diagnosticsProject = Join-Path $repositoryRoot 'src\Diagnostics\Diagnostics.csproj'
$testsRoot = Join-Path $repositoryRoot 'tests'
$testProjects = if (Test-Path -LiteralPath $testsRoot -PathType Container) {
    @(Get-ChildItem -LiteralPath $testsRoot -Filter '*.csproj' -File -Recurse | Sort-Object -Property FullName)
} else {
    @()
}

# A Windows build can follow a Linux/WSL build in the same working tree. Clear
# configuration-specific intermediates so MSBuild cannot reuse file lists from
# the other host or runtime identifier.
Invoke-Checked -FilePath $dotnet -ArgumentList @('clean', $serviceProject, '-c', $Configuration, '-r', 'win-x64', '--nologo', '-v:q')
Invoke-Checked -FilePath $dotnet -ArgumentList @('clean', $configProject, '-c', $Configuration, '-r', 'win-x64', '--nologo', '-v:q')
Invoke-Checked -FilePath $dotnet -ArgumentList @('clean', $diagnosticsProject, '-c', $Configuration, '-r', 'win-x64', '--nologo', '-v:q')
foreach ($testProject in $testProjects) {
    Invoke-Checked -FilePath $dotnet -ArgumentList @('clean', $testProject.FullName, '-c', $Configuration, '--nologo', '-v:q')
}

Invoke-Checked -FilePath $dotnet -ArgumentList @('restore', $serviceProject, '-r', 'win-x64', '--nologo')
Invoke-Checked -FilePath $dotnet -ArgumentList @('restore', $configProject, '-r', 'win-x64', '--nologo')
Invoke-Checked -FilePath $dotnet -ArgumentList @('restore', $diagnosticsProject, '-r', 'win-x64', '--nologo')
foreach ($testProject in $testProjects) {
    Invoke-Checked -FilePath $dotnet -ArgumentList @('restore', $testProject.FullName, '--nologo')
}

if (-not $SkipTests) {
    foreach ($testProject in $testProjects) {
        Invoke-Checked -FilePath $dotnet -ArgumentList @('test', $testProject.FullName, '-c', $Configuration, '--no-restore', '--nologo')
    }
}

$servicePublish = Join-Path $publishRoot 'bridge'
$configPublish = Join-Path $publishRoot 'config'
$diagnosticsPublish = Join-Path $publishRoot 'diagnostics'
Invoke-Checked -FilePath $dotnet -ArgumentList @(
    'publish', $serviceProject, '-c', $Configuration, '-r', 'win-x64',
    '--self-contained', 'true', '--no-restore', '--nologo', '-o', $servicePublish
)
Invoke-Checked -FilePath $dotnet -ArgumentList @(
    'publish', $configProject, '-c', $Configuration, '-r', 'win-x64',
    '--self-contained', 'true', '--no-restore', '--nologo',
    '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true',
    '-o', $configPublish
)
Invoke-Checked -FilePath $dotnet -ArgumentList @(
    'publish', $diagnosticsProject, '-c', $Configuration, '-r', 'win-x64',
    '--self-contained', 'true', '--no-restore', '--nologo',
    '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true',
    '-o', $diagnosticsPublish
)

$tspBuild = Join-Path $nativeBuildRoot 'Tsp.Provider'
$providerRegistrationBuild = Join-Path $nativeBuildRoot 'ProviderRegistration'
Invoke-Checked -FilePath $cmake -ArgumentList @(
    '-S', (Join-Path $repositoryRoot 'src\Tsp.Provider'),
    '-B', $tspBuild,
    '-G', $generator,
    '-A', 'x64'
)
Invoke-Checked -FilePath $cmake -ArgumentList @('--build', $tspBuild, '--config', $Configuration, '--parallel')
Invoke-Checked -FilePath $cmake -ArgumentList @(
    '-S', (Join-Path $repositoryRoot 'src\ProviderRegistration'),
    '-B', $providerRegistrationBuild,
    '-G', $generator,
    '-A', 'x64'
)
Invoke-Checked -FilePath $cmake -ArgumentList @('--build', $providerRegistrationBuild, '--config', $Configuration, '--parallel')

$bridgeStage = Join-Path $stageRoot 'bridge'
$bridgeRuntimeStage = Join-Path $bridgeStage 'runtime'
$configStage = Join-Path $stageRoot 'config'
$diagnosticsStage = Join-Path $stageRoot 'diagnostics'
$tspStage = Join-Path $stageRoot 'tsp'
$registrationStage = Join-Path $stageRoot 'registration'
New-Item -ItemType Directory -Path $bridgeRuntimeStage, $configStage, $diagnosticsStage, $tspStage, $registrationStage -Force | Out-Null

$serviceExe = Require-File -Path (Join-Path $servicePublish 'ZultysNCallBridge.exe') -Description 'Bridge service executable'
$configExe = Require-File -Path (Join-Path $configPublish 'ZultysNCallConfig.exe') -Description 'Single-file configuration executable'
$diagnosticsExe = Require-File -Path (Join-Path $diagnosticsPublish 'ZultysNCallDiag.exe') -Description 'Single-file diagnostics executable'
$tsp = Require-File -Path (Join-Path $tspBuild ($Configuration + '\ZultysNCallTsp.tsp')) -Description 'TSP'
$providerRegistrationExe = Require-File -Path (Join-Path $providerRegistrationBuild ($Configuration + '\ZultysNCallProviderReg.exe')) -Description 'Provider registration helper'

$stagedServiceExe = Join-Path $bridgeStage 'ZultysNCallBridge.exe'
$stagedConfigExe = Join-Path $configStage 'ZultysNCallConfig.exe'
$stagedDiagnosticsExe = Join-Path $diagnosticsStage 'ZultysNCallDiag.exe'
$stagedTsp = Join-Path $tspStage 'ZultysNCallTsp.tsp'
$stagedProviderRegistrationExe = Join-Path $registrationStage 'ZultysNCallProviderReg.exe'
Copy-Item -LiteralPath $serviceExe -Destination $stagedServiceExe -Force
Get-ChildItem -LiteralPath $servicePublish -Force |
    Where-Object { $_.Name -ne 'ZultysNCallBridge.exe' -and $_.Extension -ne '.pdb' } |
    Copy-Item -Destination $bridgeRuntimeStage -Recurse -Force
Copy-Item -LiteralPath $configExe -Destination $stagedConfigExe -Force
Copy-Item -LiteralPath $diagnosticsExe -Destination $stagedDiagnosticsExe -Force
Copy-Item -LiteralPath $tsp -Destination $stagedTsp -Force
Copy-Item -LiteralPath $providerRegistrationExe -Destination $stagedProviderRegistrationExe -Force

$signTool = $null
if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $signTool = Resolve-SignTool
    $signablePayloads = Get-ChildItem -LiteralPath $stageRoot -File -Recurse |
        Where-Object { $_.Extension -in @('.exe', '.dll', '.tsp') }
    foreach ($payload in $signablePayloads) {
        Sign-File -SignToolPath $signTool -Thumbprint $CertificateThumbprint -Path $payload.FullName -TimestampServer $TimestampUrl
    }
} elseif ($RequireSignedPayloads) {
    throw 'RequireSignedPayloads requires CertificateThumbprint so the staged payloads can be signed.'
}

$wixProject = Join-Path $repositoryRoot 'installer\ZultysNCall.wixproj'
Invoke-Checked -FilePath $dotnet -ArgumentList @(
    'build', $wixProject, '-c', $Configuration, '--nologo',
    ('-p:ProductVersion=' + $Version),
    ('-p:BridgeServiceExe=' + $stagedServiceExe),
    ('-p:BridgeRuntimePayload=' + $bridgeRuntimeStage),
    ('-p:ConfigExe=' + $stagedConfigExe),
    ('-p:DiagnosticsExe=' + $stagedDiagnosticsExe),
    ('-p:TspDll=' + $stagedTsp),
    ('-p:ProviderRegistrationExe=' + $stagedProviderRegistrationExe),
    '-p:EnableProviderRegistration=true'
)

# WiX 6 writes the package under installer\obj by default. Search the
# freshly-cleaned installer tree so this remains valid if the SDK changes its
# output layout or a project later sets OutputPath explicitly.
$msi = Get-ChildItem -LiteralPath $installerRoot -Filter 'ZultysNCall.msi' -File -Recurse |
    Sort-Object -Property LastWriteTimeUtc -Descending |
    Select-Object -First 1
if ($null -eq $msi) {
    throw 'WiX did not produce ZultysNCall.msi.'
}

$finalMsi = Join-Path $artifactsRoot ('ZultysNCall-' + $Version + '-x64.msi')
Copy-Item -LiteralPath $msi.FullName -Destination $finalMsi -Force
if ($null -ne $signTool) {
    Sign-File -SignToolPath $signTool -Thumbprint $CertificateThumbprint -Path $finalMsi -TimestampServer $TimestampUrl
}

Write-Host ''
Write-Host ('MSI: {0}' -f $finalMsi)
if ($null -eq $signTool) {
    Write-Host 'The MSI and payloads are unsigned. Supply -CertificateThumbprint for a release build.'
}

if ($null -ne $mappedRepositoryDrive) {
    Remove-PSDrive -Name $mappedRepositoryDrive -Force
    $mappedRepositoryDrive = $null
}

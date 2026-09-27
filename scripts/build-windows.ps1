# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param(
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "RelWithDebInfo",

    [ValidateSet("windows-x64", "windows-ci-x64")]
    [string]$Preset = "windows-x64",

    [switch]$SkipPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$BuildDirectory = Join-Path $RepositoryRoot "build_x64"
$ReleaseDirectory = Join-Path $RepositoryRoot "release"

function Resolve-CMakeExecutable {
    $Command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($null -ne $Command) {
        return $Command.Source
    }

    $BundledCMake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path -LiteralPath $BundledCMake -PathType Leaf) {
        return $BundledCMake
    }

    throw "CMake 3.28 or newer was not found. Install Visual Studio 2022 Build Tools with CMake support."
}

function Assert-CMakeVersion {
    param([Parameter(Mandatory = $true)][string]$Executable)

    $VersionOutput = & $Executable --version
    if ($LASTEXITCODE -ne 0 -or $VersionOutput.Count -eq 0) {
        throw "Unable to query the CMake version: $Executable"
    }

    $VersionMatch = [regex]::Match(($VersionOutput -join "`n"), 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)')
    if (!$VersionMatch.Success -or [version]$VersionMatch.Groups[1].Value -lt [version]'3.28.0') {
        throw "CMake 3.28 or newer is required. Found: $($VersionOutput[0])"
    }
}

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $Executable $($Arguments -join ' ')"
    }
}

function Assert-WorkspacePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Resolved = [System.IO.Path]::GetFullPath($Path)
    $WorkspacePrefix = $RepositoryRoot.TrimEnd('\') + '\'
    if (!$Resolved.StartsWith($WorkspacePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the repository: $Resolved"
    }

    return $Resolved
}

function Assert-ArchiveContents {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArchivePath,

        [Parameter(Mandatory = $true)]
        [string[]]$RequiredEntries,

        [Parameter(Mandatory = $true)]
        [string]$PackageName
    )

    $Archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $Entries = @($Archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        foreach ($RequiredEntry in $RequiredEntries) {
            if ($Entries -notcontains $RequiredEntry) {
                throw "$PackageName archive is missing: $RequiredEntry"
            }
        }

        $ForbiddenEntry = $Entries | Where-Object {
            $_ -match '(^|/)(obs64\.exe|obs-studio\.exe|libobs\.dll|Qt6[^/]*\.dll|.+\.(msi|exe|pdb))$'
        } | Select-Object -First 1
        if ($null -ne $ForbiddenEntry) {
            throw "$PackageName archive contains a forbidden runtime, installer, or debug symbol: $ForbiddenEntry"
        }
    }
    finally {
        $Archive.Dispose()
    }
}

$CMake = Resolve-CMakeExecutable
Assert-CMakeVersion -Executable $CMake
$CTest = Join-Path (Split-Path -Parent $CMake) "ctest.exe"
if (!(Test-Path -LiteralPath $CTest -PathType Leaf)) {
    throw "CTest was not found next to CMake: $CTest"
}
New-Item -ItemType Directory -Path $ReleaseDirectory -Force | Out-Null

$StagingRoot = Assert-WorkspacePath (Join-Path $ReleaseDirectory (".stage-" + [System.Guid]::NewGuid().ToString("N")))
$InstallRoot = Join-Path $StagingRoot "install"
$StandardPackageRoot = Join-Path $StagingRoot "standard"
$PortableRoot = Join-Path $StagingRoot "portable"

try {
    Invoke-CheckedCommand -Executable $CMake -Arguments @(
        "--preset", $Preset,
        "-DBUILD_TESTING=ON",
        "-DCMAKE_INSTALL_PREFIX=$InstallRoot"
    )
    Invoke-CheckedCommand -Executable $CMake -Arguments @(
        "--build", "--preset", $Preset,
        "--config", $Configuration,
        "--parallel"
    )
    Invoke-CheckedCommand -Executable $CTest -Arguments @(
        "--test-dir", $BuildDirectory,
        "--build-config", $Configuration,
        "--no-tests=error",
        "--output-on-failure"
    )

    if ($SkipPackage) {
        Write-Host "Build completed: $BuildDirectory"
        return
    }

    Invoke-CheckedCommand -Executable $CMake -Arguments @(
        "--install", $BuildDirectory,
        "--config", $Configuration,
        "--prefix", $InstallRoot
    )

    $InstalledPlugin = Join-Path $InstallRoot "obs-easy-multistream"
    $InstalledDll = Join-Path $InstalledPlugin "bin\64bit\obs-easy-multistream.dll"
    $InstalledData = Join-Path $InstalledPlugin "data"
    if (!(Test-Path -LiteralPath $InstalledDll -PathType Leaf)) {
        throw "Installed plugin DLL was not found: $InstalledDll"
    }
    if (!(Test-Path -LiteralPath $InstalledData -PathType Container)) {
        throw "Installed plugin data was not found: $InstalledData"
    }

    $StandardPluginDirectory = Join-Path $StandardPackageRoot "obs-easy-multistream"
    $StandardBinaryDirectory = Join-Path $StandardPluginDirectory "bin\64bit"
    $StandardDataDirectory = Join-Path $StandardPluginDirectory "data"
    New-Item -ItemType Directory -Path $StandardBinaryDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $StandardDataDirectory -Force | Out-Null
    Copy-Item -LiteralPath $InstalledDll -Destination $StandardBinaryDirectory
    Get-ChildItem -LiteralPath $InstalledData | Copy-Item -Destination $StandardDataDirectory -Recurse
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot "LICENSE") -Destination (Join-Path $StandardPluginDirectory "LICENSE.txt")

    $PortableBinaryDirectory = Join-Path $PortableRoot "obs-plugins\64bit"
    $PortableDataDirectory = Join-Path $PortableRoot "data\obs-plugins\obs-easy-multistream"
    New-Item -ItemType Directory -Path $PortableBinaryDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $PortableDataDirectory -Force | Out-Null

    Copy-Item -LiteralPath $InstalledDll -Destination $PortableBinaryDirectory
    Get-ChildItem -LiteralPath $InstalledData | Copy-Item -Destination $PortableDataDirectory -Recurse
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot "LICENSE") -Destination (Join-Path $PortableRoot "obs-easy-multistream-LICENSE.txt")

    $BuildSpec = Get-Content -LiteralPath (Join-Path $RepositoryRoot "buildspec.json") -Raw | ConvertFrom-Json
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $StandardArchiveName = "obs-easy-multistream-$($BuildSpec.version)-windows-x64-obs32.zip"
    $StandardArchivePath = Assert-WorkspacePath (Join-Path $ReleaseDirectory $StandardArchiveName)
    $StagedStandardArchivePath = Assert-WorkspacePath (Join-Path $StagingRoot $StandardArchiveName)
    Compress-Archive -Path (Join-Path $StandardPackageRoot "*") -DestinationPath $StagedStandardArchivePath -CompressionLevel Optimal -Force
    Assert-ArchiveContents -ArchivePath $StagedStandardArchivePath -PackageName "Standard" -RequiredEntries @(
        "obs-easy-multistream/bin/64bit/obs-easy-multistream.dll",
        "obs-easy-multistream/data/locale/en-US.ini",
        "obs-easy-multistream/data/locale/ja-JP.ini",
        "obs-easy-multistream/LICENSE.txt"
    )

    $PortableArchiveName = "obs-easy-multistream-$($BuildSpec.version)-windows-x64-obs32-portable.zip"
    $PortableArchivePath = Assert-WorkspacePath (Join-Path $ReleaseDirectory $PortableArchiveName)
    $StagedPortableArchivePath = Assert-WorkspacePath (Join-Path $StagingRoot $PortableArchiveName)
    Compress-Archive -Path (Join-Path $PortableRoot "*") -DestinationPath $StagedPortableArchivePath -CompressionLevel Optimal -Force
    Assert-ArchiveContents -ArchivePath $StagedPortableArchivePath -PackageName "Portable" -RequiredEntries @(
        "obs-plugins/64bit/obs-easy-multistream.dll",
        "data/obs-plugins/obs-easy-multistream/locale/en-US.ini",
        "data/obs-plugins/obs-easy-multistream/locale/ja-JP.ini",
        "obs-easy-multistream-LICENSE.txt"
    )

    Move-Item -LiteralPath $StagedStandardArchivePath -Destination $StandardArchivePath -Force
    Move-Item -LiteralPath $StagedPortableArchivePath -Destination $PortableArchivePath -Force
    $StandardHash = Get-FileHash -LiteralPath $StandardArchivePath -Algorithm SHA256
    $PortableHash = Get-FileHash -LiteralPath $PortableArchivePath -Algorithm SHA256
    Write-Host "Build and package completed."
    Write-Host "Standard archive: $StandardArchivePath"
    Write-Host "Standard SHA256: $($StandardHash.Hash.ToLowerInvariant())"
    Write-Host "Portable archive: $PortableArchivePath"
    Write-Host "Portable SHA256: $($PortableHash.Hash.ToLowerInvariant())"
}
finally {
    if (Test-Path -LiteralPath $StagingRoot) {
        $ValidatedStagingRoot = Assert-WorkspacePath $StagingRoot
        Remove-Item -LiteralPath $ValidatedStagingRoot -Recurse -Force
    }
}

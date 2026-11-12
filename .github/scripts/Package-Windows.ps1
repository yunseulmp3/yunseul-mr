[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    # Installer (.exe) made with Inno Setup. A problem here must not take the
    # zip away, so failures only warn.
    Log-Group "Building installer for ${ProductName}..."
    $IsccCommand = Get-Command -Name iscc -ErrorAction SilentlyContinue
    $Iscc = if ( $IsccCommand ) { $IsccCommand.Source } else { "${Env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }

    if ( ! ( Test-Path -Path $Iscc ) ) {
        Write-Warning 'Inno Setup not found, trying to install it'
        try {
            choco install innosetup --no-progress -y | Out-Null
        } catch {
            Write-Warning "$_"
        }
    }

    if ( Test-Path -Path $Iscc ) {
        Remove-Item -ErrorAction SilentlyContinue -Path "${ProjectRoot}/release/${ProductName}-*-windows-*-Installer.exe"
        $IsccArgs = @(
            "/DMyAppVersion=${ProductVersion}"
            "/DSourceDir=${ProjectRoot}\release\${Configuration}"
            "/DProjectRoot=${ProjectRoot}"
            "/DOutputDir=${ProjectRoot}\release"
            "/DOutputName=${OutputName}-Installer"
            "${ProjectRoot}\build-aux\installer-windows.iss"
        )
        & $Iscc @IsccArgs
        if ( $LASTEXITCODE -ne 0 ) {
            Write-Warning "Installer build failed (exit code ${LASTEXITCODE}); only the zip is published"
        }
    } else {
        Write-Warning 'Inno Setup is not available; only the zip is published'
    }
    Log-Group
}

Package

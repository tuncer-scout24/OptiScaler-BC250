[CmdletBinding()]
param(
    [string]$OutputDirectory = "artifacts",
    [ValidatePattern('^(bc250-alpha\.[0-9]+|v[0-9]+\.[0-9]+\.[0-9]+-beta\.[0-9]+)?$')]
    [string]$ReleaseTag = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

Push-Location $repoRoot
try {
    if (-not (Get-Command msbuild -ErrorAction SilentlyContinue)) {
        throw "MSBuild is required. Run from a Visual Studio 2022 Developer PowerShell with the C++ workload installed."
    }

    # Release moves the DLL during its post-build event, so rebuild for repeatable packaging.
    & msbuild OptiScaler.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x64 /verbosity:minimal /bl:build.binlog
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }

    $packageRoot = Join-Path $repoRoot "x64/Release/a"
    $requiredFiles = @(
        "OptiScaler.dll",
        "OptiScaler.ini",
        "setup_linux.sh",
        "setup_windows.bat",
        "OptiScaler/amdvkffx64.dll",
        "OptiScaler/amdxcffx64.dll",
        "OptiScaler/amd_fidelityfx_loader_dx12.dll",
        "OptiScaler/amd_fidelityfx_upscaler_dx12.dll",
        "OptiScaler/D3D12_OptiScaler/D3D12Core.dll"
    )
    foreach ($file in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $file) -PathType Leaf)) {
            throw "Build package is missing $file."
        }
    }

    New-Item -ItemType Directory -Force -Path "$packageRoot/profiles", "$packageRoot/docs" | Out-Null
    Copy-Item "profiles/BC250.ini" "$packageRoot/profiles/BC250.ini" -Force
    Copy-Item "docs/BC250.md" "$packageRoot/docs/BC250.md" -Force
    Copy-Item "external/fsr4/README.md" "$packageRoot/docs/FSR4-providers.md" -Force
    Copy-Item LICENSE "$packageRoot/Licenses/OptiScaler_LICENSE.txt" -Force
    # Older builds placed GPL text at the package root; avoid carrying it into a rebuild.
    $legacyLicense = Join-Path $packageRoot "LICENSE"
    if ((Test-Path -LiteralPath $legacyLicense) -and
        (Get-FileHash -LiteralPath $legacyLicense).Hash -eq (Get-FileHash LICENSE).Hash) {
        Remove-Item -LiteralPath $legacyLicense -Force
    }

    $versionHeader = Get-Content "OptiScaler/resource.h" -Raw
    $versionParts = foreach ($key in @("VER_MAJOR_VERSION", "VER_MINOR_VERSION", "VER_HOTFIX_VERSION", "VER_BUILD_NUMBER")) {
        $match = [regex]::Match($versionHeader, "(?m)^\s*#define\s+$key\s+(\d+)")
        if (-not $match.Success) {
            throw "Missing version component $key in resource.h."
        }
        $match.Groups[1].Value
    }
    $commit = git rev-parse --short HEAD
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot determine the source commit."
    }
    $fileName = "OptiScaler_v$($versionParts[0]).$($versionParts[1]).$($versionParts[2])-pre$($versionParts[3])_BC250_$commit.zip"
    if ($ReleaseTag -and $ReleaseTag.StartsWith("bc250-alpha.")) {
        $alphaNumber = $ReleaseTag.Substring("bc250-alpha.".Length)
        $fileName = "OptiScaler_BC250_Alpha$alphaNumber.zip"
    }
    elseif ($ReleaseTag) {
        $fileName = "OptiScaler_BC250_${ReleaseTag}.zip"
    }
    $outputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
    New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
    $archive = Join-Path $outputPath $fileName
    Compress-Archive -Path "$packageRoot/*" -DestinationPath $archive -Force
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$archive.sha256" -Value "$hash  $fileName" -Encoding ascii
    Write-Output "Package: $archive"
}
finally {
    Pop-Location
}

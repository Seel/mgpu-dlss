<#
  Builds AAAFrameGeneration.dll (FG-GPU1) locally with the Visual Studio already installed.

  Usage (either shell works; when started from Windows PowerShell 5.1 the script re-runs
  itself under PowerShell 7 if pwsh is installed):
      pwsh -ExecutionPolicy Bypass -File D:\projects\mgpu-dlss\fo4-fg-gpu1\build-local.ps1

  Needs only: Visual Studio 2026 (or 2022) with "Desktop development with C++", Git, internet.
  First run: ~20-40 min (vcpkg builds boost/directxtk/... once). Later runs: a few minutes.

  Work happens in -WorkDir (short path on purpose: vcpkg breaks on long paths).
  Result: <WorkDir>\package\Data\...  - zip it or point Vortex/MO2 at it.
#>
param(
    [string]$WorkDir = "D:\fgb",
    [string]$UpstreamSha = "3e237a44269a8dc8cab54aab4ef0aae83c284a43",
    [string]$VcpkgSha = "b472291f295551b7127359ea38fdd2ca092f6f1b",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
# Windows PowerShell 5.1 renders a progress bar for every Invoke-WebRequest chunk,
# which makes the ~250 MB FidelityFX download several times slower.
$ProgressPreference = "SilentlyContinue"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Windows PowerShell 5.1 is supported, but this script is developed and tested on
# PowerShell 7 and the two differ in native-command/error behaviour (5.1 turns a native
# command's stderr into a terminating error, for example). If someone started us with
# `powershell`, hand over to pwsh when it is installed so the caller, an IDE or an agent
# never has to remember which one to use. `powershell.exe` is always 5.1; installing 7
# does not change it, which is why this cannot be fixed by configuration alone.
if ($PSVersionTable.PSEdition -eq 'Desktop' -and $PSCommandPath) {
    $pwsh = (Get-Command pwsh.exe -ErrorAction SilentlyContinue).Source
    if ($pwsh) {
        Write-Host "Windows PowerShell $($PSVersionTable.PSVersion) detected; re-running under PowerShell 7..." -ForegroundColor Yellow
        $forward = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
        foreach ($name in $PSBoundParameters.Keys) {
            $value = $PSBoundParameters[$name]
            if ($value -is [System.Management.Automation.SwitchParameter]) {
                if ($value.IsPresent) { $forward += "-$name" }
            } else {
                $forward += @("-$name", [string]$value)
            }
        }
        if ($args.Count -gt 0) { $forward += $args }
        & $pwsh @forward
        $code = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 1 }
        exit $code
    }
    Write-Warning "PowerShell 7 (pwsh) not found; continuing on Windows PowerShell $($PSVersionTable.PSVersion)."
}

function Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Run($exe, [string[]]$argList) {
    & $exe @argList
    if ($LASTEXITCODE -ne 0) { throw "$exe $($argList -join ' ') failed with exit code $LASTEXITCODE" }
}
function Fetch-Pinned($url, $dir, $sha, [switch]$Recursive) {
    if (-not (Test-Path "$dir\.git")) {
        New-Item -ItemType Directory -Force $dir | Out-Null
        Run git @("-C", $dir, "init", "-q")
        Run git @("-C", $dir, "remote", "add", "origin", $url)
    }
    # --verify --quiet keeps stderr empty when the repo has no HEAD yet. Windows
    # PowerShell 5.1 turns a native command's stderr into a terminating error while
    # $ErrorActionPreference is Stop, so a plain `rev-parse HEAD` aborts the script
    # on a fresh checkout (PowerShell 7 does not, which is why this hid for a while).
    $current = (& git -C $dir rev-parse --verify --quiet HEAD)
    if ($current -ne $sha) {
        Run git @("-C", $dir, "fetch", "--depth", "1", "origin", $sha)
        Run git @("-C", $dir, "checkout", "-q", "--force", "FETCH_HEAD")
    }
    if ($Recursive) { Run git @("-C", $dir, "submodule", "update", "--init", "--recursive", "--depth", "1") }
}

# ---------------------------------------------------------------- Visual Studio
Step "Locating Visual Studio"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio with the C++ x64 toolset found" }
$vsMajor = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Split('.')[0]
$generator = if ($vsMajor -eq "18") { "Visual Studio 18 2026" } else { "Visual Studio 17 2022" }
Write-Host "Visual Studio $vsMajor at $vsPath -> generator '$generator'"

if (-not $env:VSCMD_VER) {
    Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}
foreach ($tool in "git", "cmake", "cl") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "'$tool' not on PATH after loading the VS environment" }
}
Write-Host (cmake --version | Select-Object -First 1)

# ---------------------------------------------------------------- sources
if ($Clean -and (Test-Path "$WorkDir\FO4Upscaling\build")) { Remove-Item -Recurse -Force "$WorkDir\FO4Upscaling\build" }

Step "FO4Upscaling @ $($UpstreamSha.Substring(0,7)) (with submodules)"
$src = "$WorkDir\FO4Upscaling"
Fetch-Pinned "https://github.com/northaxosky/FO4Upscaling.git" $src $UpstreamSha -Recursive

Step "Applying the GPU1 overlay"
Copy-Item -Recurse -Force "$Here\overlay\*" $src
& git -C $src status --short

Step "vcpkg @ $($VcpkgSha.Substring(0,7))"
$vcpkg = "$WorkDir\vcpkg"
Fetch-Pinned "https://github.com/microsoft/vcpkg.git" $vcpkg $VcpkgSha
if (-not (Test-Path "$vcpkg\vcpkg.exe")) { Run "$vcpkg\bootstrap-vcpkg.bat" @("-disableMetrics") }
$env:VCPKG_ROOT = $vcpkg

# ---------------------------------------------------------------- build
Step "Configure"
Copy-Item -Force "$src\cmake\CommonLibF4.cmake.txt" "$src\extern\CommonLibF4\CMakeLists.txt"
Push-Location $src
try {
    Run cmake @("-S", ".", "--preset=default", "-G", $generator)
    Step "Build AAAFrameGeneration (Release)"
    Run cmake @("--build", "build", "--config", "Release", "--target", "AAAFrameGeneration")
} finally { Pop-Location }

# ---------------------------------------------------------------- package
Step "Package"
$pkg = "$WorkDir\package"
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
$plugins = "$pkg\Data\F4SE\Plugins"
New-Item -ItemType Directory -Force "$plugins\FrameGeneration\FidelityFX", "$pkg\Data\MCM\Config\FrameGeneration" | Out-Null
$bin = "$src\build\plugins\FrameGeneration\Release"
Copy-Item "$bin\AAAFrameGeneration.dll" $plugins
if (Test-Path "$bin\AAAFrameGeneration.pdb") { Copy-Item "$bin\AAAFrameGeneration.pdb" $plugins }
Copy-Item "$src\package\F4SE\Plugins\FrameGeneration\*.hlsl" "$plugins\FrameGeneration"
Copy-Item "$src\package\F4SE\Plugins\FrameGeneration\FidelityFX\LICENSE.txt" "$plugins\FrameGeneration\FidelityFX"
Copy-Item -Recurse "$src\package\MCM\Config\FrameGeneration\*" "$pkg\Data\MCM\Config\FrameGeneration"

# FidelityFX runtime (same version/hash as upstream scripts/sdk-manifest.sh)
$ffxZip = "$WorkDir\FidelityFX-SDK-v1.1.4.zip"
$ffxSha = "0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952"
if (-not (Test-Path $ffxZip)) {
    Invoke-WebRequest "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip" -OutFile $ffxZip
}
if ((Get-FileHash $ffxZip -Algorithm SHA256).Hash.ToLower() -ne $ffxSha) { Remove-Item $ffxZip; throw "FidelityFX zip hash mismatch" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($ffxZip)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -like "*PrebuiltSignedDLL/amd_fidelityfx_dx12.dll" } | Select-Object -First 1
    if (-not $entry) { throw "amd_fidelityfx_dx12.dll not in the FidelityFX zip" }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$plugins\FrameGeneration\FidelityFX\amd_fidelityfx_dx12.dll", $true)
} finally { $zip.Dispose() }
Copy-Item "$Here\README.md" "$pkg\FG-GPU1-README.md"

Step "Done"
Get-ChildItem -Recurse -File $pkg | ForEach-Object { $_.FullName.Substring($pkg.Length + 1) }
Write-Host "`nInstall: zip '$pkg' and add it in Vortex, or copy its Data folder into the game folder." -ForegroundColor Green

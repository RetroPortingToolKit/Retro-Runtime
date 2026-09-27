# Build retro-core-runner for THIS Windows machine, for local development: the
# runner a developer hands a port or app as an explicit runner (--runner,
# RETRO_CORE_RUNNER). Linux and macOS use scripts/build-local.sh; the two take
# the same options and print the same things.
#
# Not a release: scripts/package-release.sh is the release path, and its gates
# do not apply here. Kept from it: Release by default, the static MSVC runtime
# (/MT, no CRT DLLs to find), the version stamped as "dev" and the commit read
# from git (with -dirty when tracked files are modified).
#
# MSVC: when cl.exe is not on PATH, Visual Studio is found with vswhere and its
# developer environment entered for this script. Ninja is used when it is on
# PATH (Visual Studio ships one), else the Visual Studio generator.
#
# SDL3 is optional, found the way CMakeLists.txt finds it. Without it the
# runner cannot lend a GL context (no --gl); the output says which build you
# got. A shared SDL3.dll is copied beside the runner, where Windows finds it.
#
# The runner is checked before its path is printed: its --version must run and
# report "game_package 1". The LAST line on stdout is always
#   RETRO_CORE_RUNNER=<absolute path>
#
# usage: scripts\build-local.ps1 [-Debug] [-Out DIR] [-Build DIR]
#          [-Sdl3Prefix DIR | -NoSdl] [-Jobs N] [-Test] [-Help]
#
# Written 2026-09-26 and NOT YET RUN: no Windows machine was available. Whoever
# first runs it replaces this note with what they ran and saw.
param(
    [switch]$Debug,
    [string]$Out = "",
    [string]$Build = "",
    [string]$Sdl3Prefix = "",
    [switch]$NoSdl,
    [int]$Jobs = 0,
    [switch]$Test,
    [switch]$Help
)
$ErrorActionPreference = "Stop"

function Die([string]$msg) { [Console]::Error.WriteLine("build-local: $msg"); exit 1 }
function Say([string]$msg) { [Console]::Error.WriteLine("build-local: $msg") }
# Native tools: output to the console (stderr), never into this script's
# stdout. Their stderr must not become a terminating error (Windows PowerShell).
function Invoke-Native([string]$what, [string]$exe, [string[]]$argv) {
    $ErrorActionPreference = "Continue"
    & $exe @argv 2>&1 | ForEach-Object { [Console]::Error.WriteLine("$_") }
    if ($LASTEXITCODE -ne 0) { Die "$what failed (exit $LASTEXITCODE)" }
}

if ($Help) {
    @"
usage: scripts\build-local.ps1 [options]

Build retro-core-runner for this machine (local development, not a release)
and print its path last, as RETRO_CORE_RUNNER=<absolute path>.

  -Debug             Debug build (default: Release)
  -Out DIR           where the runner is copied
                     (default: <repo>\out\local\<platform>\)
  -Build DIR         CMake build directory
                     (default: <repo>\build-local, or build-local-debug)
  -Sdl3Prefix DIR    use the SDL3 installed under DIR (it must be found)
  -NoSdl             build without SDL3 (the runner then has no --gl)
  -Jobs N            parallel build jobs (default: all CPUs)
  -Test              run the test suite (ctest) after building
  -Help              this text

<platform> is windows-x86_64 or windows-arm64.
Relative paths are taken against the current directory.
"@
    exit 0
}
if ($PSVersionTable.PSEdition -eq "Core" -and -not $IsWindows) {
    Die "this script builds on Windows; use scripts/build-local.sh on Linux and macOS"
}
if ($Sdl3Prefix -and $NoSdl) { Die "-Sdl3Prefix and -NoSdl contradict each other" }
if ($Jobs -lt 0) { Die "-Jobs: want a positive number, got $Jobs" }

$Config = if ($Debug) { "Debug" } else { "Release" }
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).ProviderPath

$hostArch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
switch ($hostArch) {
    "AMD64" { $Arch = "x86_64"; $VsArch = "amd64"; $VsTools = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"; $VsPlatform = "x64" }
    "ARM64" { $Arch = "arm64";  $VsArch = "arm64"; $VsTools = "Microsoft.VisualStudio.Component.VC.Tools.ARM64";   $VsPlatform = "ARM64" }
    default { Die "$hostArch`: no platform name for this architecture" }
}
$Platform = "windows-$Arch"

# Paths from the command line are relative to where the script was run.
function Get-AbsDir([string]$p) {
    $full = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine((Get-Location).ProviderPath, $p))
    New-Item -ItemType Directory -Force -Path $full | Out-Null
    return $full.TrimEnd('\', '/')
}
if (-not $Build) { $Build = Join-Path $Root $(if ($Debug) { "build-local-debug" } else { "build-local" }) }
$Build = Get-AbsDir $Build
if (-not $Out) { $Out = Join-Path $Root "out\local\$Platform" }
$Out = Get-AbsDir $Out
if ($Out -eq $Build) { Die "-Out and -Build must differ" }
if ($Sdl3Prefix) {
    if (-not (Test-Path -PathType Container $Sdl3Prefix)) { Die "-Sdl3Prefix ${Sdl3Prefix}: no such directory" }
    $Sdl3Prefix = (Resolve-Path $Sdl3Prefix).ProviderPath
}
if ($Jobs -eq 0) { $Jobs = [Environment]::ProcessorCount }

# ---- MSVC: enter Visual Studio's developer environment if cl.exe is missing --
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { Die "cl.exe is not on PATH and vswhere.exe was not found (install Visual Studio with C++)" }
    $vs = & $vswhere -latest -products * -requires $VsTools -property installationPath
    if (-not $vs) { Die "no Visual Studio with the C++ tools ($VsTools) found by vswhere" }
    $devShell = Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    if (-not (Test-Path $devShell)) { Die "$devShell not found (Visual Studio 2019 16.2 or newer is needed)" }
    Say "entering the developer environment of $vs ($VsArch)"
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
        -DevCmdArguments "-arch=$VsArch -host_arch=$VsArch -no_logo" | Out-Null
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { Die "cl.exe is still not on PATH after entering $vs" }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Die "cmake not found" }

# ---- version stamp: "dev", and the commit with -dirty if it has changes ------
$Commit = ""
$ErrorActionPreference = "Continue"
if (Get-Command git -ErrorAction SilentlyContinue) {
    $Commit = (& git -C $Root rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { $Commit = "" }
    elseif ($Commit) {
        # Tracked files only, as git describe --dirty counts them.
        & git -C $Root diff --quiet HEAD -- 2>$null
        if ($LASTEXITCODE -ne 0) { $Commit += "-dirty" }
    }
}
$ErrorActionPreference = "Stop"

# ---- configure ----------------------------------------------------------------
$cmakeArgs = @(
    "-S", $Root, "-B", $Build,
    "-DRETRO_RUNTIME_VERSION=dev",
    "-DRETRO_RUNTIME_COMMIT=$Commit",   # empty: CMake reads git itself, else "unknown"
    # As the release does: /MT, so no CRT DLLs to find at run time.
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`$<`$<CONFIG:Debug>:Debug>"
)
$Ninja = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
if ($Ninja) {
    $cmakeArgs += @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl")
} else {
    $cmakeArgs += @("-A", $VsPlatform)   # the newest Visual Studio generator
}
if ($NoSdl) {
    $cmakeArgs += @("-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON")
} else {
    $cmakeArgs += @("-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=OFF", "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=OFF")
    if ($Sdl3Prefix) {
        $cmakeArgs += "-DCMAKE_PREFIX_PATH=$Sdl3Prefix"
        $cfg = Get-ChildItem -Path $Sdl3Prefix -Recurse -Filter SDL3Config.cmake -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cfg) { $cmakeArgs += "-DSDL3_DIR=$($cfg.DirectoryName)" }
    }
}

# A build directory configured with other SDL choices, or another generator,
# keeps them in its cache: start it afresh when the choices change.
$stamp = Join-Path $Build "build-local.args"
$argText = ($cmakeArgs -join "`n")
if ((Test-Path $stamp) -and ((Get-Content -Raw $stamp).TrimEnd() -ne $argText)) {
    Say "configuration changed since the last build in $Build; reconfiguring from scratch"
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Build "CMakeCache.txt")
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $Build "CMakeFiles")
}

Say "$Platform, $Config, build $Build"
Invoke-Native "cmake configure" "cmake" $cmakeArgs
Set-Content -Path $stamp -Value $argText
Invoke-Native "cmake build" "cmake" @("--build", $Build, "--config", $Config, "--parallel", "$Jobs")

if ($Test) {
    Invoke-Native "ctest" "ctest" @("--test-dir", $Build, "-C", $Config, "--output-on-failure", "--parallel", "$Jobs")
}

# ---- install into a stage, then copy flat into -Out ----------------------------
$stage = Join-Path $Build "build-local-stage"
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stage
Invoke-Native "cmake install" "cmake" @("--install", $Build, "--config", $Config, "--prefix", $stage)
$Runner = Join-Path $Out "retro-core-runner.exe"
Copy-Item -Force (Join-Path $stage "bin\retro-core-runner.exe") $Runner
Copy-Item -Force (Join-Path $Root "LICENSE") (Join-Path $Out "LICENSE")

# ---- SDL3: none, static, or SDL3.dll copied beside the runner -------------------
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Out "SDL3.dll")   # from an earlier build
$SdlNote = ""
$deps = @()
if (Get-Command dumpbin -ErrorAction SilentlyContinue) {
    $deps = @(& dumpbin -nologo -dependents $Runner)
} else {
    Say "dumpbin not found: cannot tell whether the runner imports SDL3.dll (the --version check below still runs it)"
}
if ($deps -match '^\s*SDL3\.dll\s*$') {
    # Where the linked SDL3 lives: -Sdl3Prefix, else the prefix CMake found it in,
    # else PATH. A development package holds one per architecture (lib\x64, ...).
    $roots = @()
    if ($Sdl3Prefix) { $roots += $Sdl3Prefix }
    $cache = Join-Path $Build "CMakeCache.txt"
    $dirLine = Select-String -Path $cache -Pattern '^SDL3_DIR:[A-Z]+=(.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dirLine) {
        $d = $dirLine.Matches[0].Groups[1].Value
        $roots += $d
        foreach ($i in 1..3) { $d = Split-Path -Parent $d; if ($d) { $roots += $d } }
    }
    $candidates = @()
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $candidates += Get-ChildItem -Path $r -Recurse -Filter SDL3.dll -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName }
        }
    }
    $onPath = Get-Command SDL3.dll -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($onPath) { $candidates += $onPath.Source }
    $archDir = if ($Arch -eq "arm64") { '\\arm64\\' } else { '\\x64\\' }
    $dll = ($candidates | Where-Object { $_ -match $archDir } | Select-Object -First 1)
    if (-not $dll) { $dll = ($candidates | Where-Object { $_ -notmatch '\\(x86|x64|arm64)\\' } | Select-Object -First 1) }
    if (-not $dll) { Die "retro-core-runner needs SDL3.dll and it was not found (pass -Sdl3Prefix, or -NoSdl)" }
    Copy-Item -Force $dll (Join-Path $Out "SDL3.dll")
    $SdlNote = "shared, copied beside the runner (SDL3.dll, from $dll)"
}

# ---- check the copy, then print where it is ------------------------------------
$report = @(& $Runner --version)
if ($LASTEXITCODE -ne 0) { Die "$Runner --version failed (exit $LASTEXITCODE)" }
$report = $report | ForEach-Object { "$_".TrimEnd("`r") }
function Get-Field([string]$key) {
    $line = $report | Where-Object { $_ -like "$key *" } | Select-Object -First 1
    if ($line) { return $line.Substring($key.Length + 1) } else { return "" }
}
if (-not ($report -contains "game_package 1")) { Die "$Runner --version has no 'game_package 1' line" }
if ((Get-Field "version") -ne "dev") { Die "$Runner says version '$(Get-Field version)', expected dev" }
if ($Commit -and (Get-Field "commit") -ne $Commit) { Die "$Runner says commit '$(Get-Field commit)', expected $Commit" }
if ((Get-Field "gl") -eq "1") {
    if (-not $SdlNote) { $SdlNote = "linked statically" }
    $SdlNote = "SDL3 $SdlNote; --gl available"
} else {
    if ($Sdl3Prefix) { Die "-Sdl3Prefix $Sdl3Prefix was given, but SDL3 was not found there" }
    $SdlNote = "no SDL3; --gl (lending a GL context) unavailable"
}

Write-Output "build-local: $Platform $Config runner in $Out"
Write-Output "build-local: $SdlNote"
$report | Write-Output
Write-Output "RETRO_CORE_RUNNER=$Runner"

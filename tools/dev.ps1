<#
.SYNOPSIS
    Prepares a Windows shell for building fumar, then optionally runs a command in it.

.DESCRIPTION
    Ninja plus clang-cl need a working MSVC environment. clang-cl is only the
    compiler driver: the standard library headers, the Windows SDK and the
    import libraries still come from Visual Studio, and they are located through
    the INCLUDE / LIB / LIBPATH environment variables. CMake does not set those
    up on its own, and the Visual Studio Developer Prompt is what normally does
    it, so this script does the same thing programmatically:

      1. locate a Visual Studio installation that ships clang-cl (via vswhere)
      2. import every variable that vcvars64.bat exports
      3. put clang-cl, cmake and ninja from the VS bundle on PATH
      4. make sure VULKAN_SDK is visible, even in a shell started before the
         SDK was installed

    Without step 2 the link step fails with unresolved symbols from libcmt or
    kernel32 - the classic symptom of a missing MSVC environment.

.EXAMPLE
    .\scripts\dev.ps1 cmake --preset windows-debug
    Configure the project.

.EXAMPLE
    .\scripts\dev.ps1 cmake --build --preset windows-debug
    Build it.

.EXAMPLE
    . .\scripts\dev.ps1
    Dot-source it to keep the environment in the current shell.
#>

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Command
)

$ErrorActionPreference = 'Stop'

# --- 1. find a Visual Studio installation that has clang-cl -----------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Is Visual Studio installed?"
}

$installs = & $vswhere -all -prerelease -products * -property installationPath
if (-not $installs) {
    throw "vswhere reported no Visual Studio installation."
}

$vsPath = $null
foreach ($candidate in $installs) {
    $hasClang  = Test-Path (Join-Path $candidate 'VC\Tools\Llvm\x64\bin\clang-cl.exe')
    $hasVcVars = Test-Path (Join-Path $candidate 'VC\Auxiliary\Build\vcvars64.bat')
    if ($hasClang -and $hasVcVars) { $vsPath = $candidate; break }
}

if (-not $vsPath) {
    throw @"
No Visual Studio installation with clang-cl was found.
Open the Visual Studio Installer and add the component:
  'C++ Clang Compiler for Windows' (Microsoft.VisualStudio.Component.VC.Llvm.Clang)
"@
}

# --- 2. import the MSVC environment ----------------------------------------
# vcvars64.bat only exists as a batch script, so we run it in cmd, dump the
# resulting environment with `set`, and copy each line back into this session.

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$dumped = cmd /c "call `"$vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
    throw "vcvars64.bat failed with exit code $LASTEXITCODE"
}

foreach ($line in $dumped) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2]
    }
}

# --- 3. add the tools bundled with Visual Studio to PATH -------------------
# vcvars does not expose clang-cl, and cmake/ninja ship inside the VS CMake
# component rather than as standalone installations.

$toolDirs = @(
    (Join-Path $vsPath 'VC\Tools\Llvm\x64\bin')
    (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin')
    (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
)

foreach ($dir in $toolDirs) {
    if ((Test-Path $dir) -and ($env:PATH -notlike "*$dir*")) {
        $env:PATH = "$dir;$env:PATH"
    }
}

# --- 4. Vulkan SDK ----------------------------------------------------------
# The SDK installer sets VULKAN_SDK machine-wide, but a shell started before
# the installation still has the old environment. Read it back from the
# registry so this works without a reboot.

if (-not $env:VULKAN_SDK) {
    $env:VULKAN_SDK = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine')
}
if (-not $env:VULKAN_SDK) {
    throw "VULKAN_SDK is not set. Install the Vulkan SDK: winget install KhronosGroup.VulkanSDK"
}

$vulkanBin = Join-Path $env:VULKAN_SDK 'Bin'
if ((Test-Path $vulkanBin) -and ($env:PATH -notlike "*$vulkanBin*")) {
    $env:PATH = "$vulkanBin;$env:PATH"
}

# --- 5. run the command, or report what we set up --------------------------

if ($Command -and $Command.Count -gt 0) {
    $exe = $Command[0]
    $rest = @()
    if ($Command.Count -gt 1) { $rest = $Command[1..($Command.Count - 1)] }

    # Windows PowerShell turns anything a native program writes to stderr into
    # an error record, and with 'Stop' in effect that aborts the run. Plenty of
    # well-behaved tools use stderr for progress - git prints "Cloning into..."
    # there - so the real success signal is the exit code, not stderr.
    $ErrorActionPreference = 'Continue'

    & $exe @rest
    exit $LASTEXITCODE
}

Write-Host "fumar dev environment ready" -ForegroundColor Green
Write-Host "  Visual Studio : $vsPath"
Write-Host "  clang-cl      : $((Get-Command clang-cl).Source)"
Write-Host "  cmake         : $((Get-Command cmake).Source)"
Write-Host "  ninja         : $((Get-Command ninja).Source)"
Write-Host "  Vulkan SDK    : $env:VULKAN_SDK"
Write-Host ""
Write-Host "Note: run this script with a command (.\scripts\dev.ps1 cmake --preset windows-debug)"
Write-Host "or dot-source it (. .\scripts\dev.ps1) to keep the environment in this shell."

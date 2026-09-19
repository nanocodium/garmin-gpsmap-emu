<#
.SYNOPSIS
    Build the gpsmap7x08 QEMU machine on Windows, using MSYS2/MinGW.

.DESCRIPTION
    QEMU is a POSIX-ish build (meson + a Unix shell), so the compiler and the
    libraries come from MSYS2's MINGW64 environment.  The result is a *native*
    Windows qemu-system-arm.exe that needs no MSYS2 at run time beyond the
    MinGW DLL directory on PATH (the tools add that themselves).

    Steps: install the MinGW packages, clone QEMU, copy the machine sources in
    (tools/patch_qemu_tree.py), configure and build.

.PARAMETER QemuDir
    Where the QEMU source tree lives.  Default: <MSYS2 home>\qemu-garmin.

.PARAMETER Msys2
    MSYS2 installation root.  Default: C:\msys64.

.PARAMETER Tag
    QEMU tag to clone.  Default: v10.2.1.

.PARAMETER Jobs
    Parallel compile jobs.  Default: number of processors minus two.

.EXAMPLE
    tools\win\install_qemu_machine.ps1
#>
[CmdletBinding()]
param(
    [string]$QemuDir,
    [string]$Msys2 = 'C:\msys64',
    [string]$Tag = 'v10.2.1',
    [int]$Jobs = 0,
    [switch]$SkipPackages
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'env.ps1')

$bash = Join-Path $Msys2 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) {
    throw @"
MSYS2 not found at $Msys2.
Install it from https://www.msys2.org (or: winget install MSYS2.MSYS2), then
re-run this script.  Pass -Msys2 <path> if it lives somewhere else.
"@
}
if ($Jobs -le 0) { $Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 2) }
if (-not $QemuDir) {
    $QemuDir = Join-Path $Msys2 ("home\" + $env:USERNAME + "\qemu-garmin")
}

function Invoke-Msys2([string]$Command, [string]$What) {
    Write-Host "==> $What" -ForegroundColor Cyan
    $env:MSYSTEM = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    & $bash -lc $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

# 1. build dependencies -----------------------------------------------------
# glib/pixman/zlib are QEMU's own requirements; libepoxy is what garmin_gl.c
# renders through; SDL2 is the display front end (GTK is not needed here).
$packages = @(
    'mingw-w64-x86_64-gcc', 'mingw-w64-x86_64-pkgconf', 'mingw-w64-x86_64-meson',
    'mingw-w64-x86_64-ninja', 'mingw-w64-x86_64-glib2', 'mingw-w64-x86_64-pixman',
    'mingw-w64-x86_64-zlib', 'mingw-w64-x86_64-libepoxy', 'mingw-w64-x86_64-SDL2',
    'mingw-w64-x86_64-python'
) -join ' '
if (-not $SkipPackages) {
    Invoke-Msys2 "pacman -S --needed --noconfirm $packages" 'installing MinGW packages'
}

# 2. QEMU source ------------------------------------------------------------
if (-not (Test-Path (Join-Path $QemuDir 'hw\arm\meson.build'))) {
    Write-Host "==> cloning QEMU $Tag into $QemuDir" -ForegroundColor Cyan
    # core.autocrlf=false: QEMU's configure and scripts must stay LF.
    git clone --depth 1 --branch $Tag --config core.autocrlf=false `
        --config core.symlinks=false `
        https://gitlab.com/qemu-project/qemu.git $QemuDir
    if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }
} else {
    Write-Host "==> using existing QEMU tree $QemuDir" -ForegroundColor Cyan
}

# 3. the machine ------------------------------------------------------------
Write-Host '==> installing the machine sources' -ForegroundColor Cyan
& (Get-GpsmapPython) (Join-Path $repo 'tools\patch_qemu_tree.py') $QemuDir
if ($LASTEXITCODE -ne 0) { throw 'patch_qemu_tree.py failed' }

# 4. configure and build ----------------------------------------------------
# --disable-guest-agent: qga/vss-win32 does not compile against current
# mingw-w64 headers and is of no use to a bare-metal ARM target.
$q = (& $bash -lc "cygpath -u '$QemuDir'").Trim()
$configure = "cd '$q' && mkdir -p build && cd build && " +
    "if [ ! -f build.ninja ]; then ../configure --target-list=arm-softmmu " +
    "--disable-docs --disable-werror --disable-guest-agent --enable-sdl " +
    "--enable-opengl; fi"
Invoke-Msys2 $configure 'configuring'
Invoke-Msys2 "cd '$q/build' && ninja -j $Jobs" 'building (this takes a while)'

# 5. check ------------------------------------------------------------------
$exe = Join-Path $QemuDir 'build\qemu-system-arm.exe'
$env:PATH = (Join-Path $Msys2 'mingw64\bin') + ';' + $env:PATH
$found = & $exe -M help | Select-String gpsmap
if (-not $found) { throw 'the gpsmap7x08 machine is not in the built binary' }
Write-Host ''
Write-Host "built $exe" -ForegroundColor Green
Write-Host "  $found"
Write-Host ''
Write-Host 'Next: tools\win\setup_images.ps1 (firmware images and drives),' -ForegroundColor Yellow
Write-Host '      then tools\win\run_gpsmap.ps1 -Main' -ForegroundColor Yellow

<#
.SYNOPSIS
    Launch the gpsmap7x08 QEMU machine on Windows.

.DESCRIPTION
    A wrapper around tools/run_gpsmap.py, which builds the command line and
    is shared with the Linux/WSL scripts.  Monitor, QMP and the GPS UART are
    TCP sockets on loopback (Windows QEMU has no unix sockets); the tools find
    them through the redirect files in the log directory, so
    `tools\win\qmon.ps1 'info status'` just works.

.PARAMETER Main
    Boot the main image (the GUI firmware) instead of the loader.

.PARAMETER Display
    QEMU -display value.  Default 'sdl,show-cursor=on': a real window you can
    click in (click = touch), with the host mouse pointer visible over it.
    Use 'none' for headless.

.PARAMETER Snapshot
    Restore this VM snapshot at startup (see make_snapshot.ps1).

.PARAMETER GlHooks
    OpenGL ES hook table.  Default fw\gl_hooks.txt when it exists; pass ''
    to disable interception.

.PARAMETER LogDir
    Where serial output, the QEMU log and the socket redirects go.
    Default %TEMP%\gpsmap.

.PARAMETER NoGps
    Do not start the internal GPS module stand-in.

.PARAMETER GlLog
    Log every intercepted OpenGL call (slow).

.PARAMETER QemuArgs
    Everything after -- is passed to QEMU verbatim.

.EXAMPLE
    tools\win\run_gpsmap.ps1 -Main

.EXAMPLE
    tools\win\run_gpsmap.ps1 -Main -Display none -- -d int,unimp,guest_errors
#>
[CmdletBinding()]
param(
    [switch]$Main,
    [string]$Display = 'sdl,show-cursor=on',
    [string]$Snapshot,
    [string]$GlHooks,
    [string]$LogDir,
    [int]$Smp = 1,
    [switch]$NoGps,
    [switch]$GlLog,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$QemuArgs = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$repo = Get-GpsmapRepo

if (-not $PSBoundParameters.ContainsKey('GlHooks')) {
    $default = Join-Path $repo 'fw\gl_hooks.txt'
    $GlHooks = if (Test-Path $default) { 'fw/gl_hooks.txt' } else { '' }
}

$envVars = @{}
if ($Main)     { $envVars['MAIN'] = '1' }
if ($NoGps)    { $envVars['NOGPS'] = '1' }
if ($GlLog)    { $envVars['GARMIN_GL_LOG'] = '1' }
if ($Snapshot) { $envVars['SNAP'] = $Snapshot }
if ($GlHooks)  { $envVars['GLHOOKS'] = $GlHooks }
if ($LogDir)   { $envVars['LOGDIR'] = $LogDir }

$argv = @('-display', $Display, '-smp', "$Smp") + ($QemuArgs | Where-Object { $_ -ne '--' })
Invoke-GpsmapTool -Tool 'run_gpsmap.py' -Arguments $argv -Env $envVars

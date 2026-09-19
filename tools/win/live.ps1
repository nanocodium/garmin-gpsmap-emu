<#
.SYNOPSIS
    Start (or restart) a long-running session with GL interception plus the
    live viewer at http://localhost:8765.

.DESCRIPTION
    Wrapper around tools/live.py.  Both processes are detached, so this returns
    immediately; run it again to restart them.  The default display is an SDL
    window you can click in (click = touch).

.EXAMPLE
    tools\win\live.ps1

.EXAMPLE
    tools\win\live.ps1 -Snapshot gui -Display none
#>
[CmdletBinding()]
param(
    [string]$Display = 'sdl,show-cursor=on',
    [string]$Snapshot,
    [int]$Port = 8765,
    [string]$LogDir,
    [string]$GlHooks = 'fw/gl_hooks.txt'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$argv = @('--display', $Display, '--port', "$Port", '--hooks', $GlHooks)
if ($Snapshot) { $argv += @('--snap', $Snapshot) }
$envVars = @{}
if ($LogDir) { $envVars['LOGDIR'] = $LogDir }
Invoke-GpsmapTool -Tool 'live.py' -Arguments $argv -Env $envVars

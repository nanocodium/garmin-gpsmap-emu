<#
.SYNOPSIS
    Tap the emulated touch panel at a pixel position (1024x600).

.EXAMPLE
    tools\win\tap.ps1 512 470            # "I Agree" on the warning screen
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][int]$X,
    [Parameter(Mandatory = $true, Position = 1)][int]$Y,
    [string]$LogDir,
    [double]$Hold = 0.4
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if (-not $LogDir) { $LogDir = Join-Path $env:TEMP 'gpsmap\live' }
Invoke-GpsmapTool -Tool 'tap.py' -Arguments @(
    "$X", "$Y", '--sock', (Join-Path $LogDir 'qmp.sock'), '--hold', "$Hold")

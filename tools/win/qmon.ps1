<#
.SYNOPSIS
    Send HMP monitor commands to the running machine.

.EXAMPLE
    tools\win\qmon.ps1 'info status' 'info registers'

.EXAMPLE
    tools\win\qmon.ps1 -LogDir $env:TEMP\gpsmap\live 'stop' 'savevm gui'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Commands,
    [Parameter(DontShow = $false)][string]$LogDir,
    [double]$Wait = 1.0
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$argv = @()
if ($LogDir) { $argv += @('--sock', (Join-Path $LogDir 'gpsmap_mon.sock')) }
$argv += @('--wait', "$Wait") + $Commands
Invoke-GpsmapTool -Tool 'qmon.py' -Arguments $argv

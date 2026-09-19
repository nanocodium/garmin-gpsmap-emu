<#
.SYNOPSIS
    Boot the main image, wait for the application layer and save a VM snapshot.

.DESCRIPTION
    Booting to the GUI takes minutes; a snapshot restores in seconds
    (run_gpsmap.ps1 -Snapshot <name>).  Snapshots are stored in fw\sd1.qcow2.
    Rebuild them after changing device models.

.EXAMPLE
    tools\win\make_snapshot.ps1 booted 140
#>
[CmdletBinding()]
param(
    [string]$Name = 'booted',
    [int]$Seconds = 140,
    [string]$LogDir
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$envVars = @{}
if ($LogDir) { $envVars['LOGDIR'] = $LogDir }
Invoke-GpsmapTool -Tool 'make_snapshot.py' -Arguments @($Name, "$Seconds") -Env $envVars

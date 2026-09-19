<#
.SYNOPSIS
    Prepare everything the machine boots from, starting at the update-card zip.

.DESCRIPTION
    Runs, in order:
      tools/gdec_fast.py       decrypt the loader (95.dat) and main (115.dat)
                               firmware images out of the update card
      tools/mkbootcfg.py       the synthetic GPMC CS0 boot-configuration flash
      tools/mkemmc.py          the synthetic 1 GiB eMMC with main installed
      tools/mkdrives.py        the qcow2 drives (snapshot store, eMMC overlay,
                               empty card slots)
      tools/mk_resource_sd.py  the GUI resource card (optional, ~16 GB zip and
                               a few minutes; skip with -NoResources)

    Everything it writes lands in fw\ and is skipped if already present, so
    re-running is cheap.

.PARAMETER Zip
    The Garmin update-card zip.  Default:
    <repo>\GPSMAPSerieswithSDCard_202608031.zip

.EXAMPLE
    tools\win\setup_images.ps1

.EXAMPLE
    tools\win\setup_images.ps1 -Zip D:\downloads\GPSMAPSeries.zip -NoResources
#>
[CmdletBinding()]
param(
    [string]$Zip,
    [switch]$NoResources,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
$repo = Get-GpsmapRepo
$fw = Join-Path $repo 'fw'
if (-not $Zip) { $Zip = Join-Path $repo 'GPSMAPSerieswithSDCard_202608031.zip' }
New-Item -ItemType Directory -Force $fw | Out-Null

function Step([string]$What) { Write-Host "==> $What" -ForegroundColor Cyan }
function Have([string]$Path) {
    (Test-Path $Path) -and -not $Force
}

# 1. firmware images --------------------------------------------------------
$loader = Join-Path $fw 'gpsmap7x08_loader_0x80050000.bin'
$mainfw = Join-Path $fw 'gpsmap7x08_main_0x80050000.bin'
if ((Have $loader) -and (Have $mainfw)) {
    Write-Host "keeping the decrypted firmware images in $fw"
} else {
    if (-not (Test-Path $Zip)) {
        throw @"
No update card zip at $Zip.
It is the Garmin GPSMAP series update download (about 14 GB); pass its path
with -Zip.  Without it there is no firmware to boot.
"@
    }
    Step 'decrypting the loader and main firmware images'
    Invoke-GpsmapTool -Tool 'gdec_fast.py' -Arguments @('--zip', $Zip, '--out', $fw, '95.dat', '115.dat')
}

# 2. boot configuration ----------------------------------------------------
$bootcfg = Join-Path $fw 'bootcfg_cs0.bin'
if (Have $bootcfg) {
    Write-Host "keeping $bootcfg"
} else {
    Step 'building the boot-configuration flash image'
    Invoke-GpsmapTool -Tool 'mkbootcfg.py' -Arguments @($bootcfg)
}

# 3. eMMC ------------------------------------------------------------------
$mnand = Join-Path $fw 'mnand.img'
if (Have $mnand) {
    Write-Host "keeping $mnand"
} else {
    Step 'building the eMMC image with the main region installed'
    Invoke-GpsmapTool -Tool 'mkemmc.py' -Arguments @($mnand, '--main', $mainfw)
}

# 4. GUI resources ---------------------------------------------------------
# Before the drives: mkdrives.py fills the sd0 slot with an empty card if
# nothing is there, and that would shadow the real package.
$res = Join-Path $fw 'sd_resources.qcow2'
if ($NoResources) {
    Write-Host 'skipping the GUI resource card (-NoResources): image handles ' -NoNewline
    Write-Host 'will show the built-in "missing image" bitmap'
} elseif (Have $res) {
    Write-Host "keeping $res"
} else {
    Step 'building the GUI resource card (unpacks ~132 MiB, takes a minute)'
    Invoke-GpsmapTool -Tool 'mk_resource_sd.py' -Arguments @('--zip', $Zip, '--out', $res)
}

# 5. drives ----------------------------------------------------------------
Step 'creating the qcow2 drives'
$drivesArgs = @('--fw', $fw)
if ($Force) { $drivesArgs += '--force' }
Invoke-GpsmapTool -Tool 'mkdrives.py' -Arguments $drivesArgs

Write-Host ''
Write-Host 'fw\ now contains:' -ForegroundColor Green
Get-ChildItem $fw | Sort-Object Name |
    Format-Table Name, @{ n = 'MiB'; e = { [math]::Round($_.Length / 1MB, 1) } } -AutoSize |
    Out-String | Write-Host
Write-Host 'Next: tools\win\run_gpsmap.ps1 -Main' -ForegroundColor Yellow

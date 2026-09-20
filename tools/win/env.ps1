<#
    Shared helpers for the Windows entry points (dot-source this).

    Get-GpsmapPython   a Python 3 interpreter (py -3 launcher, python3, python)
    Get-GpsmapRepo     the repository root
    Invoke-GpsmapTool  run one of tools/*.py with the repo as working directory
#>

function Get-GpsmapRepo {
    (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Get-GpsmapPython {
    # "python" on Windows is often python 2 or a Store stub, so check the
    # version rather than taking the first hit on PATH.
    foreach ($c in @('python3', 'python')) {
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd) {
            $v = & $cmd.Source -c 'import sys; print(sys.version_info[0])' 2>$null
            if ($v -eq '3') { return $cmd.Source }
        }
    }
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        # the launcher: -3 has to be the first argument, so hand back a wrapper
        $script:GpsmapPyLauncher = $py.Source
        return $py.Source
    }
    throw @"
No Python 3 found.  Install it from python.org or the Microsoft Store
(winget install Python.Python.3.13) and re-run.
"@
}

function Invoke-GpsmapTool {
    param(
        [Parameter(Mandatory = $true)][string]$Tool,
        [string[]]$Arguments = @(),
        [switch]$PassThru,
        [hashtable]$Env = @{}
    )
    $repo = Get-GpsmapRepo
    $python = Get-GpsmapPython
    $argv = @()
    if ((Split-Path $python -Leaf) -eq 'py.exe') { $argv += '-3' }
    $argv += (Join-Path $repo ("tools\" + $Tool))
    $argv += $Arguments

    $old = @{}
    foreach ($k in $Env.Keys) {
        $old[$k] = [Environment]::GetEnvironmentVariable($k)
        [Environment]::SetEnvironmentVariable($k, $Env[$k])
    }
    try {
        Push-Location $repo
        if ($PassThru) {
            return Start-Process -FilePath $python -ArgumentList $argv `
                -PassThru -NoNewWindow
        }
        & $python @argv
        if ($LASTEXITCODE -ne 0) {
            throw "tools/$Tool exited with $LASTEXITCODE"
        }
    } finally {
        Pop-Location
        foreach ($k in $old.Keys) {
            [Environment]::SetEnvironmentVariable($k, $old[$k])
        }
    }
}

# Downloads only the build tools' Python (the embeddable package, about 11 MB) into tools\portable, with the code of
# scripts\portable.ps1: the same pinned address, the same SHA-256 check, the same unpacking. The app runs this when the player asks
# for patches before the first build has fetched all the tools; the Python is the one the build uses later. It prints the same
# "@app download <done> <total>" lines as portable.ps1 -ForApp. Exit code 0 = Python is there, 1 = it could not be fetched.
# Plain ASCII only (PowerShell 5.1 reads a file without a byte order mark in the ANSI code page).

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$ProgressPreference = 'SilentlyContinue'

try {
    # Dot-sourcing loads the list of tools and the functions and runs nothing.
    . (Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts\portable.ps1') -ForApp
    $python = $tools | Where-Object { $_.Name -eq 'python' } | Select-Object -First 1
    if (Is-Installed $python) {
        Say "$($python.Title) : already there"
    }
    else {
        New-Item -ItemType Directory -Force -Path $Dir, $archives | Out-Null
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072   # TLS 1.2
        Say "$($python.Title) : $(Mb $python.Size) MB"
        Install-Tool $python
        if ((Test-Path -LiteralPath $archives) -and -not (Get-ChildItem -LiteralPath $archives -Force)) { Remove-Item -LiteralPath $archives }
        Say 'done'
    }
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)"
    exit 1
}

# Sets up a new PC for ps2-native-recomp after cloning this repo: installs the toolchain with
# winget, adds the upstream remote and makes isos\ and wt\. Safe to run again.
#
#   & "$HOME\Desktop\PS2-Recomp\scripts\bootstrap.ps1"            full setup
#   & "...\bootstrap.ps1" -SkipInstall                             only remote + folders

param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [switch]$SkipInstall
)

$ErrorActionPreference = 'Stop'

$upstream = 'https://github.com/snuri00/ps2-web-recomp.git'

$tools = @(
    'MartinStorsjo.LLVM-MinGW.UCRT',  # clang + mingw-w64 + libc++, the Windows toolchain
    'Kitware.CMake',
    'Ninja-build.Ninja',
    'Ccache.Ccache',                  # keeps compiled objects, so worktrees and new build folders compile little
    'Python.Python.3.12',
    'Git.Git',
    'GitHub.cli',
    '7zip.7zip'
)

if (-not $SkipInstall) {
    foreach ($id in $tools) {
        Write-Host "== $id"
        winget list --id $id -e --accept-source-agreements | Out-Null
        if ($LASTEXITCODE -eq 0) { Write-Host '   already installed'; continue }
        winget install --id $id -e --accept-package-agreements --accept-source-agreements
        if ($LASTEXITCODE -ne 0) { throw "winget could not install $id" }
    }
    # Pick up the PATH changes from the installs in this shell.
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path', 'User')
}

# The original project, fetched to merge its updates (never pushed to).
if (-not (git -C $Root remote | Select-String -SimpleMatch -Quiet 'upstream')) {
    git -C $Root remote add upstream $upstream
    git -C $Root remote set-url --push upstream no-push
}
git -C $Root fetch upstream --quiet
if ($LASTEXITCODE -ne 0) { throw 'git fetch upstream failed' }

# Commits of the project owner (the logged-in gh account is exoteric33) go out as exoteric33 with GitHub's private
# address, the same on every PC. Nobody else gets that name, and an author already set for this clone stays.
$login = ''
if (Get-Command gh -ErrorAction SilentlyContinue) {
    $stop = $ErrorActionPreference; $ErrorActionPreference = 'Continue'  # 5.1 turns a native command's stderr into an error
    try { $login = "$(gh api user --jq .login 2>$null)".Trim() } catch { $login = '' }
    $ErrorActionPreference = $stop
}
$name = "$(git -C $Root config --local --get user.name)".Trim()
$email = "$(git -C $Root config --local --get user.email)".Trim()
if ($name -or $email) {
    Write-Host "Git author of this clone stays $name <$email>."
} elseif ($login -eq 'exoteric33') {
    git -C $Root config user.name exoteric33
    git -C $Root config user.email 288114086+exoteric33@users.noreply.github.com
} else {
    Write-Host 'Git author not changed. Set your own if you have not yet: git config --global user.name "Your Name" and git config --global user.email you@example.com'
}

New-Item -ItemType Directory -Force (Join-Path $Root 'isos') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Root 'wt') | Out-Null

Write-Host ''
Write-Host 'Done. Copy your disc images into isos\ by hand, then start Claude Code in'
Write-Host "$Root and say ""continue""."

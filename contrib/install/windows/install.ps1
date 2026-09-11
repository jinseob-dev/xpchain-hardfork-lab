# Install XPChain Core from an extracted release folder (run as Administrator for Program Files).
param(
    [string]$Prefix = "$env:ProgramFiles\XPChain"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

$files = @(
    @{ Src = "xpchain-qt.exe"; Dest = $Prefix },
    @{ Src = "xpchaind.exe"; Dest = Join-Path $Prefix "daemon" },
    @{ Src = "xpchain-cli.exe"; Dest = Join-Path $Prefix "daemon" },
    @{ Src = "xpchain-tx.exe"; Dest = Join-Path $Prefix "daemon" }
)

New-Item -ItemType Directory -Force -Path $Prefix, (Join-Path $Prefix "daemon") | Out-Null
foreach ($f in $files) {
    $srcPath = Join-Path $Root $f.Src
    if (-not (Test-Path $srcPath)) { continue }
    Copy-Item -Force $srcPath (Join-Path $f.Dest (Split-Path $srcPath -Leaf))
}

$startMenu = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs"
New-Item -ItemType Directory -Force -Path $startMenu | Out-Null
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut((Join-Path $startMenu "XPChain Core.lnk"))
$shortcut.TargetPath = Join-Path $Prefix "xpchain-qt.exe"
$shortcut.WorkingDirectory = $Prefix
$shortcut.Save()

$testnetDataDir = Join-Path $env:APPDATA "XPChain-Testnet"
$testnetShortcut = $shell.CreateShortcut((Join-Path $startMenu "XPChain Testnet.lnk"))
$testnetShortcut.TargetPath = Join-Path $Prefix "xpchain-qt.exe"
$testnetShortcut.Arguments = "-testnet -datadir=`"$testnetDataDir`""
$testnetShortcut.WorkingDirectory = $Prefix
$testnetShortcut.Save()

Write-Host "Installed to $Prefix"
Write-Host "Launch: $Prefix\xpchain-qt.exe"
Write-Host "Testnet shortcut: XPChain Testnet"

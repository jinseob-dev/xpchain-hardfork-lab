@echo off
setlocal EnableExtensions
set "SRC=%~dp0"
set "PREFIX=%ProgramFiles%\XPChain"
if not "%~1"=="" set "PREFIX=%~1"

echo Installing XPChain Core to %PREFIX%
if not exist "%PREFIX%" mkdir "%PREFIX%"
if not exist "%PREFIX%\daemon" mkdir "%PREFIX%\daemon"

copy /Y "%SRC%xpchain-qt.exe" "%PREFIX%\" >nul
copy /Y "%SRC%xpchaind.exe" "%PREFIX%\daemon\" >nul
copy /Y "%SRC%xpchain-cli.exe" "%PREFIX%\daemon\" >nul
copy /Y "%SRC%xpchain-tx.exe" "%PREFIX%\daemon\" >nul 2>nul
if exist "%SRC%README-INSTALL.txt" copy /Y "%SRC%README-INSTALL.txt" "%PREFIX%\" >nul

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%ProgramData%\Microsoft\Windows\Start Menu\Programs\XPChain Core.lnk');" ^
  "$s.TargetPath='%PREFIX%\xpchain-qt.exe'; $s.WorkingDirectory='%PREFIX%'; $s.Save()" 2>nul

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%ProgramData%\Microsoft\Windows\Start Menu\Programs\XPChain Testnet.lnk');" ^
  "$s.TargetPath='%PREFIX%\xpchain-qt.exe'; $s.Arguments='-testnet -datadir=^"%APPDATA%\XPChain-Testnet^"'; $s.WorkingDirectory='%PREFIX%'; $s.Save()" 2>nul

echo.
echo Installed. Start menu shortcut: XPChain Core
echo Testnet shortcut: XPChain Testnet
echo Or run: "%PREFIX%\xpchain-qt.exe"
echo Daemon: "%PREFIX%\daemon\xpchaind.exe"
endlocal

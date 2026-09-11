XPChain Core — install from release archive
============================================

Linux (x86_64 or ARM64 tar.gz)
--------------------------------
1. Extract the archive for your CPU and enter its directory.
2. Install:    ./install.sh
3. Add PATH:   export PATH="$HOME/.local/xpchain/bin:$PATH"   (add to ~/.bashrc)
4. Run GUI:    xpchain-qt
5. Testnet:    xpchain-testnet
6. Uninstall:  ./uninstall.sh

macOS (.dmg)
------------
1. Open the .dmg and drag **XPChain-Core.app** to **Applications**.
   For testnet packages, drag **XPChain-Testnet.app** instead.
2. First launch: right-click the app → **Open** (ad-hoc signed builds).
3. CLI tools are inside the .app bundle or use the companion .tar.gz binaries.

Windows (zip or setup.exe)
--------------------------
**Setup.exe (recommended):** run the NSIS installer and follow the wizard.
Then launch **XPChain Core (testnet, 64-bit)** from the Start menu.

**Zip:** extract, then either:
  - Double-click **install.bat** (Administrator), or
  - PowerShell:  .\install.ps1

The testnet launchers use a separate data directory: `%APPDATA%\XPChain-Testnet`
on Windows, `~/.xpchain-testnet` on Linux, and
`~/Library/Application Support/XPChain-Testnet` on macOS.

This preview build is not a formal signed v0.27.0 release (IS_RELEASE=false).

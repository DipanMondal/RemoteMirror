# LightDesk / `ltdesk` installer layer
---
## Build

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build 
```
or
```powershell
envsetup.bat
build.bat
```

Expected outputs:

```text
build\ASUS_Optimization.exe
build\RemoteMirrorViewer.exe
build\ltdesk.exe
```

## Test before installer

From `build\`:

```cmd
ltdesk up
ltdesk status
ltdesk down
```

`ltdesk up` starts `ASUS_Optimization.exe --auto-start --no-taskbar`.

## Build the installer

1. Install Inno Setup.
2. Open `packaging\inno\ltdesk.iss` in Inno Setup Compiler.
3. Compile it after the Release build exists.
4. Run the generated installer as Administrator.
5. Open a **new** Command Prompt and run:

```cmd
ltdesk up
ltdesk down
```

The installer copies `ASUS_Optimization.exe` and `ltdesk.exe` into `Program Files`, adds that folder to the machine PATH, and adds private-network firewall rules for ports 50500, 50510, and 50511.

## Developer Note

1. `LightDeskHostService.exe` — background service, no GUI.
2. `ASUS_Optimization.exe` — user-session GUI/tray app.
3. `ltdesk.exe` — CLI that talks to the service/app.

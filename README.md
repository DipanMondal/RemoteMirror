# RemoteMirror (Light Desk)

An internal LAN screen-mirroring and remote-control tool for Windows. One PC (the
**host / server**) shares its screen and optionally accepts keyboard & mouse
control; another PC (the **viewer / client**) discovers it on the local network,
watches the live screen, and — when granted — drives it.

- **Capture:** DXGI Desktop Duplication (GPU, idle-aware)
- **Video:** H.264 via Media Foundation (low-latency, temporal compression)
- **Transport:** UDP discovery + two TCP channels (control + video), `TCP_NODELAY`
- **Control:** keyboard/mouse access can be granted/revoked from **either side**

> Naming note: the server executable currently builds as `ASUS_Optimization.exe`.

---

## Communication structure

```
+-------------------------+                     +-------------------------+
|        Server PC        |                     |        Viewer PC        |
|   Target / Controlled   |                     |    Client / Controller  |
+-------------------------+                     +-------------------------+
| Server GUI              |                     | Viewer GUI              |
| Turn ON/OFF Server      |                     | Server List             |
| Grant Keyboard / Mouse  |                     | Live Screen Window      |
+------------+------------+                     +------------+------------+
             |                                               |
             | UDP Discovery (port 50500, broadcast)         |
             |<--------------------------------------------->|
             |                                               |
             | TCP Control (port 50510)                      |
             | Hello/OK, Access grants, Input events         |
             |<--------------------------------------------->|
             |                                               |
             | TCP Video (port 50511)                        |
             | FrameHeader + H.264 Annex-B access units      |
             |---------------------------------------------->|
             |                                               |
+------------+------------+                     +------------+------------+
| DXGI Desktop Duplication|                     | Media Foundation        |
| Media Foundation H.264  |                     | H.264 decoder -> BGRA   |
| encoder                 |                     | StretchDIBits renderer  |
+-------------------------+                     +-------------------------+
| Input Injection         |                     | Keyboard/Mouse capture  |
| SendInput (gated)       |                     | Alt+K, Alt+M, Alt+X     |
+-------------------------+                     +-------------------------+
```

---

## Prerequisites

- **Windows 10 or 11**, 64-bit.
  - *Windows "N" editions* need the **Media Feature Pack** installed — the H.264
    Media Foundation codecs are absent otherwise and video init will fail.
- A **GPU/display adapter** that supports DXGI Desktop Duplication (essentially
  all modern Intel/AMD/NVIDIA). Plain RDP sessions don't expose duplication.
- **Visual Studio 2022** or **Build Tools for VS 2022** (MSVC v143, C++ workload).
- **CMake 3.20+**.
- **Windows 10/11 SDK** (ships with the VS C++ workload).
- Both PCs on the **same Layer-2 network / subnet** (so UDP broadcast works).

---

## Build

From a normal terminal (PowerShell or cmd) at the repo root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs land in `build\Release\`:

| Binary                   | Role                                            |
|--------------------------|-------------------------------------------------|
| `ASUS_Optimization.exe`  | **Server** (the host machine)        |
| `RemoteMirrorViewer.exe` | **Viewer** (the controller)                     |
| `PipelineTest.exe`       | Offline encode→decode self-test (no network)    |

> If a build fails with `LNK1104: cannot open file ...exe`, an instance is still
> running — close it (or `Stop-Process -Name ASUS_Optimization,RemoteMirrorViewer`)
> and rebuild.

---

## Run

### 1. On the host (the PC being shared)

1. Run **`ASUS_Optimization.exe`**.
2. Click **Turn ON Server**. The window shows the PC name, IP, and the three
   ports. Status becomes *"ON - Waiting for viewers..."*.
3. Leave it running. The **Grant Keyboard / Grant Mouse** buttons stay disabled
   until a viewer connects.

### 2. On the viewer (the controlling PC)

1. Run **`RemoteMirrorViewer.exe`**.
2. Click **Refresh Servers** — discovered hosts appear in the list.
3. Select the host and click **Connect**. The live screen appears.

### 3. Granting keyboard / mouse control (works from either side)

Access starts **blocked** every session. It can be toggled from **both** ends and
the two stay in sync:

- **From the host:** click **Grant Keyboard** / **Grant Mouse** (click again to
  revoke). The button caption shows `GRANTED` / `blocked`.
- **From the viewer:** press **Alt+K** (keyboard) / **Alt+M** (mouse).
- **Alt+X** on the viewer disconnects.

The on-screen `K: ON/OFF  M: ON/OFF` indicator (top-right of the viewer) reflects
the current grant regardless of which side changed it. Input is injected on the
host **only** while the matching grant is ON.

---

## Testing on another PC (same network)

1. **Open the firewall on the HOST.** The first run usually triggers a Windows
   Firewall prompt — allow it on Private networks. To do it explicitly, run an
   **elevated** terminal on the host:

   ```powershell
   netsh advfirewall firewall add rule name="RemoteMirror UDP 50500" `
     dir=in action=allow protocol=UDP localport=50500
   netsh advfirewall firewall add rule name="RemoteMirror TCP 50510-50511" `
     dir=in action=allow protocol=TCP localport=50510-50511
   ```

   (Remove later with `netsh advfirewall firewall delete rule name="RemoteMirror UDP 50500"` etc.)

2. **Confirm both PCs are on the same subnet** — e.g. both `192.168.1.x`. Run
   `ipconfig` on each. Different subnets or "Guest"/isolated Wi-Fi will block the
   discovery broadcast.

3. On the host: run the server, **Turn ON**.

4. On the other PC: run the viewer → **Refresh Servers** → select → **Connect**.

5. Verify: live screen updates; grant mouse from the host and move/click; grant
   keyboard and type into an app on the host.

**If discovery finds nothing:**

- Re-check the host firewall (step 1) and that the server is **ON**.
- Make sure it's not a Public network profile (broadcast is more restricted).
- Some managed switches / Wi-Fi APs block broadcast or isolate clients — try a
  simpler network or a phone hotspot (below).
- Sanity-check reachability from the viewer PC:

  ```powershell
  Test-NetConnection <HOST_IP> -Port 50510
  ```

---

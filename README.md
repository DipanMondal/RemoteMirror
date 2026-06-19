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
> For an internal, auditable tool you should rename this to something honest
> (see `CMakeLists.txt`). It is called out here so nobody is surprised.

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

| Channel    | Port  | Protocol | Purpose                                  |
|------------|-------|----------|------------------------------------------|
| Discovery  | 50500 | UDP      | Broadcast find-servers + reply           |
| Control    | 50510 | TCP      | Handshake, access grants, input events   |
| Video      | 50511 | TCP      | H.264 frame stream                        |

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
| `ASUS_Optimization.exe`  | **Server** (the controlled/host machine)        |
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

### Quick single-PC smoke test

You can run the server and viewer on the **same** machine: Turn ON the server,
then in the viewer Refresh → Connect to the host's own LAN entry. You'll see your
own screen mirrored. (Granting input control will drive your own machine, so do
that only briefly.) Two machines is the realistic test.

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

## Testing with a mobile device on the same network

There is **no native Android/iOS viewer yet** — the viewer is a Windows app, and
the video is a custom `FrameHeader` + H.264-Annex-B TCP stream. A phone takes part
in two supported ways:

### A) Use the phone as the network (recommended for ad-hoc demos)

1. Enable the phone's **Mobile Hotspot**.
2. Connect **both** Windows PCs to that hotspot's Wi-Fi.
3. Build/run as above. Discovery works because both PCs share the hotspot subnet.
   (The host's network picker even prefers typical private / `192.168.137.x`
   hotspot ranges.)
4. **Caveat:** some phones enable **AP/client isolation**, which blocks PC↔PC
   traffic and broadcast. If discovery fails on a hotspot, that's usually why —
   use a normal Wi-Fi router instead, or a PC-hosted hotspot.

### B) Use the phone to verify the host is reachable

From the phone on the same Wi-Fi (using any free network tool app — e.g. PingTools,
Termux with `nc`, or Fing):

- **Ping** the host IP to confirm L3 reachability.
- **TCP port check** the control port: `nc -vz <HOST_IP> 50510` (Termux) — a
  successful connect means the host is reachable and the firewall is open.

This validates the network path only; it will **not** render the screen.

> Future work: a real mobile client would implement the UDP discovery message,
> the `FrameHeader` framing in `common/protocol.h`, and an H.264 decoder (Android
> `MediaCodec` / iOS `VideoToolbox`).

---

## Automated pipeline self-test

`PipelineTest.exe` exercises the full encode→decode path (DXGI not required): it
feeds synthetic colour frames through the H.264 encoder and decoder and checks the
round-tripped colours. No network, no GUI — good for verifying the Media
Foundation codecs are present on a machine.

```powershell
cmake --build build --config Release --target PipelineTest
.\build\Release\PipelineTest.exe
```

Expected tail:

```
units=12 keyframes=1 decoded_frames=12
PASS: encode/decode pipeline + colour conversion OK
```

A `FAIL: decoder produced no frames` here almost always means the H.264 codec is
missing (Windows N without the Media Feature Pack).

---

## Known limitations / security notes

This is an internal tool and is **not yet hardened**:

- **No encryption** — screen contents *and keystrokes* travel in plaintext TCP.
  Use only on a trusted LAN until TLS is added.
- **No authentication** — any viewer on the network that finds the host can
  connect and watch. Access *grants* (keyboard/mouse) require host or viewer
  consent, but viewing does not.
- **Primary monitor only**; no multi-monitor.
- **No remote cursor overlay** — DXGI captures the desktop without the hardware
  cursor, so the controller doesn't see the host's pointer.
- **One viewer at a time.**

These are the priority items before this is "production" for a security context.

---

## Troubleshooting quick reference

| Symptom                                   | Likely cause / fix                                            |
|-------------------------------------------|---------------------------------------------------------------|
| Viewer finds no servers                   | Host firewall / server not ON / different subnet / AP isolation |
| Connects but black screen                 | Missing H.264 codec (Windows N → Media Feature Pack)          |
| Status shows "GDI fallback"               | Normal — DXGI duplication unavailable (hybrid GPU / RDP); it auto-falls back to GDI capture (works, just more CPU). The shown `DXGI 0x…` code is informational |
| "Screen capture init failed (no DXGI and no GDI)" | Headless/no interactive desktop session                |
| Build error `LNK1104 ... .exe`            | A previous instance is still running — close it               |
| Input granted but nothing happens         | Confirm the grant is ON on **both** the host label and viewer indicator |
```

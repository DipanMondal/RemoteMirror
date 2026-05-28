# RemoteMirror Milestone 1 Build Guide

This milestone builds two Windows GUI applications:

- `RemoteMirrorServer.exe`
- `RemoteMirrorViewer.exe`

## What works in Milestone 1

- Server GUI with ON/OFF power button
- UDP LAN discovery on port `50500`
- Viewer GUI with server list
- Viewer can refresh and find running servers on the same network

## What is not added yet

- TCP control channel
- Screen capture
- Live video streaming
- Keyboard/mouse control

These come in the next milestones.

## Build from CMD

Open a Developer Command Prompt / Native Tools CMD where `cl` is available.

Check:

```cmd
cl
cmake --version
```

Then:

```cmd
cd RemoteMirror_Milestone1
cmake -S . -B build -G "NMake Makefiles"
cmake --build build
```

Run:

```cmd
build\RemoteMirrorServer.exe
build\RemoteMirrorViewer.exe
```

## Test on one PC

1. Start `RemoteMirrorServer.exe`
2. Click `Turn ON Server`
3. Start `RemoteMirrorViewer.exe`
4. Click `Refresh Servers`
5. Your server PC should appear in the list

## Test on two PCs

1. Connect both PCs to the same Wi-Fi/LAN
2. Run server on target PC
3. Turn server ON
4. Run viewer on client PC
5. Click Refresh Servers

If it does not appear, allow the app through Windows Defender Firewall.

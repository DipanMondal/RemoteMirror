# Light Desk
---
## Communication structure
```
+-------------------------+                     +-------------------------+
|        Server PC        |                     |        Viewer PC        |
|   Target / Controlled   |                     |    Client / Controller  |
+-------------------------+                     +-------------------------+
| Server GUI              |                     | Viewer GUI              |
| Start / Stop Button     |                     | Server List             |
| Status: Running/Stopped |                     | Live Screen Window      |
+------------+------------+                     +------------+------------+
             |                                               |
             | UDP Discovery                                 |
             |<--------------------------------------------->|
             |                                               |
             | TCP Control Channel                           |
             | Auth, Input Events, Hotkeys, Ping             |
             |<--------------------------------------------->|
             |                                               |
             | Video Stream Channel                          |
             | Captured Frames / JPEG / H.264                |
             |---------------------------------------------->|
             |                                               |
+------------+------------+                     +------------+------------+
| Screen Capture          |                     | Frame Decoder           |
| DXGI Desktop Duplication|                     | Renderer                |
+-------------------------+                     +-------------------------+
| Input Injection         |                     | Keyboard/Mouse Capture  |
| SendInput               |                     | Alt+K, Alt+M, Alt+X     |
+-------------------------+                     +-------------------------+
```
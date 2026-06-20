#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>


#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "net_utils.h"
#include "capture.h"
#include "encoder.h"
#include "ltdesk_control.h"

#define IDC_POWER_BUTTON    1001
#define IDC_STATUS_LABEL    1002
#define IDC_INFO_LABEL      1003
#define IDC_CLIENT_LABEL    1004
#define IDC_ACCESS_LABEL    1005
#define IDC_KEYBOARD_BUTTON 1006
#define IDC_MOUSE_BUTTON    1007

#define WM_SERVER_STATUS (WM_APP + 1)
#define WM_CLIENT_STATUS (WM_APP + 2)
#define WM_UPDATE_ACCESS (WM_APP + 3)
#define WM_TRAY_ICON    (WM_APP + 10)

#define ID_TRAY_OPEN    2001
#define ID_TRAY_START   2002
#define ID_TRAY_STOP    2003
#define ID_TRAY_EXIT    2004

static HWND g_statusLabel = nullptr;
static HWND g_infoLabel = nullptr;
static HWND g_clientLabel = nullptr;
static HWND g_accessLabel = nullptr;
static HWND g_powerButton = nullptr;
static HWND g_keyboardButton = nullptr;
static HWND g_mouseButton = nullptr;

static bool g_trayIconAdded = false;

static std::atomic_bool g_serverRunning{ false };
static std::atomic_bool g_keyboardAccess{ false };
static std::atomic_bool g_mouseAccess{ false };
static std::atomic_bool g_viewerConnected{ false };

static std::thread g_discoveryThread;
static std::thread g_controlThread;
static std::thread g_videoThread;

static SOCKET g_discoverySocket = INVALID_SOCKET;
static SOCKET g_controlListenSocket = INVALID_SOCKET;
static SOCKET g_controlClientSocket = INVALID_SOCKET;
static SOCKET g_videoListenSocket = INVALID_SOCKET;
static SOCKET g_videoClientSocket = INVALID_SOCKET;

static std::mutex g_socketMutex;


static bool command_line_has_flag(const char* command_line, const char* flag) {
    if (!command_line || !flag) {
        return false;
    }

    return std::string(command_line).find(flag) != std::string::npos;
}

static void show_host_window(HWND hwnd) {
    if (!hwnd) {
        return;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}

static void add_tray_icon(HWND hwnd) {
    if (g_trayIconAdded) {
        return;
    }

    NOTIFYICONDATAA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_ICON;
    nid.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    strcpy_s(nid.szTip, ltdesk::kAppDisplayName);

    if (Shell_NotifyIconA(NIM_ADD, &nid)) {
        g_trayIconAdded = true;
    }
}

static void remove_tray_icon(HWND hwnd) {
    if (!g_trayIconAdded) {
        return;
    }

    NOTIFYICONDATAA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;

    Shell_NotifyIconA(NIM_DELETE, &nid);
    g_trayIconAdded = false;
}

static void show_tray_menu(HWND hwnd) {
    POINT cursor{};
    GetCursorPos(&cursor);

    HMENU menu = CreatePopupMenu();

    if (!menu) {
        return;
    }

    AppendMenuA(menu, MF_STRING, ID_TRAY_OPEN, "Open ASUS_ Optimization");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING | (g_serverRunning.load() ? MF_GRAYED : 0), ID_TRAY_START, "Start server");
    AppendMenuA(menu, MF_STRING | (!g_serverRunning.load() ? MF_GRAYED : 0), ID_TRAY_STOP, "Stop server");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}


static void close_socket_safe(SOCKET& socket_handle) {
    std::lock_guard<std::mutex> lock(g_socketMutex);

    if (socket_handle != INVALID_SOCKET) {
        closesocket(socket_handle);
        socket_handle = INVALID_SOCKET;
    }
}

static void post_status(HWND hwnd, const std::string& text) {
    std::string* copy = new std::string(text);
    PostMessageA(hwnd, WM_SERVER_STATUS, 0, reinterpret_cast<LPARAM>(copy));
}

static void post_client_status(HWND hwnd, const std::string& text) {
    std::string* copy = new std::string(text);
    PostMessageA(hwnd, WM_CLIENT_STATUS, 0, reinterpret_cast<LPARAM>(copy));
}

// Asks the UI thread to refresh the access buttons/label from the current state.
static void post_access_ui(HWND hwnd) {
    PostMessageA(hwnd, WM_UPDATE_ACCESS, 0, 0);
}

// Pushes the host's current grant state down to the connected viewer so its UI
// and input gating match what the host has allowed.
static void send_access_state_to_viewer() {
    std::ostringstream out;
    out << rm::CONTROL_ACCESS_STATE
        << "|keyboard=" << (g_keyboardAccess.load() ? 1 : 0)
        << "|mouse=" << (g_mouseAccess.load() ? 1 : 0)
        << "\n";

    std::string line = out.str();

    std::lock_guard<std::mutex> lock(g_socketMutex);

    if (g_controlClientSocket != INVALID_SOCKET) {
        send(g_controlClientSocket, line.c_str(), static_cast<int>(line.size()), 0);
    }
}

// Runs on the UI thread: button captions reflect grant state, and the buttons
// are only enabled while a viewer is connected.
static void refresh_access_controls() {
    bool connected = g_viewerConnected.load();
    bool keyboard = g_keyboardAccess.load();
    bool mouse = g_mouseAccess.load();

    if (g_keyboardButton) {
        SetWindowTextA(g_keyboardButton,
            keyboard ? "Keyboard: GRANTED  (click to revoke)"
                     : "Keyboard: blocked  (click to grant)");
        EnableWindow(g_keyboardButton, connected ? TRUE : FALSE);
    }

    if (g_mouseButton) {
        SetWindowTextA(g_mouseButton,
            mouse ? "Mouse: GRANTED  (click to revoke)"
                  : "Mouse: blocked  (click to grant)");
        EnableWindow(g_mouseButton, connected ? TRUE : FALSE);
    }

    if (g_accessLabel) {
        std::ostringstream out;
        out << "Keyboard Access: " << (keyboard ? "ON" : "OFF")
            << "    Mouse Access: " << (mouse ? "ON" : "OFF");
        SetWindowTextA(g_accessLabel, out.str().c_str());
    }
}

static bool send_all(SOCKET sock, const char* data, int total_bytes) {
    int sent_total = 0;

    while (sent_total < total_bytes) {
        int sent = send(sock, data + sent_total, total_bytes - sent_total, 0);

        if (sent <= 0) {
            return false;
        }

        sent_total += sent;
    }

    return true;
}

static std::string extract_value(const std::string& text, const std::string& key) {
    std::string token = key + "=";
    size_t start = text.find(token);

    if (start == std::string::npos) {
        return "";
    }

    start += token.size();

    size_t end = text.find('|', start);

    if (end == std::string::npos) {
        return text.substr(start);
    }

    return text.substr(start, end - start);
}

static int to_int_safe(const std::string& value, int default_value = 0) {
    if (value.empty()) {
        return default_value;
    }

    return std::atoi(value.c_str());
}

static bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

static int clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }

    if (value > high) {
        return high;
    }

    return value;
}

static std::string make_discovery_reply() {
    std::ostringstream out;

    out << rm::DISCOVER_REPLY_PREFIX
        << "|name=" << rm::get_hostname()
        << "|ip=" << rm::get_primary_ipv4()
        << "|control_port=" << rm::CONTROL_PORT
        << "|video_port=" << rm::VIDEO_PORT;

    return out.str();
}

static void inject_keyboard_event(int vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);

    if (!down) {
        input.ki.dwFlags = KEYEVENTF_KEYUP;
    }

    SendInput(1, &input, sizeof(INPUT));
}

static void inject_mouse_move(int x, int y, int screen_width, int screen_height) {
    if (screen_width <= 1) {
        screen_width = GetSystemMetrics(SM_CXSCREEN);
    }

    if (screen_height <= 1) {
        screen_height = GetSystemMetrics(SM_CYSCREEN);
    }

    x = clamp_int(x, 0, screen_width - 1);
    y = clamp_int(y, 0, screen_height - 1);

    LONG absolute_x = static_cast<LONG>((static_cast<double>(x) * 65535.0) / (screen_width - 1));
    LONG absolute_y = static_cast<LONG>((static_cast<double>(y) * 65535.0) / (screen_height - 1));

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = absolute_x;
    input.mi.dy = absolute_y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    SendInput(1, &input, sizeof(INPUT));
}

static void inject_mouse_button(const std::string& button, bool down) {
    DWORD flag = 0;

    if (button == "L") {
        flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    } else if (button == "R") {
        flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else if (button == "M") {
        flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }

    if (flag == 0) {
        return;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;

    SendInput(1, &input, sizeof(INPUT));
}

static void inject_mouse_wheel(int delta) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);

    SendInput(1, &input, sizeof(INPUT));
}

static void process_control_line(HWND hwnd, const std::string& line) {
    // Access can be toggled from either end. When the viewer changes it, mirror
    // the new state into the host's gating + UI. We do not echo it back (the
    // viewer is already in that state), which avoids a feedback loop.
    if (starts_with(line, rm::CONTROL_ACCESS_STATE)) {
        int keyboard = to_int_safe(extract_value(line, "keyboard"));
        int mouse = to_int_safe(extract_value(line, "mouse"));

        g_keyboardAccess.store(keyboard == 1);
        g_mouseAccess.store(mouse == 1);

        post_access_ui(hwnd);
        return;
    }

    if (starts_with(line, rm::CONTROL_KEY_EVENT)) {
        if (!g_keyboardAccess.load()) {
            return;
        }

        int vk = to_int_safe(extract_value(line, "vk"));
        int down = to_int_safe(extract_value(line, "down"));

        if (vk > 0) {
            inject_keyboard_event(vk, down == 1);
        }

        return;
    }

    if (starts_with(line, rm::CONTROL_MOUSE_MOVE)) {
        if (!g_mouseAccess.load()) {
            return;
        }

        int x = to_int_safe(extract_value(line, "x"));
        int y = to_int_safe(extract_value(line, "y"));
        int screen_w = to_int_safe(extract_value(line, "screen_w"));
        int screen_h = to_int_safe(extract_value(line, "screen_h"));

        inject_mouse_move(x, y, screen_w, screen_h);
        return;
    }

    if (starts_with(line, rm::CONTROL_MOUSE_BUTTON)) {
        if (!g_mouseAccess.load()) {
            return;
        }

        std::string button = extract_value(line, "button");
        int down = to_int_safe(extract_value(line, "down"));

        inject_mouse_button(button, down == 1);
        return;
    }

    if (starts_with(line, rm::CONTROL_MOUSE_WHEEL)) {
        if (!g_mouseAccess.load()) {
            return;
        }

        int delta = to_int_safe(extract_value(line, "delta"));
        inject_mouse_wheel(delta);
        return;
    }
}

static void discovery_loop(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        g_discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }

    if (g_discoverySocket == INVALID_SOCKET) {
        post_status(hwnd, "Status: Failed to create UDP socket");
        return;
    }

    rm::set_socket_timeout_ms(g_discoverySocket, 500);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(rm::DISCOVERY_PORT);

    if (bind(g_discoverySocket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        post_status(hwnd, "Status: Failed to bind UDP discovery port 50500");
        close_socket_safe(g_discoverySocket);
        return;
    }

    char buffer[1024]{};

    while (g_serverRunning.load()) {
        sockaddr_in client_addr{};
        int client_len = sizeof(client_addr);

        int received = recvfrom(
            g_discoverySocket,
            buffer,
            sizeof(buffer) - 1,
            0,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len
        );

        if (received == SOCKET_ERROR) {
            int error = WSAGetLastError();

            if (error == WSAETIMEDOUT) {
                continue;
            }

            if (!g_serverRunning.load()) {
                break;
            }

            continue;
        }

        buffer[received] = '\0';

        if (std::string(buffer) == rm::DISCOVER_REQUEST) {
            std::string reply = make_discovery_reply();

            sendto(
                g_discoverySocket,
                reply.c_str(),
                static_cast<int>(reply.size()),
                0,
                reinterpret_cast<sockaddr*>(&client_addr),
                client_len
            );
        }
    }

    close_socket_safe(g_discoverySocket);
}

static void handle_connected_client(HWND hwnd, SOCKET client_socket, const std::string& client_ip) {
    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        g_controlClientSocket = client_socket;
    }

    // Flush input events immediately for responsive remote control.
    rm::set_tcp_nodelay(client_socket);

    // New session always starts with nothing granted; the host decides.
    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);
    g_viewerConnected.store(true);

    post_status(hwnd, "Status: ON - Viewer connected");
    post_client_status(hwnd, "Connected Viewer: " + client_ip);
    post_access_ui(hwnd);

    std::string ok = std::string(rm::CONTROL_OK) + "\n";
    send(client_socket, ok.c_str(), static_cast<int>(ok.size()), 0);

    // Tell the viewer the initial (blocked) grant state.
    send_access_state_to_viewer();

    char buffer[2048]{};
    std::string pending;

    while (g_serverRunning.load()) {
        int received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (received <= 0) {
            break;
        }

        buffer[received] = '\0';
        pending += buffer;

        size_t newline_pos = std::string::npos;

        while ((newline_pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline_pos);
            pending.erase(0, newline_pos + 1);

            if (line.find(rm::CONTROL_DISCONNECT) != std::string::npos) {
                close_socket_safe(g_controlClientSocket);

                g_keyboardAccess.store(false);
                g_mouseAccess.store(false);
                g_viewerConnected.store(false);

                if (g_serverRunning.load()) {
                    post_status(hwnd, "Status: ON - Waiting for viewers...");
                    post_client_status(hwnd, "Connected Viewer: None");
                    post_access_ui(hwnd);
                }

                return;
            }

            process_control_line(hwnd, line);
        }
    }

    close_socket_safe(g_controlClientSocket);

    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);
    g_viewerConnected.store(false);

    if (g_serverRunning.load()) {
        post_status(hwnd, "Status: ON - Waiting for viewers...");
        post_client_status(hwnd, "Connected Viewer: None");
        post_access_ui(hwnd);
    }
}

static void control_loop(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        g_controlListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    if (g_controlListenSocket == INVALID_SOCKET) {
        post_status(hwnd, "Status: Failed to create TCP socket");
        return;
    }

    BOOL reuse = TRUE;

    setsockopt(
        g_controlListenSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse),
        sizeof(reuse)
    );

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(rm::CONTROL_PORT);

    if (bind(g_controlListenSocket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        post_status(hwnd, "Status: Failed to bind TCP control port 50510");
        close_socket_safe(g_controlListenSocket);
        return;
    }

    if (listen(g_controlListenSocket, 1) == SOCKET_ERROR) {
        post_status(hwnd, "Status: Failed to listen on TCP control port");
        close_socket_safe(g_controlListenSocket);
        return;
    }

    post_status(hwnd, "Status: ON - Waiting for viewers...");

    while (g_serverRunning.load()) {
        sockaddr_in client_addr{};
        int client_len = sizeof(client_addr);

        SOCKET client_socket = accept(
            g_controlListenSocket,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len
        );

        if (client_socket == INVALID_SOCKET) {
            if (!g_serverRunning.load()) {
                break;
            }

            continue;
        }

        char client_ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, sizeof(client_ip));

        char hello_buffer[256]{};
        int received = recv(client_socket, hello_buffer, sizeof(hello_buffer) - 1, 0);

        if (received <= 0) {
            closesocket(client_socket);
            continue;
        }

        hello_buffer[received] = '\0';

        std::string hello(hello_buffer);

        if (hello.find(rm::CONTROL_HELLO) == std::string::npos) {
            closesocket(client_socket);
            continue;
        }

        handle_connected_client(hwnd, client_socket, client_ip);
    }

    close_socket_safe(g_controlListenSocket);
}

// Captures the screen with DXGI Desktop Duplication and streams H.264 access
// units to a single connected viewer until it disconnects or the server stops.
static void stream_to_client(HWND hwnd, SOCKET client_socket, bool mf_ready) {
    if (!mf_ready) {
        post_status(hwnd, "Status: Media Foundation init failed");
        return;
    }

    rm::ScreenDuplicator capturer;

    if (!capturer.initialize()) {
        post_status(hwnd, "Status: Screen capture init failed (no DXGI and no GDI)");
        return;
    }

    if (capturer.using_gdi()) {
        std::ostringstream mode;
        mode << "Status: ON - streaming (GDI fallback; DXGI 0x"
             << std::hex << std::uppercase
             << static_cast<unsigned long>(capturer.last_error()) << ")";
        post_status(hwnd, mode.str());
    } else {
        post_status(hwnd, "Status: ON - streaming (DXGI)");
    }

    rm::H264Encoder encoder;

    const int target_fps = 30;
    int enc_w = 0;
    int enc_h = 0;

    uint64_t frame_id = 0;
    auto last_keyframe = std::chrono::steady_clock::now();
    bool first_frame = true;

    std::vector<unsigned char> bgra;
    std::vector<std::vector<unsigned char>> units;
    std::vector<bool> keyflags;
    std::vector<char> send_scratch;

    while (g_serverRunning.load()) {
        int width = 0;
        int height = 0;

        rm::ScreenDuplicator::Result result = capturer.acquire(bgra, width, height, 30);

        if (result == rm::ScreenDuplicator::Result::Idle) {
            continue;
        }

        if (result == rm::ScreenDuplicator::Result::Lost) {
            capturer.shutdown();
            if (!capturer.initialize()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            enc_w = 0; // force encoder rebuild on next captured frame
            continue;
        }

        if (result == rm::ScreenDuplicator::Result::Error) {
            break;
        }

        // (Re)create the encoder when the capture resolution changes.
        if (width != enc_w || height != enc_h) {
            long long bitrate = static_cast<long long>(width) * height * target_fps * 7 / 100; // ~0.07 bpp
            bitrate = clamp_int(static_cast<int>(bitrate < 20000000 ? bitrate : 20000000), 1500000, 20000000);

            if (!encoder.initialize(width, height, target_fps, static_cast<int>(bitrate))) {
                post_status(hwnd, "Status: H.264 encoder init failed");
                break;
            }

            enc_w = width;
            enc_h = height;
            first_frame = true;
        }

        auto now = std::chrono::steady_clock::now();
        bool force_key = first_frame ||
            std::chrono::duration_cast<std::chrono::seconds>(now - last_keyframe).count() >= 3;

        units.clear();
        keyflags.clear();

        if (!encoder.encode(bgra.data(), force_key, units, keyflags)) {
            continue; // transient; keep going
        }

        if (force_key && !units.empty()) {
            last_keyframe = now;
            first_frame = false;
        }

        bool send_failed = false;

        for (size_t i = 0; i < units.size(); ++i) {
            const std::vector<unsigned char>& unit = units[i];

            rm::FrameHeader header{};
            header.magic = rm::FRAME_MAGIC;
            header.width = static_cast<uint32_t>(enc_w);
            header.height = static_cast<uint32_t>(enc_h);
            header.screen_width = static_cast<uint32_t>(capturer.desktop_width());
            header.screen_height = static_cast<uint32_t>(capturer.desktop_height());
            header.format = rm::FRAME_FORMAT_H264;
            header.flags = keyflags[i] ? rm::FRAME_FLAG_KEYFRAME : 0u;
            header.payload_size = static_cast<uint32_t>(unit.size());
            header.frame_id = frame_id++;

            // Coalesce header + payload into one send (pairs with TCP_NODELAY).
            send_scratch.resize(sizeof(header) + unit.size());
            std::memcpy(send_scratch.data(), &header, sizeof(header));
            std::memcpy(send_scratch.data() + sizeof(header), unit.data(), unit.size());

            if (!send_all(client_socket, send_scratch.data(), static_cast<int>(send_scratch.size()))) {
                send_failed = true;
                break;
            }
        }

        if (send_failed) {
            break;
        }
    }
}

static void video_loop(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        g_videoListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    if (g_videoListenSocket == INVALID_SOCKET) {
        post_status(hwnd, "Status: Failed to create video socket");
        return;
    }

    BOOL reuse = TRUE;

    setsockopt(
        g_videoListenSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse),
        sizeof(reuse)
    );

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(rm::VIDEO_PORT);

    if (bind(g_videoListenSocket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        post_status(hwnd, "Status: Failed to bind video port 50511");
        close_socket_safe(g_videoListenSocket);
        return;
    }

    if (listen(g_videoListenSocket, 1) == SOCKET_ERROR) {
        post_status(hwnd, "Status: Failed to listen on video port");
        close_socket_safe(g_videoListenSocket);
        return;
    }

    // COM + Media Foundation for the H.264 encode pipeline on this thread.
    bool com_ready = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    bool mf_ready = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    while (g_serverRunning.load()) {
        sockaddr_in client_addr{};
        int client_len = sizeof(client_addr);

        SOCKET client_socket = accept(
            g_videoListenSocket,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len
        );

        if (client_socket == INVALID_SOCKET) {
            if (!g_serverRunning.load()) {
                break;
            }

            continue;
        }

        rm::set_tcp_nodelay(client_socket);

        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            g_videoClientSocket = client_socket;
        }

        stream_to_client(hwnd, client_socket, mf_ready);

        close_socket_safe(g_videoClientSocket);
    }

    if (mf_ready) {
        MFShutdown();
    }

    if (com_ready) {
        CoUninitialize();
    }

    close_socket_safe(g_videoListenSocket);
}

static void start_server(HWND hwnd) {
    if (g_serverRunning.load()) {
        return;
    }

    if (!rm::init_winsock()) {
        MessageBoxA(hwnd, "WinSock startup failed.", ltdesk::kAppDisplayName, MB_ICONERROR);
        return;
    }

    g_serverRunning.store(true);

    SetWindowTextA(g_powerButton, "Turn OFF Server");

    std::ostringstream info;

    info << "PC Name: " << rm::get_hostname()
         << "\r\nIP: " << rm::get_primary_ipv4()
         << "\r\nDiscovery Port: " << rm::DISCOVERY_PORT
         << "\r\nControl Port: " << rm::CONTROL_PORT
         << "\r\nVideo Port: " << rm::VIDEO_PORT;

    SetWindowTextA(g_infoLabel, info.str().c_str());
    SetWindowTextA(g_clientLabel, "Connected Viewer: None");

    g_viewerConnected.store(false);
    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);
    refresh_access_controls();

    g_discoveryThread = std::thread(discovery_loop, hwnd);
    g_controlThread = std::thread(control_loop, hwnd);
    g_videoThread = std::thread(video_loop, hwnd);
}

static void stop_server(HWND hwnd) {
    if (!g_serverRunning.load()) {
        return;
    }

    g_serverRunning.store(false);

    close_socket_safe(g_discoverySocket);
    close_socket_safe(g_controlClientSocket);
    close_socket_safe(g_controlListenSocket);
    close_socket_safe(g_videoClientSocket);
    close_socket_safe(g_videoListenSocket);

    if (g_discoveryThread.joinable()) {
        g_discoveryThread.join();
    }

    if (g_controlThread.joinable()) {
        g_controlThread.join();
    }

    if (g_videoThread.joinable()) {
        g_videoThread.join();
    }

    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);
    g_viewerConnected.store(false);

    SetWindowTextA(g_powerButton, "Turn ON Server");
    SetWindowTextA(g_statusLabel, "Status: OFF");
    SetWindowTextA(g_clientLabel, "Connected Viewer: None");
    refresh_access_controls();

    rm::cleanup_winsock();
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowA(
            "STATIC",
            ltdesk::kAppDisplayName,
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 20, 340, 30,
            hwnd,
            nullptr,
            nullptr,
            nullptr
        );

        g_powerButton = CreateWindowA(
            "BUTTON",
            "Turn ON Server",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            100, 70, 180, 45,
            hwnd,
            reinterpret_cast<HMENU>(IDC_POWER_BUTTON),
            nullptr,
            nullptr
        );

        g_statusLabel = CreateWindowA(
            "STATIC",
            "Status: OFF",
            WS_CHILD | WS_VISIBLE,
            30, 135, 330, 25,
            hwnd,
            reinterpret_cast<HMENU>(IDC_STATUS_LABEL),
            nullptr,
            nullptr
        );

        g_clientLabel = CreateWindowA(
            "STATIC",
            "Connected Viewer: None",
            WS_CHILD | WS_VISIBLE,
            30, 165, 330, 25,
            hwnd,
            reinterpret_cast<HMENU>(IDC_CLIENT_LABEL),
            nullptr,
            nullptr
        );

        g_accessLabel = CreateWindowA(
            "STATIC",
            "Keyboard Access: OFF    Mouse Access: OFF",
            WS_CHILD | WS_VISIBLE,
            30, 195, 330, 25,
            hwnd,
            reinterpret_cast<HMENU>(IDC_ACCESS_LABEL),
            nullptr,
            nullptr
        );

        g_keyboardButton = CreateWindowA(
            "BUTTON",
            "Keyboard: blocked  (click to grant)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            30, 225, 330, 32,
            hwnd,
            reinterpret_cast<HMENU>(IDC_KEYBOARD_BUTTON),
            nullptr,
            nullptr
        );

        g_mouseButton = CreateWindowA(
            "BUTTON",
            "Mouse: blocked  (click to grant)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            30, 262, 330, 32,
            hwnd,
            reinterpret_cast<HMENU>(IDC_MOUSE_BUTTON),
            nullptr,
            nullptr
        );

        g_infoLabel = CreateWindowA(
            "STATIC",
            "PC Name: -\r\nIP: -\r\nDiscovery Port: 50500\r\nControl Port: 50510\r\nVideo Port: 50511",
            WS_CHILD | WS_VISIBLE,
            30, 305, 330, 120,
            hwnd,
            reinterpret_cast<HMENU>(IDC_INFO_LABEL),
            nullptr,
            nullptr
        );

        add_tray_icon(hwnd);

        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_POWER_BUTTON) {
            if (g_serverRunning.load()) {
                stop_server(hwnd);
            } else {
                start_server(hwnd);
            }
        } else if (LOWORD(wparam) == IDC_KEYBOARD_BUTTON) {
            if (g_viewerConnected.load()) {
                g_keyboardAccess.store(!g_keyboardAccess.load());
                send_access_state_to_viewer();
                refresh_access_controls();
            }
        } else if (LOWORD(wparam) == IDC_MOUSE_BUTTON) {
            if (g_viewerConnected.load()) {
                g_mouseAccess.store(!g_mouseAccess.load());
                send_access_state_to_viewer();
                refresh_access_controls();
            }
        } else if (LOWORD(wparam) == ID_TRAY_OPEN) {
            show_host_window(hwnd);
        } else if (LOWORD(wparam) == ID_TRAY_START) {
            start_server(hwnd);
        } else if (LOWORD(wparam) == ID_TRAY_STOP) {
            stop_server(hwnd);
        } else if (LOWORD(wparam) == ID_TRAY_EXIT) {
            SendMessageA(hwnd, WM_CLOSE, 0, 0);
        }

        return 0;

    case WM_SERVER_STATUS: {
        std::string* text = reinterpret_cast<std::string*>(lparam);

        if (text) {
            SetWindowTextA(g_statusLabel, text->c_str());
            delete text;
        }

        return 0;
    }

    case WM_CLIENT_STATUS: {
        std::string* text = reinterpret_cast<std::string*>(lparam);

        if (text) {
            SetWindowTextA(g_clientLabel, text->c_str());
            delete text;
        }

        return 0;
    }

    case WM_UPDATE_ACCESS:
        refresh_access_controls();
        return 0;

    case WM_TRAY_ICON:
        if (lparam == WM_LBUTTONDBLCLK) {
            show_host_window(hwnd);
        } else if (lparam == WM_RBUTTONUP) {
            show_tray_menu(hwnd);
        }

        return 0;

    case WM_CLOSE:
        stop_server(hwnd);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        stop_server(hwnd);
        remove_tray_icon(hwnd);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR command_line, int show_cmd) {
    bool auto_start = command_line_has_flag(command_line, "--auto-start");
    bool no_taskbar = command_line_has_flag(command_line, "--no-taskbar");

    HANDLE single_instance_mutex = CreateMutexA(nullptr, TRUE, ltdesk::kSingleInstanceMutex);

    if (single_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowA(ltdesk::kServerWindowClass, nullptr);

        if (existing) {
            show_host_window(existing);
        }

        CloseHandle(single_instance_mutex);
        return 0;
    }

    const char* class_name = ltdesk::kServerWindowClass;

    WNDCLASSA wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassA(&wc);

    DWORD extended_style = no_taskbar ? WS_EX_TOOLWINDOW : 0;

    HWND hwnd = CreateWindowExA(
        extended_style,
        class_name,
        ltdesk::kAppDisplayName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        400,
        500,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!hwnd) {
        if (single_instance_mutex) {
            CloseHandle(single_instance_mutex);
        }

        return 0;
    }

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);

    if (auto_start) {
        PostMessageA(hwnd, WM_COMMAND, MAKEWPARAM(IDC_POWER_BUTTON, BN_CLICKED), reinterpret_cast<LPARAM>(g_powerButton));
    }

    MSG msg{};

    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (single_instance_mutex) {
        CloseHandle(single_instance_mutex);
    }

    return static_cast<int>(msg.wParam);
}
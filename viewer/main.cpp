#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "net_utils.h"

#define IDC_REFRESH_BUTTON 2001
#define IDC_SERVER_LIST    2002
#define IDC_CONNECT_BUTTON 2003
#define IDC_STATUS_LABEL   2004

#define HOTKEY_DISCONNECT  9001
#define HOTKEY_KEYBOARD    9002
#define HOTKEY_MOUSE       9003

#define WM_ADD_SERVER       (WM_APP + 10)
#define WM_DISCOVERY_DONE   (WM_APP + 11)
#define WM_VIEWER_STATUS    (WM_APP + 12)
#define WM_CONNECTION_STATE (WM_APP + 13)
#define WM_NEW_FRAME        (WM_APP + 14)

static HWND g_serverList = nullptr;
static HWND g_statusLabel = nullptr;
static HWND g_refreshButton = nullptr;
static HWND g_connectButton = nullptr;

static std::atomic_bool g_discoveryRunning{ false };
static std::atomic_bool g_connected{ false };
static std::atomic_bool g_connecting{ false };
static std::atomic_bool g_keyboardAccess{ false };
static std::atomic_bool g_mouseAccess{ false };

static std::thread g_discoveryThread;
static std::thread g_connectionThread;
static std::thread g_videoThread;

static SOCKET g_controlSocket = INVALID_SOCKET;
static SOCKET g_videoSocket = INVALID_SOCKET;

static std::mutex g_socketMutex;
static std::mutex g_frameMutex;

static std::vector<unsigned char> g_latestFrame;
static int g_frameWidth = 0;
static int g_frameHeight = 0;
static int g_remoteScreenWidth = 0;
static int g_remoteScreenHeight = 0;

static RECT g_lastVideoRect{ 10, 45, 890, 590 };

struct ServerInfo {
    std::string name;
    std::string ip;
    std::string display;
    std::string raw;
};

static int clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }

    if (value > high) {
        return high;
    }

    return value;
}

static void post_status(HWND hwnd, const std::string& text) {
    std::string* copy = new std::string(text);
    PostMessageA(hwnd, WM_VIEWER_STATUS, 0, reinterpret_cast<LPARAM>(copy));
}

static bool recv_all(SOCKET sock, char* data, int total_bytes) {
    int received_total = 0;

    while (received_total < total_bytes) {
        int received = recv(sock, data + received_total, total_bytes - received_total, 0);

        if (received <= 0) {
            return false;
        }

        received_total += received;
    }

    return true;
}

static void send_control_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_socketMutex);

    if (g_controlSocket == INVALID_SOCKET) {
        return;
    }

    std::string message = line + "\n";

    send(
        g_controlSocket,
        message.c_str(),
        static_cast<int>(message.size()),
        0
    );
}

static void send_access_state() {
    std::ostringstream out;

    out << rm::CONTROL_ACCESS_STATE
        << "|keyboard=" << (g_keyboardAccess.load() ? 1 : 0)
        << "|mouse=" << (g_mouseAccess.load() ? 1 : 0);

    send_control_line(out.str());
}

static void close_control_socket_safe() {
    std::lock_guard<std::mutex> lock(g_socketMutex);

    if (g_controlSocket != INVALID_SOCKET) {
        shutdown(g_controlSocket, SD_BOTH);
        closesocket(g_controlSocket);
        g_controlSocket = INVALID_SOCKET;
    }
}

static void close_video_socket_safe() {
    std::lock_guard<std::mutex> lock(g_socketMutex);

    if (g_videoSocket != INVALID_SOCKET) {
        shutdown(g_videoSocket, SD_BOTH);
        closesocket(g_videoSocket);
        g_videoSocket = INVALID_SOCKET;
    }
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

static void clear_server_list() {
    int count = static_cast<int>(SendMessageA(g_serverList, LB_GETCOUNT, 0, 0));

    for (int i = 0; i < count; ++i) {
        ServerInfo* info = reinterpret_cast<ServerInfo*>(
            SendMessageA(g_serverList, LB_GETITEMDATA, i, 0)
        );

        if (info && info != reinterpret_cast<ServerInfo*>(LB_ERR)) {
            delete info;
        }
    }

    SendMessageA(g_serverList, LB_RESETCONTENT, 0, 0);
}

static void post_server(HWND hwnd, const std::string& reply, const std::string& sender_ip) {
    std::string name = extract_value(reply, "name");
    std::string ip = extract_value(reply, "ip");

    if (name.empty()) {
        name = "Unknown-PC";
    }

    if (ip.empty() || ip == "Unknown") {
        ip = sender_ip;
    }

    std::ostringstream display;
    display << name << "    " << ip << "    Control: " << rm::CONTROL_PORT;

    ServerInfo* info = new ServerInfo;
    info->name = name;
    info->ip = ip;
    info->display = display.str();
    info->raw = reply;

    PostMessageA(hwnd, WM_ADD_SERVER, 0, reinterpret_cast<LPARAM>(info));
}

static void discovery_loop(HWND hwnd) {
    if (!rm::init_winsock()) {
        PostMessageA(hwnd, WM_DISCOVERY_DONE, 0, 0);
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock == INVALID_SOCKET) {
        rm::cleanup_winsock();
        PostMessageA(hwnd, WM_DISCOVERY_DONE, 0, 0);
        return;
    }

    rm::set_broadcast_enabled(sock);
    rm::set_socket_timeout_ms(sock, 500);

    sockaddr_in broadcast_addr{};
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(rm::DISCOVERY_PORT);
    inet_pton(AF_INET, "255.255.255.255", &broadcast_addr.sin_addr);

    const char* msg = rm::DISCOVER_REQUEST;

    for (int i = 0; i < 3; ++i) {
        sendto(
            sock,
            msg,
            static_cast<int>(strlen(msg)),
            0,
            reinterpret_cast<sockaddr*>(&broadcast_addr),
            sizeof(broadcast_addr)
        );

        char buffer[2048]{};

        for (int j = 0; j < 4; ++j) {
            sockaddr_in sender_addr{};
            int sender_len = sizeof(sender_addr);

            int received = recvfrom(
                sock,
                buffer,
                sizeof(buffer) - 1,
                0,
                reinterpret_cast<sockaddr*>(&sender_addr),
                &sender_len
            );

            if (received == SOCKET_ERROR) {
                int error = WSAGetLastError();

                if (error == WSAETIMEDOUT) {
                    break;
                }

                continue;
            }

            buffer[received] = '\0';

            char sender_ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip, sizeof(sender_ip));

            std::string reply(buffer);

            if (reply.rfind(rm::DISCOVER_REPLY_PREFIX, 0) == 0) {
                post_server(hwnd, reply, sender_ip);
            }
        }
    }

    closesocket(sock);
    rm::cleanup_winsock();

    PostMessageA(hwnd, WM_DISCOVERY_DONE, 0, 0);
}

static void start_discovery(HWND hwnd) {
    if (g_discoveryRunning.load()) {
        return;
    }

    if (g_connected.load() || g_connecting.load()) {
        MessageBoxA(hwnd, "Disconnect first before searching again.", "RemoteMirror Viewer", MB_ICONINFORMATION);
        return;
    }

    clear_server_list();

    SetWindowTextA(g_statusLabel, "Status: Searching servers...");
    EnableWindow(g_refreshButton, FALSE);

    g_discoveryRunning.store(true);
    g_discoveryThread = std::thread(discovery_loop, hwnd);
}

static void finish_discovery() {
    if (g_discoveryThread.joinable()) {
        g_discoveryThread.join();
    }

    g_discoveryRunning.store(false);

    EnableWindow(g_refreshButton, TRUE);
    SetWindowTextA(g_statusLabel, "Status: Discovery complete");
}

static void show_connection_ui(bool connected) {
    ShowWindow(g_refreshButton, connected ? SW_HIDE : SW_SHOW);
    ShowWindow(g_connectButton, connected ? SW_HIDE : SW_SHOW);
    ShowWindow(g_serverList, connected ? SW_HIDE : SW_SHOW);
}

static void disconnect_from_server(HWND hwnd) {
    if (!g_connected.load() && !g_connecting.load()) {
        return;
    }

    send_control_line(rm::CONTROL_DISCONNECT);

    g_connected.store(false);
    g_connecting.store(false);
    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);

    close_video_socket_safe();
    close_control_socket_safe();

    ReleaseCapture();

    post_status(hwnd, "Status: Disconnected");
    PostMessageA(hwnd, WM_CONNECTION_STATE, 0, 0);
}

static void video_receive_loop(HWND hwnd, std::string ip) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (sock == INVALID_SOCKET) {
        post_status(hwnd, "Status: Failed to create video socket");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        g_videoSocket = sock;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(rm::VIDEO_PORT);

    if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) != 1) {
        close_video_socket_safe();
        post_status(hwnd, "Status: Invalid video server IP");
        return;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        close_video_socket_safe();
        post_status(hwnd, "Status: Video connection failed");
        return;
    }

    post_status(hwnd, "Status: Connected - receiving screen feed | Alt+K Keyboard | Alt+M Mouse | Alt+X Disconnect");

    while (g_connected.load()) {
        rm::FrameHeader header{};

        bool header_ok = recv_all(
            sock,
            reinterpret_cast<char*>(&header),
            sizeof(header)
        );

        if (!header_ok) {
            break;
        }

        if (header.magic != rm::FRAME_MAGIC ||
            header.format != rm::FRAME_FORMAT_BGRA ||
            header.width == 0 ||
            header.height == 0 ||
            header.payload_size == 0) {
            break;
        }

        std::vector<unsigned char> payload(header.payload_size);

        bool payload_ok = recv_all(
            sock,
            reinterpret_cast<char*>(payload.data()),
            static_cast<int>(payload.size())
        );

        if (!payload_ok) {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            g_latestFrame = std::move(payload);
            g_frameWidth = static_cast<int>(header.width);
            g_frameHeight = static_cast<int>(header.height);
            g_remoteScreenWidth = static_cast<int>(header.screen_width);
            g_remoteScreenHeight = static_cast<int>(header.screen_height);
        }

        PostMessageA(hwnd, WM_NEW_FRAME, 0, 0);
    }

    close_video_socket_safe();
}

static void connection_loop(HWND hwnd, std::string ip) {
    if (!rm::init_winsock()) {
        g_connecting.store(false);
        post_status(hwnd, "Status: WinSock startup failed");
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (sock == INVALID_SOCKET) {
        g_connecting.store(false);
        rm::cleanup_winsock();
        post_status(hwnd, "Status: Failed to create TCP socket");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        g_controlSocket = sock;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(rm::CONTROL_PORT);

    if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) != 1) {
        close_control_socket_safe();
        g_connecting.store(false);
        rm::cleanup_winsock();
        post_status(hwnd, "Status: Invalid server IP");
        return;
    }

    post_status(hwnd, "Status: Connecting to " + ip + "...");

    if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        close_control_socket_safe();
        g_connecting.store(false);
        rm::cleanup_winsock();
        post_status(hwnd, "Status: Connection failed");
        return;
    }

    std::string hello = std::string(rm::CONTROL_HELLO) + "\n";

    send(
        sock,
        hello.c_str(),
        static_cast<int>(hello.size()),
        0
    );

    char buffer[1024]{};

    int received = recv(sock, buffer, sizeof(buffer) - 1, 0);

    if (received <= 0) {
        close_control_socket_safe();
        g_connecting.store(false);
        rm::cleanup_winsock();
        post_status(hwnd, "Status: Server did not respond");
        return;
    }

    buffer[received] = '\0';

    std::string response(buffer);

    if (response.find(rm::CONTROL_OK) == std::string::npos) {
        close_control_socket_safe();
        g_connecting.store(false);
        rm::cleanup_winsock();
        post_status(hwnd, "Status: Server rejected connection");
        return;
    }

    g_connected.store(true);
    g_connecting.store(false);
    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);

    PostMessageA(hwnd, WM_CONNECTION_STATE, 1, 0);

    send_access_state();

    if (g_videoThread.joinable()) {
        g_videoThread.join();
    }

    g_videoThread = std::thread(video_receive_loop, hwnd, ip);

    while (g_connected.load()) {
        int server_msg = recv(sock, buffer, sizeof(buffer) - 1, 0);

        if (server_msg <= 0) {
            break;
        }
    }

    g_connected.store(false);
    g_connecting.store(false);
    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);

    close_control_socket_safe();
    close_video_socket_safe();

    if (g_videoThread.joinable()) {
        g_videoThread.join();
    }

    post_status(hwnd, "Status: Disconnected");
    PostMessageA(hwnd, WM_CONNECTION_STATE, 0, 0);

    rm::cleanup_winsock();
}

static void connect_to_selected_server(HWND hwnd) {
    if (g_connected.load() || g_connecting.load()) {
        MessageBoxA(hwnd, "Already connected or connecting.", "RemoteMirror Viewer", MB_ICONINFORMATION);
        return;
    }

    int selected = static_cast<int>(SendMessageA(g_serverList, LB_GETCURSEL, 0, 0));

    if (selected == LB_ERR) {
        MessageBoxA(hwnd, "Select a server first.", "RemoteMirror Viewer", MB_ICONINFORMATION);
        return;
    }

    ServerInfo* info = reinterpret_cast<ServerInfo*>(
        SendMessageA(g_serverList, LB_GETITEMDATA, selected, 0)
    );

    if (!info || info == reinterpret_cast<ServerInfo*>(LB_ERR)) {
        MessageBoxA(hwnd, "Invalid server selection.", "RemoteMirror Viewer", MB_ICONERROR);
        return;
    }

    g_connecting.store(true);

    if (g_connectionThread.joinable()) {
        g_connectionThread.join();
    }

    g_connectionThread = std::thread(connection_loop, hwnd, info->ip);
}

static RECT calculate_fit_rect(const RECT& bounds, int image_width, int image_height) {
    RECT result = bounds;

    if (image_width <= 0 || image_height <= 0) {
        return result;
    }

    int bounds_width = bounds.right - bounds.left;
    int bounds_height = bounds.bottom - bounds.top;

    double scale_x = static_cast<double>(bounds_width) / image_width;
    double scale_y = static_cast<double>(bounds_height) / image_height;
    double scale = (scale_x < scale_y) ? scale_x : scale_y;

    int draw_width = static_cast<int>(image_width * scale);
    int draw_height = static_cast<int>(image_height * scale);

    result.left = bounds.left + (bounds_width - draw_width) / 2;
    result.top = bounds.top + (bounds_height - draw_height) / 2;
    result.right = result.left + draw_width;
    result.bottom = result.top + draw_height;

    return result;
}

static bool point_inside_rect(const RECT& rect, int x, int y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

static void send_mouse_move_from_point(int x, int y) {
    if (!g_connected.load() || !g_mouseAccess.load()) {
        return;
    }

    RECT rect = g_lastVideoRect;

    int rect_width = rect.right - rect.left;
    int rect_height = rect.bottom - rect.top;

    if (rect_width <= 1 || rect_height <= 1) {
        return;
    }

    int remote_w = g_remoteScreenWidth;
    int remote_h = g_remoteScreenHeight;

    if (remote_w <= 1 || remote_h <= 1) {
        return;
    }

    int local_x = clamp_int(x - rect.left, 0, rect_width - 1);
    int local_y = clamp_int(y - rect.top, 0, rect_height - 1);

    int remote_x = static_cast<int>((static_cast<double>(local_x) / (rect_width - 1)) * (remote_w - 1));
    int remote_y = static_cast<int>((static_cast<double>(local_y) / (rect_height - 1)) * (remote_h - 1));

    std::ostringstream out;

    out << rm::CONTROL_MOUSE_MOVE
        << "|x=" << remote_x
        << "|y=" << remote_y
        << "|screen_w=" << remote_w
        << "|screen_h=" << remote_h;

    send_control_line(out.str());
}

static void send_mouse_button(const std::string& button, bool down) {
    if (!g_connected.load() || !g_mouseAccess.load()) {
        return;
    }

    std::ostringstream out;

    out << rm::CONTROL_MOUSE_BUTTON
        << "|button=" << button
        << "|down=" << (down ? 1 : 0);

    send_control_line(out.str());
}

static void send_mouse_wheel(int delta) {
    if (!g_connected.load() || !g_mouseAccess.load()) {
        return;
    }

    std::ostringstream out;

    out << rm::CONTROL_MOUSE_WHEEL
        << "|delta=" << delta;

    send_control_line(out.str());
}

static void send_key_event(WPARAM vk, LPARAM, bool down) {
    if (!g_connected.load() || !g_keyboardAccess.load()) {
        return;
    }

    std::ostringstream out;

    out << rm::CONTROL_KEY_EVENT
        << "|vk=" << static_cast<int>(vk)
        << "|down=" << (down ? 1 : 0);

    send_control_line(out.str());
}

static void paint_video(HWND hwnd, HDC hdc) {
    RECT client{};
    GetClientRect(hwnd, &client);

    RECT video_bounds{};
    video_bounds.left = 10;
    video_bounds.top = 45;
    video_bounds.right = client.right - 10;
    video_bounds.bottom = client.bottom - 10;

    HBRUSH black_brush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &video_bounds, black_brush);
    DeleteObject(black_brush);

    std::vector<unsigned char> frame_copy;
    int width = 0;
    int height = 0;

    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        frame_copy = g_latestFrame;
        width = g_frameWidth;
        height = g_frameHeight;
    }

    SetBkMode(hdc, TRANSPARENT);

    if (frame_copy.empty() || width <= 0 || height <= 0) {
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextA(
            hdc,
            "Waiting for video frames...",
            -1,
            &video_bounds,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE
        );

        g_lastVideoRect = video_bounds;
        return;
    }

    RECT draw_rect = calculate_fit_rect(video_bounds, width, height);
    g_lastVideoRect = draw_rect;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(
        hdc,
        draw_rect.left,
        draw_rect.top,
        draw_rect.right - draw_rect.left,
        draw_rect.bottom - draw_rect.top,
        0,
        0,
        width,
        height,
        frame_copy.data(),
        &bmi,
        DIB_RGB_COLORS,
        SRCCOPY
    );

    std::ostringstream indicator;

    indicator << "K: " << (g_keyboardAccess.load() ? "ON" : "OFF")
              << "   M: " << (g_mouseAccess.load() ? "ON" : "OFF")
              << "   Alt+X: Disconnect";

    RECT indicator_rect{};
    indicator_rect.left = client.right - 330;
    indicator_rect.top = 10;
    indicator_rect.right = client.right - 10;
    indicator_rect.bottom = 35;

    SetTextColor(hdc, RGB(0, 0, 0));
    DrawTextA(
        hdc,
        indicator.str().c_str(),
        -1,
        &indicator_rect,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE
    );
}

static void toggle_keyboard_access(HWND hwnd) {
    if (!g_connected.load()) {
        return;
    }

    g_keyboardAccess.store(!g_keyboardAccess.load());

    send_access_state();

    InvalidateRect(hwnd, nullptr, FALSE);
}

static void toggle_mouse_access(HWND hwnd) {
    if (!g_connected.load()) {
        return;
    }

    g_mouseAccess.store(!g_mouseAccess.load());

    if (!g_mouseAccess.load()) {
        ReleaseCapture();
    }

    send_access_state();

    InvalidateRect(hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowA(
            "STATIC",
            "RemoteMirror Viewer",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 15, 500, 30,
            hwnd,
            nullptr,
            nullptr,
            nullptr
        );

        g_refreshButton = CreateWindowA(
            "BUTTON",
            "Refresh Servers",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            30, 60, 150, 35,
            hwnd,
            reinterpret_cast<HMENU>(IDC_REFRESH_BUTTON),
            nullptr,
            nullptr
        );

        g_connectButton = CreateWindowA(
            "BUTTON",
            "Connect",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            190, 60, 100, 35,
            hwnd,
            reinterpret_cast<HMENU>(IDC_CONNECT_BUTTON),
            nullptr,
            nullptr
        );

        g_serverList = CreateWindowA(
            "LISTBOX",
            "",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
            30, 110, 500, 180,
            hwnd,
            reinterpret_cast<HMENU>(IDC_SERVER_LIST),
            nullptr,
            nullptr
        );

        g_statusLabel = CreateWindowA(
            "STATIC",
            "Status: Click Refresh Servers",
            WS_CHILD | WS_VISIBLE,
            30, 310, 800, 25,
            hwnd,
            reinterpret_cast<HMENU>(IDC_STATUS_LABEL),
            nullptr,
            nullptr
        );

        RegisterHotKey(hwnd, HOTKEY_DISCONNECT, MOD_ALT, 'X');
        RegisterHotKey(hwnd, HOTKEY_KEYBOARD, MOD_ALT, 'K');
        RegisterHotKey(hwnd, HOTKEY_MOUSE, MOD_ALT, 'M');

        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_REFRESH_BUTTON) {
            start_discovery(hwnd);
        }

        if (LOWORD(wparam) == IDC_CONNECT_BUTTON) {
            connect_to_selected_server(hwnd);
        }

        return 0;

    case WM_HOTKEY:
        if (wparam == HOTKEY_DISCONNECT) {
            disconnect_from_server(hwnd);
        } else if (wparam == HOTKEY_KEYBOARD) {
            toggle_keyboard_access(hwnd);
        } else if (wparam == HOTKEY_MOUSE) {
            toggle_mouse_access(hwnd);
        }

        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        send_key_event(wparam, lparam, true);
        return g_keyboardAccess.load() ? 0 : DefWindowProcA(hwnd, msg, wparam, lparam);

    case WM_KEYUP:
    case WM_SYSKEYUP:
        send_key_event(wparam, lparam, false);
        return g_keyboardAccess.load() ? 0 : DefWindowProcA(hwnd, msg, wparam, lparam);

    case WM_MOUSEMOVE:
        if (g_connected.load() && g_mouseAccess.load()) {
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);

            if (point_inside_rect(g_lastVideoRect, x, y) || GetCapture() == hwnd) {
                send_mouse_move_from_point(x, y);
            }

            return 0;
        }

        break;

    case WM_LBUTTONDOWN:
        if (g_connected.load() && g_mouseAccess.load()) {
            SetFocus(hwnd);
            SetCapture(hwnd);
            send_mouse_move_from_point(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            send_mouse_button("L", true);
            return 0;
        }

        break;

    case WM_LBUTTONUP:
        if (g_connected.load() && g_mouseAccess.load()) {
            send_mouse_move_from_point(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            send_mouse_button("L", false);
            ReleaseCapture();
            return 0;
        }

        break;

    case WM_RBUTTONDOWN:
        if (g_connected.load() && g_mouseAccess.load()) {
            SetFocus(hwnd);
            SetCapture(hwnd);
            send_mouse_move_from_point(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            send_mouse_button("R", true);
            return 0;
        }

        break;

    case WM_RBUTTONUP:
        if (g_connected.load() && g_mouseAccess.load()) {
            send_mouse_move_from_point(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            send_mouse_button("R", false);
            ReleaseCapture();
            return 0;
        }

        break;

    case WM_MBUTTONDOWN:
        if (g_connected.load() && g_mouseAccess.load()) {
            SetFocus(hwnd);
            SetCapture(hwnd);
            send_mouse_move_from_point(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            send_mouse_button("M", true);
            return 0;
        }

        break;

    case WM_MBUTTONUP:
        if (g_connected.load() && g_mouseAccess.load()) {
            send_mouse_move_from_point(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            send_mouse_button("M", false);
            ReleaseCapture();
            return 0;
        }

        break;

    case WM_MOUSEWHEEL:
        if (g_connected.load() && g_mouseAccess.load()) {
            int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            send_mouse_wheel(delta);
            return 0;
        }

        break;

    case WM_ADD_SERVER: {
        ServerInfo* info = reinterpret_cast<ServerInfo*>(lparam);

        if (info) {
            int count = static_cast<int>(SendMessageA(g_serverList, LB_GETCOUNT, 0, 0));

            bool exists = false;

            for (int i = 0; i < count; ++i) {
                ServerInfo* existing_info = reinterpret_cast<ServerInfo*>(
                    SendMessageA(g_serverList, LB_GETITEMDATA, i, 0)
                );

                if (existing_info && existing_info != reinterpret_cast<ServerInfo*>(LB_ERR)) {
                    if (existing_info->ip == info->ip) {
                        exists = true;
                        break;
                    }
                }
            }

            if (!exists) {
                int index = static_cast<int>(
                    SendMessageA(g_serverList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(info->display.c_str()))
                );

                SendMessageA(g_serverList, LB_SETITEMDATA, index, reinterpret_cast<LPARAM>(info));
            } else {
                delete info;
            }
        }

        return 0;
    }

    case WM_DISCOVERY_DONE:
        finish_discovery();
        return 0;

    case WM_VIEWER_STATUS: {
        std::string* text = reinterpret_cast<std::string*>(lparam);

        if (text) {
            SetWindowTextA(g_statusLabel, text->c_str());
            delete text;
        }

        return 0;
    }

    case WM_CONNECTION_STATE:
        show_connection_ui(wparam == 1);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_NEW_FRAME:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        if (g_connected.load()) {
            paint_video(hwnd, hdc);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        disconnect_from_server(hwnd);

        if (g_discoveryRunning.load()) {
            g_discoveryRunning.store(false);
        }

        finish_discovery();

        if (g_connectionThread.joinable()) {
            g_connectionThread.join();
        }

        if (g_videoThread.joinable()) {
            g_videoThread.join();
        }

        clear_server_list();

        UnregisterHotKey(hwnd, HOTKEY_DISCONNECT);
        UnregisterHotKey(hwnd, HOTKEY_KEYBOARD);
        UnregisterHotKey(hwnd, HOTKEY_MOUSE);

        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        disconnect_from_server(hwnd);

        if (g_discoveryRunning.load()) {
            g_discoveryRunning.store(false);
        }

        finish_discovery();

        if (g_connectionThread.joinable()) {
            g_connectionThread.join();
        }

        if (g_videoThread.joinable()) {
            g_videoThread.join();
        }

        clear_server_list();

        UnregisterHotKey(hwnd, HOTKEY_DISCONNECT);
        UnregisterHotKey(hwnd, HOTKEY_KEYBOARD);
        UnregisterHotKey(hwnd, HOTKEY_MOUSE);

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_cmd) {
    const char* class_name = "RemoteMirrorViewerWindowClass";

    WNDCLASSA wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0,
        class_name,
        "RemoteMirror Viewer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        600,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!hwnd) {
        return 0;
    }

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);

    MSG msg{};

    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return static_cast<int>(msg.wParam);
}
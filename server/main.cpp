#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

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

#define IDC_POWER_BUTTON 1001
#define IDC_STATUS_LABEL 1002
#define IDC_INFO_LABEL   1003
#define IDC_CLIENT_LABEL 1004

#define WM_SERVER_STATUS (WM_APP + 1)
#define WM_CLIENT_STATUS (WM_APP + 2)

static HWND g_statusLabel = nullptr;
static HWND g_infoLabel = nullptr;
static HWND g_clientLabel = nullptr;
static HWND g_powerButton = nullptr;

static std::atomic_bool g_serverRunning{ false };

static std::thread g_discoveryThread;
static std::thread g_controlThread;
static std::thread g_videoThread;

static SOCKET g_discoverySocket = INVALID_SOCKET;
static SOCKET g_controlListenSocket = INVALID_SOCKET;
static SOCKET g_controlClientSocket = INVALID_SOCKET;
static SOCKET g_videoListenSocket = INVALID_SOCKET;
static SOCKET g_videoClientSocket = INVALID_SOCKET;

static std::mutex g_socketMutex;

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

static std::string make_discovery_reply() {
    std::ostringstream out;

    out << rm::DISCOVER_REPLY_PREFIX
        << "|name=" << rm::get_hostname()
        << "|ip=" << rm::get_primary_ipv4()
        << "|control_port=" << rm::CONTROL_PORT
        << "|video_port=" << rm::VIDEO_PORT;

    return out.str();
}

static bool capture_screen_bgra(std::vector<unsigned char>& pixels, int& out_width, int& out_height) {
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);

    if (screen_width <= 0 || screen_height <= 0) {
        return false;
    }

    int target_width = screen_width;
    int target_height = screen_height;

    const int max_width = 960;

    if (screen_width > max_width) {
        target_width = max_width;
        target_height = static_cast<int>((static_cast<double>(screen_height) / screen_width) * target_width);
    }

    HDC screen_dc = GetDC(nullptr);

    if (!screen_dc) {
        return false;
    }

    HDC memory_dc = CreateCompatibleDC(screen_dc);

    if (!memory_dc) {
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = target_width;
    bmi.bmiHeader.biHeight = -target_height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* raw_bits = nullptr;

    HBITMAP bitmap = CreateDIBSection(
        memory_dc,
        &bmi,
        DIB_RGB_COLORS,
        &raw_bits,
        nullptr,
        0
    );

    if (!bitmap || !raw_bits) {
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);

    SetStretchBltMode(memory_dc, HALFTONE);

    BOOL ok = StretchBlt(
        memory_dc,
        0,
        0,
        target_width,
        target_height,
        screen_dc,
        0,
        0,
        screen_width,
        screen_height,
        SRCCOPY
    );

    if (!ok) {
        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }

    size_t data_size = static_cast<size_t>(target_width) * target_height * 4;

    pixels.resize(data_size);
    std::memcpy(pixels.data(), raw_bits, data_size);

    out_width = target_width;
    out_height = target_height;

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);

    return true;
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

    post_status(hwnd, "Status: ON - Viewer connected");
    post_client_status(hwnd, "Connected Viewer: " + client_ip);

    std::string ok = std::string(rm::CONTROL_OK) + "\n";
    send(client_socket, ok.c_str(), static_cast<int>(ok.size()), 0);

    char buffer[1024]{};

    while (g_serverRunning.load()) {
        int received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (received <= 0) {
            break;
        }

        buffer[received] = '\0';
        std::string message(buffer);

        if (message.find(rm::CONTROL_DISCONNECT) != std::string::npos) {
            break;
        }
    }

    close_socket_safe(g_controlClientSocket);

    if (g_serverRunning.load()) {
        post_status(hwnd, "Status: ON - Waiting for viewers...");
        post_client_status(hwnd, "Connected Viewer: None");
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

        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            g_videoClientSocket = client_socket;
        }

        uint64_t frame_id = 0;

        while (g_serverRunning.load()) {
            std::vector<unsigned char> pixels;
            int width = 0;
            int height = 0;

            if (!capture_screen_bgra(pixels, width, height)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            rm::FrameHeader header{};
            header.magic = rm::FRAME_MAGIC;
            header.width = static_cast<uint32_t>(width);
            header.height = static_cast<uint32_t>(height);
            header.format = rm::FRAME_FORMAT_BGRA;
            header.payload_size = static_cast<uint32_t>(pixels.size());
            header.frame_id = frame_id++;

            bool header_ok = send_all(
                client_socket,
                reinterpret_cast<const char*>(&header),
                sizeof(header)
            );

            if (!header_ok) {
                break;
            }

            bool payload_ok = send_all(
                client_socket,
                reinterpret_cast<const char*>(pixels.data()),
                static_cast<int>(pixels.size())
            );

            if (!payload_ok) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        close_socket_safe(g_videoClientSocket);
    }

    close_socket_safe(g_videoListenSocket);
}

static void start_server(HWND hwnd) {
    if (g_serverRunning.load()) {
        return;
    }

    if (!rm::init_winsock()) {
        MessageBoxA(hwnd, "WinSock startup failed.", "RemoteMirror Server", MB_ICONERROR);
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

    SetWindowTextA(g_powerButton, "Turn ON Server");
    SetWindowTextA(g_statusLabel, "Status: OFF");
    SetWindowTextA(g_clientLabel, "Connected Viewer: None");

    rm::cleanup_winsock();
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowA(
            "STATIC",
            "RemoteMirror Server",
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

        g_infoLabel = CreateWindowA(
            "STATIC",
            "PC Name: -\r\nIP: -\r\nDiscovery Port: 50500\r\nControl Port: 50510\r\nVideo Port: 50511",
            WS_CHILD | WS_VISIBLE,
            30, 205, 330, 120,
            hwnd,
            reinterpret_cast<HMENU>(IDC_INFO_LABEL),
            nullptr,
            nullptr
        );

        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_POWER_BUTTON) {
            if (g_serverRunning.load()) {
                stop_server(hwnd);
            } else {
                start_server(hwnd);
            }
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

    case WM_CLOSE:
        stop_server(hwnd);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        stop_server(hwnd);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_cmd) {
    const char* class_name = "RemoteMirrorServerWindowClass";

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
        "RemoteMirror Server",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        400,
        390,
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
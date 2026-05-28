#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
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

#define WM_ADD_SERVER      (WM_APP + 10)
#define WM_DISCOVERY_DONE  (WM_APP + 11)

static HWND g_serverList = nullptr;
static HWND g_statusLabel = nullptr;
static HWND g_refreshButton = nullptr;
static HWND g_connectButton = nullptr;

static std::atomic_bool g_discoveryRunning{ false };
static std::thread g_discoveryThread;

struct ServerInfo {
    std::string display;
    std::string raw;
};

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

    // Send discovery request multiple times to improve reliability.
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

    SendMessageA(g_serverList, LB_RESETCONTENT, 0, 0);
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
            30, 310, 500, 25,
            hwnd,
            reinterpret_cast<HMENU>(IDC_STATUS_LABEL),
            nullptr,
            nullptr
        );
        return 0;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_REFRESH_BUTTON) {
            start_discovery(hwnd);
        }

        if (LOWORD(wparam) == IDC_CONNECT_BUTTON) {
            int selected = static_cast<int>(SendMessageA(g_serverList, LB_GETCURSEL, 0, 0));
            if (selected == LB_ERR) {
                MessageBoxA(hwnd, "Select a server first.", "RemoteMirror Viewer", MB_ICONINFORMATION);
            } else {
                char selected_text[512]{};
                SendMessageA(g_serverList, LB_GETTEXT, selected, reinterpret_cast<LPARAM>(selected_text));

                std::string message = "Selected server:\r\n";
                message += selected_text;
                message += "\r\n\r\nTCP connection and video streaming will be added in Milestone 2.";

                MessageBoxA(hwnd, message.c_str(), "RemoteMirror Viewer", MB_OK | MB_ICONINFORMATION);
            }
        }
        return 0;

    case WM_ADD_SERVER: {
        ServerInfo* info = reinterpret_cast<ServerInfo*>(lparam);
        if (info) {
            // Avoid duplicate visible entries.
            int count = static_cast<int>(SendMessageA(g_serverList, LB_GETCOUNT, 0, 0));
            bool exists = false;

            for (int i = 0; i < count; ++i) {
                char existing[512]{};
                SendMessageA(g_serverList, LB_GETTEXT, i, reinterpret_cast<LPARAM>(existing));
                if (info->display == existing) {
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                SendMessageA(g_serverList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(info->display.c_str()));
            }

            delete info;
        }
        return 0;
    }

    case WM_DISCOVERY_DONE:
        finish_discovery();
        return 0;

    case WM_CLOSE:
        if (g_discoveryRunning.load()) {
            g_discoveryRunning.store(false);
        }
        finish_discovery();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_discoveryRunning.load()) {
            g_discoveryRunning.store(false);
        }
        finish_discovery();
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
        580,
        400,
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

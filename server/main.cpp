#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincodec.h>

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
#define IDC_ACCESS_LABEL 1005

#define WM_SERVER_STATUS (WM_APP + 1)
#define WM_CLIENT_STATUS (WM_APP + 2)
#define WM_ACCESS_STATUS (WM_APP + 3)

static HWND g_statusLabel = nullptr;
static HWND g_infoLabel = nullptr;
static HWND g_clientLabel = nullptr;
static HWND g_accessLabel = nullptr;
static HWND g_powerButton = nullptr;

static std::atomic_bool g_serverRunning{ false };
static std::atomic_bool g_keyboardAccess{ false };
static std::atomic_bool g_mouseAccess{ false };

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

static void post_access_status(HWND hwnd) {
    std::ostringstream out;
    out << "Keyboard Access: " << (g_keyboardAccess.load() ? "ON" : "OFF")
        << "    Mouse Access: " << (g_mouseAccess.load() ? "ON" : "OFF");

    std::string* copy = new std::string(out.str());
    PostMessageA(hwnd, WM_ACCESS_STATUS, 0, reinterpret_cast<LPARAM>(copy));
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

template <typename T>
static void safe_release(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

static bool encode_bgra_to_jpeg(
    const std::vector<unsigned char>& bgra_pixels,
    int width,
    int height,
    float quality,
    std::vector<unsigned char>& jpeg_output
) {
    if (bgra_pixels.empty() || width <= 0 || height <= 0) {
        return false;
    }

    HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool should_uninitialize_com = SUCCEEDED(co_hr);

    if (FAILED(co_hr) && co_hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    bool success = false;

    IWICImagingFactory* factory = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* property_bag = nullptr;

    do {
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)
        );

        if (FAILED(hr)) {
            break;
        }

        hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);

        if (FAILED(hr)) {
            break;
        }

        hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);

        if (FAILED(hr)) {
            break;
        }

        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);

        if (FAILED(hr)) {
            break;
        }

        hr = encoder->CreateNewFrame(&frame, &property_bag);

        if (FAILED(hr)) {
            break;
        }

        if (property_bag) {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");

            VARIANT value{};
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = quality;

            property_bag->Write(1, &option, &value);
            VariantClear(&value);
        }

        hr = frame->Initialize(property_bag);

        if (FAILED(hr)) {
            break;
        }

        hr = frame->SetSize(width, height);

        if (FAILED(hr)) {
            break;
        }

        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;

        hr = frame->SetPixelFormat(&pixel_format);

        if (FAILED(hr)) {
            break;
        }

        std::vector<unsigned char> bgr_pixels;
        bgr_pixels.resize(static_cast<size_t>(width) * height * 3);

        for (int i = 0; i < width * height; ++i) {
            bgr_pixels[i * 3 + 0] = bgra_pixels[i * 4 + 0];
            bgr_pixels[i * 3 + 1] = bgra_pixels[i * 4 + 1];
            bgr_pixels[i * 3 + 2] = bgra_pixels[i * 4 + 2];
        }

        UINT stride = static_cast<UINT>(width * 3);
        UINT image_size = static_cast<UINT>(bgr_pixels.size());

        hr = frame->WritePixels(
            static_cast<UINT>(height),
            stride,
            image_size,
            bgr_pixels.data()
        );

        if (FAILED(hr)) {
            break;
        }

        hr = frame->Commit();

        if (FAILED(hr)) {
            break;
        }

        hr = encoder->Commit();

        if (FAILED(hr)) {
            break;
        }

        STATSTG stat{};
        hr = stream->Stat(&stat, STATFLAG_NONAME);

        if (FAILED(hr)) {
            break;
        }

        HGLOBAL global_memory = nullptr;
        hr = GetHGlobalFromStream(stream, &global_memory);

        if (FAILED(hr) || !global_memory) {
            break;
        }

        SIZE_T jpeg_size = static_cast<SIZE_T>(stat.cbSize.QuadPart);

        void* memory_ptr = GlobalLock(global_memory);

        if (!memory_ptr) {
            break;
        }

        jpeg_output.resize(jpeg_size);
        std::memcpy(jpeg_output.data(), memory_ptr, jpeg_size);

        GlobalUnlock(global_memory);

        success = true;

    } while (false);

    safe_release(property_bag);
    safe_release(frame);
    safe_release(encoder);
    safe_release(stream);
    safe_release(factory);

    if (should_uninitialize_com) {
        CoUninitialize();
    }

    return success;
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
    if (starts_with(line, rm::CONTROL_ACCESS_STATE)) {
        int keyboard = to_int_safe(extract_value(line, "keyboard"));
        int mouse = to_int_safe(extract_value(line, "mouse"));

        g_keyboardAccess.store(keyboard == 1);
        g_mouseAccess.store(mouse == 1);

        post_access_status(hwnd);
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

static bool capture_screen_bgra(
    std::vector<unsigned char>& pixels,
    int& out_width,
    int& out_height,
    int& out_screen_width,
    int& out_screen_height
) {
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);

    if (screen_width <= 0 || screen_height <= 0) {
        return false;
    }

    out_screen_width = screen_width;
    out_screen_height = screen_height;

    int target_width = screen_width;
    int target_height = screen_height;

    const int max_width = 1600;

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

    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);

    post_status(hwnd, "Status: ON - Viewer connected");
    post_client_status(hwnd, "Connected Viewer: " + client_ip);
    post_access_status(hwnd);

    std::string ok = std::string(rm::CONTROL_OK) + "\n";
    send(client_socket, ok.c_str(), static_cast<int>(ok.size()), 0);

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

                if (g_serverRunning.load()) {
                    post_status(hwnd, "Status: ON - Waiting for viewers...");
                    post_client_status(hwnd, "Connected Viewer: None");
                    post_access_status(hwnd);
                }

                return;
            }

            process_control_line(hwnd, line);
        }
    }

    close_socket_safe(g_controlClientSocket);

    g_keyboardAccess.store(false);
    g_mouseAccess.store(false);

    if (g_serverRunning.load()) {
        post_status(hwnd, "Status: ON - Waiting for viewers...");
        post_client_status(hwnd, "Connected Viewer: None");
        post_access_status(hwnd);
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
            int screen_width = 0;
            int screen_height = 0;

            if (!capture_screen_bgra(pixels, width, height, screen_width, screen_height)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            std::vector<unsigned char> jpeg_frame;

			bool jpeg_ok = encode_bgra_to_jpeg(
				pixels,
				width,
				height,
				0.85f,
				jpeg_frame
			);

			if (!jpeg_ok || jpeg_frame.empty()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}

			rm::FrameHeader header{};
			header.magic = rm::FRAME_MAGIC;
			header.width = static_cast<uint32_t>(width);
			header.height = static_cast<uint32_t>(height);
			header.screen_width = static_cast<uint32_t>(screen_width);
			header.screen_height = static_cast<uint32_t>(screen_height);
			header.format = rm::FRAME_FORMAT_JPEG;
			header.payload_size = static_cast<uint32_t>(jpeg_frame.size());
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
				reinterpret_cast<const char*>(jpeg_frame.data()),
				static_cast<int>(jpeg_frame.size())
			);

			if (!payload_ok) {
				break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(66));
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
    SetWindowTextA(g_accessLabel, "Keyboard Access: OFF    Mouse Access: OFF");

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

    SetWindowTextA(g_powerButton, "Turn ON Server");
    SetWindowTextA(g_statusLabel, "Status: OFF");
    SetWindowTextA(g_clientLabel, "Connected Viewer: None");
    SetWindowTextA(g_accessLabel, "Keyboard Access: OFF    Mouse Access: OFF");

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

        g_infoLabel = CreateWindowA(
            "STATIC",
            "PC Name: -\r\nIP: -\r\nDiscovery Port: 50500\r\nControl Port: 50510\r\nVideo Port: 50511",
            WS_CHILD | WS_VISIBLE,
            30, 235, 330, 120,
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

    case WM_ACCESS_STATUS: {
        std::string* text = reinterpret_cast<std::string*>(lparam);

        if (text) {
            SetWindowTextA(g_accessLabel, text->c_str());
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
        420,
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
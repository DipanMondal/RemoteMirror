#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "ltdesk_control.h"

namespace {

HWND find_server_window() {
    return FindWindowA(ltdesk::kServerWindowClass, nullptr);
}

void bring_server_to_front(HWND hwnd) {
    if (!hwnd) {
        return;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}

std::filesystem::path module_directory() {
    wchar_t buffer[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);

    if (len == 0 || len == MAX_PATH) {
        return std::filesystem::current_path();
    }

    return std::filesystem::path(buffer).parent_path();
}

bool wait_for_server_window(HWND& hwnd, int timeout_ms) {
    const int sleep_ms = 100;
    int waited = 0;

    while (waited < timeout_ms) {
        hwnd = find_server_window();

        if (hwnd) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        waited += sleep_ms;
    }

    return false;
}

int command_up() {
    HWND existing = find_server_window();

    if (existing) {
        bring_server_to_front(existing);
        std::cout << "ltdesk: server is already running.\n";
        return 0;
    }

    std::filesystem::path server_path = module_directory() / ltdesk::kServerExeName;

    if (!std::filesystem::exists(server_path)) {
        std::cerr << "ltdesk: cannot find " << server_path.string() << "\n";
        return 2;
    }

    std::wstring command_line = L"\"" + server_path.wstring() + L"\" --auto-start --no-taskbar";
    std::wstring working_dir = server_path.parent_path().wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION process{};

    // CreateProcess may modify the command-line buffer, so pass a mutable copy.
    std::wstring mutable_command_line = command_line;

    BOOL ok = CreateProcessW(
        server_path.c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        working_dir.c_str(),
        &startup,
        &process
    );

    if (!ok) {
        std::cerr << "ltdesk: failed to start server. GetLastError=" << GetLastError() << "\n";
        return 3;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    HWND hwnd = nullptr;

    if (wait_for_server_window(hwnd, 5000)) {
        bring_server_to_front(hwnd);
        std::cout << "ltdesk: server started.\n";
        return 0;
    }

    std::cerr << "ltdesk: server process was launched, but its window was not found.\n";
    return 4;
}

int command_down() {
    HWND hwnd = find_server_window();

    if (!hwnd) {
        std::cout << "ltdesk: server is not running.\n";
        return 0;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    HANDLE process = nullptr;

    if (pid != 0) {
        process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    }

    PostMessageA(hwnd, WM_CLOSE, 0, 0);

    if (process) {
        DWORD wait_result = WaitForSingleObject(process, 10000);
        CloseHandle(process);

        if (wait_result == WAIT_TIMEOUT) {
            std::cerr << "ltdesk: stop request was sent, but the server did not exit in time.\n";
            return 5;
        }
    }

    std::cout << "ltdesk: server stopped.\n";
    return 0;
}

int command_status() {
    HWND hwnd = find_server_window();
    std::cout << "ltdesk: server is " << (hwnd ? "running" : "not running") << ".\n";
    return hwnd ? 0 : 1;
}

void print_usage() {
    std::cout << "LightDesk command helper\n"
              << "Usage:\n"
              << "  ltdesk up      Start ASUS_ Optimization and open the GUI\n"
              << "  ltdesk down    Stop ASUS_ Optimization\n"
              << "  ltdesk status  Check whether ASUS_ Optimization is running\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "up") {
        return command_up();
    }

    if (command == "down") {
        return command_down();
    }

    if (command == "status") {
        return command_status();
    }

    print_usage();
    return 1;
}
